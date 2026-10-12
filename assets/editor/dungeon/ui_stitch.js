/* ui_stitch.js — the DUNGEON page of the Dungeons tab: the stitcher
 * (docs/PLAN_dungeons.md §5.2, package D4).
 *
 * One dungeon (assets/dungeons/<id>/dungeon.json + refs.json) open at a time:
 *
 *   PIECES   drag a piece type from the palette onto an OPEN socket (amber
 *            frame) and it snaps (dungeongen.attachPiece: sized to the socket,
 *            turned to face it, the link written both ways); select a socket
 *            and pick a type to do the same without a drag. A selected piece
 *            shows its slider panel LIVE: every move recompiles the whole
 *            program in this page (the same dungeongen.js the compiler script
 *            runs), installs it in the voxel server and soft-refreshes only
 *            the regions whose prims changed -- coarse while the slider is
 *            held, full on release. A gizmo (or the arrow keys) moves a piece,
 *            R turns it a quarter; links that no longer meet go RED, and
 *            "re-snap" puts it back on its link.
 *   GROW     select an open socket -> Grow (count, branch, seed, kit, types):
 *            the proposal shows GHOSTED (purple boxes, real voxels) until you
 *            Keep, Reroll or Discard it.
 *   ENTITIES the L4 refs (ui_entities.js): mobs, spawners, doors, gates,
 *            levers, plates, traps, keys, containers, regions, waynodes,
 *            markers; signal wires, key threads, patrol routes drawn live;
 *            props (L2) from the prop palette.
 *   VIEW     x-ray (only the dungeon's own surfaces; near walls see-through),
 *            solid (the cut-away rock), In world (the map's real terrain
 *            around the entrance with a hole cut down to the dungeon), a y
 *            range (level slicing), isolate the selected piece, fly (WASD) or
 *            orbit.
 *   CHECKS   the validation panel (dungeongen.validate + the ref rules) is
 *            always live; click an issue to fly to it. The graph overview
 *            draws pieces as nodes in level bands.
 *   UNDO     every change is one step of one history (Ctrl+Z / Ctrl+Y).
 *
 * FRAMES: pieces / props in dungeon.json are in the DUNGEON frame; the view,
 * the refs and the shell are in the ANCHOR frame (entrance at 0,0,0). S.A is
 * the entrance's dungeon-frame position the refs were authored against: when
 * an edit moves the entrance (A changes), the refs move with the rock.
 */
'use strict';
import * as DG from './dungeongen.js';
import {primContains} from './dgutil.js';
import {h, toast, api, Previewer, View3D, History, sliderPanel, drawSocket, socketCentre, COL, css, fmtM, materialNames, makeFrame} from './ui_core.js';
import * as E from './ui_entities.js';
import {readSchedule, writeSchedule} from '../schedules.js';

const ID_RE = /^[a-z0-9_-]{1,48}$/;

export function createStitchPage(T) {
  const S = {
    id: '', d: DG.newDungeon({name: 'Untitled', style: 'mines', seed: 1}), refs: [], about: '', A: [0, 0, 0],
    saved: '', prog: null, placed: [], sockets: [], links: [], broken: new Set(), issues: [], refIssues: [],
    sel: null, tool: null, view: 'xray', ymin: null, ymax: null, isolate: false, world: null, showSug: false, showWay: false, labels: false,
    proposal: null, cat: {mobs: [], schedules: [], loot: [], behaviors: [], keys: [], items: [], presets: {}},
    schedules: {}, schedDirty: new Set(), list: [], drop: null, drag: null, heights: null, infoKey: '', btab: 'valid', showInfo: false,
    grow: {count: 4, branch: 0.3, seed: 1, kit: '', types: null, close: true}
  };
  const pv = new Previewer('stitch');
  const view = new View3D({draw, onClick, capture, onHover});
  const hist = new History(() => ({d: S.d, refs: S.refs, about: S.about, A: S.A, props: S.d.props}),
                           (s) => { S.d = DG.normalizeDungeon(s.d); S.refs = s.refs; S.about = s.about; S.A = s.A; fixSel(); },
                           () => rebuild({voxels: true}));
  const ui = {};
  let built = false, compileT = 0, validT = 0, previewT = 0, materials = null;
  // READINESS (ready(), below): a compile or a preview queued on a timer, or a
  // preview() between its install and its ROI / refresh, is work not yet visible.
  let pendC = false, pendP = false, inPreview = false;

  /* =========================================================================
   * layout
   * ====================================================================== */
  function build() {
    ui.listSel = h('select', {title: 'assets/dungeons/<id>/', onchange: () => open(ui.listSel.value)});
    ui.name = h('input', {type: 'text', class: 'dg-name', title: 'the dungeon\'s display name', onchange: () => edit('rename', () => { S.d.name = ui.name.value.trim() || 'Untitled'; }, null, {voxels: false})});
    ui.style = h('select', {title: 'the default kit (style) of new pieces and of the palette slots', onchange: () => edit('style', () => { S.d.style = ui.style.value; })}, DG.STYLES.map(s => h('option', {value: s}, s)));
    ui.seed = h('input', {type: 'number', title: 'the dungeon seed: every piece without its own seed derives from it', onchange: () => edit('seed', () => { S.d.seed = (+ui.seed.value >>> 0) || 1; })});
    ui.status = h('span', {class: 'st'}, '');
    ui.undo = h('button', {onclick: () => hist.doUndo(), title: 'Ctrl+Z'}, 'Undo');
    ui.redo = h('button', {onclick: () => hist.doRedo(), title: 'Ctrl+Y'}, 'Redo');
    ui.save = h('button', {class: 'dg-pri', onclick: save, title: 'write dungeon.json + shell.json + shell.bin + refs.json (Ctrl+S)'}, 'Save');
    const bar = h('div', {class: 'dg-bar'},
      h('label', {}, 'dungeon ', ui.listSel),
      h('button', {onclick: newDungeon}, 'New'), ui.save, h('button', {onclick: duplicate, title: 'save a copy under a new id'}, 'Duplicate'),
      h('button', {onclick: newVariant, title: 'a VARIANT of this (saved) dungeon: a dungeon.json that extends it and stores only what differs -- changes to the base still reach it'}, 'New variant…'),
      h('button', {onclick: revert, title: 'throw away unsaved changes'}, 'Revert'),
      h('span', {}, '│'), h('label', {}, 'name ', ui.name), h('label', {}, 'style ', ui.style), h('label', {}, 'seed ', ui.seed),
      h('span', {}, '│'), ui.undo, ui.redo,
      h('span', {class: 'grow'}), ui.status,
      h('button', {onclick: playtest, title: 'launch the game at the entrance (the dungeon must be saved and placed on a map)'}, '▶ Playtest'));
    // left: palettes
    ui.palTabs = h('div', {class: 'dg-tabs2'});
    ui.palBody = h('div', {});
    const side = h('div', {class: 'dg-side'}, ui.palTabs, ui.palBody);
    // centre: view + view bar + bottom panel
    ui.ghost = h('div', {class: 'dg-ghostbar', style: 'display:none'});
    ui.empty = h('div', {class: 'dg-empty', style: 'position:absolute;inset:0;display:none;pointer-events:none'});
    view.el.append(ui.ghost, ui.empty);
    ui.vbar = h('div', {class: 'dg-vbar'});
    ui.btabs = h('div', {class: 'dg-btabs'});
    ui.bbody = h('div', {class: 'dg-bbody'});
    ui.bottom = h('div', {class: 'dg-bottom'}, ui.btabs, ui.bbody);
    const center = h('div', {class: 'dg-center'}, view.el, ui.vbar, ui.bottom);
    ui.insp = h('div', {class: 'dg-insp'});
    T.page.append(bar, h('div', {class: 'dg-main dg-stmain'}, side, center, ui.insp));
    buildViewBar();
    bindDrop();
    window.addEventListener('keydown', onKey);
    S.palTab = 'pieces';
    renderPalette();
  }

  function buildViewBar() {
    const mode = (label, m, title) => h('button', {'data-v': m, title, onclick: () => { setViewMode(m); }}, label);
    ui.ymin = h('input', {type: 'range', min: -200, max: 200, step: 1, title: 'level slice: lowest y shown'});
    ui.ymax = h('input', {type: 'range', min: -200, max: 200, step: 1, title: 'level slice: highest y shown'});
    ui.yout = h('span', {style: 'font-family:var(--mono);min-width:120px'}, '');
    const ychg = () => { S.ymin = +ui.ymin.value; S.ymax = +ui.ymax.value; if (S.ymin > S.ymax) [S.ymin, S.ymax] = [S.ymax, S.ymin]; applyClip(); };
    ui.ymin.addEventListener('input', ychg); ui.ymax.addEventListener('input', ychg);
    ui.iso = h('button', {title: 'show only the selected piece', onclick: () => { S.isolate = !S.isolate; ui.iso.classList.toggle('on', S.isolate); applyClip(); }}, 'isolate piece');
    ui.cam = h('button', {title: 'fly (WASD + mouse) or orbit', onclick: () => {
      const wv = view.wv; if (!wv) return;
      if (wv.cam.mode === 'orbit') { wv.cam.pos = wv.camEye(); wv.cam.mode = 'fly'; } else { const f = wv.camForward(); wv.cam.target = [0, 1, 2].map(i => wv.cam.pos[i] + f[i] * 80); wv.cam.dist = 80; wv.cam.mode = 'orbit'; }
      ui.cam.textContent = wv.cam.mode === 'orbit' ? 'orbit' : 'fly'; view.dirty = true; }}, 'orbit');
    const tog = (label, key, title) => { const c = h('input', {type: 'checkbox', checked: S[key], onchange: () => { S[key] = c.checked; view.dirty = true; }}); return h('label', {title}, c, label); };
    ui.worldBox = h('span', {style: 'display:none'});
    ui.vbar.append(mode('x-ray', 'xray', 'only the dungeon\'s own surfaces: rock is see-through'), mode('solid', 'solid', 'the rock block it is carved into'),
                   mode('in world', 'world', 'the map\'s real terrain round the entrance, a hole cut down to the dungeon'), ui.worldBox,
                   h('span', {}, ' y '), ui.ymin, ui.ymax, ui.yout, h('button', {onclick: () => { S.ymin = S.ymax = null; syncY(); applyClip(); }, title: 'all levels'}, 'all'),
                   ui.iso, ui.cam, h('button', {onclick: frameAll, title: 'F'}, 'frame'),
                   tog('labels', 'labels', 'name every ref'), tog('waynodes', 'showWay', 'the generated walking graph'), tog('suggested', 'showSug', 'what the pieces suggest (doors, chests, traps, lights)'));
    for (const b of ui.vbar.querySelectorAll('button[data-v]')) b.classList.toggle('on', b.dataset.v === S.view);
  }
  /** sel: which placement to show in world -- {map, id} (the World map page's
   *  "Open in Dungeons tab" names its own map and entry), else the one already
   *  on screen, else one on the game's map, else the first. */
  function setViewMode(m, sel) {
    if (m === 'world' && !placementOf(sel)) {
      const at = prompt('This dungeon is not placed on a map yet. Preview it at which map column? "x, z" (world voxels) -- or place it on Environment > World map (Dungeons tool).', '0, 0');
      if (!at) return;
      const v = at.split(/[ ,]+/).map(Number);
      if (v.length < 2 || !v.every(isFinite)) return;
      S.world = {map: '', entry: {id: '_scratch', at: [Math.round(v[0]), Math.round(v[1])], yaw: 0, yOffset: 0}, scratch: true};
    } else if (m === 'world') S.world = placementOf(sel);
    S.view = m;
    for (const b of ui.vbar.querySelectorAll('button[data-v]')) b.classList.toggle('on', b.dataset.v === m);
    ui.worldBox.style.display = m === 'world' ? '' : 'none';
    if (m === 'world') renderWorldBox();
    S.infoKey = '';
    S.heights = null;
    schedulePreview(0);
  }
  /** The in-world chip: which placement (any map) is shown, switchable. */
  function renderWorldBox() {
    const e = S.world.entry, P = placementsOf();
    const label = (p) => p.map + ': ' + p.id + ' @ ' + (p.at || []).join(', ') + ' yaw ' + (p.yaw || 0) + (p.yOffset ? ' yOff ' + p.yOffset : '');
    if (P.length > 1 || (P.length && S.world.scratch)) {
      const s = h('select', {title: 'which placement to show: every map that places this dungeon (Environment > World map > Dungeons tool)', onchange: () => {
        if (s.value === '_scratch') { S.world = null; placementOverride = true; setViewMode('world', {map: '\u0000'}); placementOverride = false; return; }
        const [map, id] = s.value.split('\u0001'); setViewMode('world', {map, id});
      }}, ...P.map(p => h('option', {value: p.map + '\u0001' + p.id}, label(p))), h('option', {value: '_scratch'}, 'a scratch column…'));
      s.value = S.world.scratch ? '_scratch' : S.world.map + '\u0001' + e.id;
      ui.worldBox.replaceChildren(s);
    } else {
      ui.worldBox.replaceChildren(h('span', {class: 'dg-chip'}, S.world.scratch ? 'scratch: ' + e.at.join(', ') + ' yaw ' + (e.yaw || 0) + ' yOff ' + (e.yOffset || 0) : label(Object.assign({map: S.world.map}, e))));
    }
  }
  let placementOverride = false;
  function placementsOf() {
    const L = S.list.find(x => x.id === S.id);
    return (L && L.placements) || [];
  }
  function placementOf(sel) {
    if (placementOverride) return null;      // "a scratch column…": ask for one
    const P = placementsOf();
    let p = null;
    if (sel && sel.map) p = P.find(x => x.map === sel.map && (!sel.id || x.id === sel.id)) || null;
    if (!p && S.world && !S.world.scratch) p = P.find(x => x.map === S.world.map && x.id === S.world.entry.id) || null;
    if (!p) p = P.find(x => x.map === S.gameMap) || P[0] || null;
    return p ? {map: p.map, entry: p} : null;
  }

  /* ---- palettes ---- */
  function renderPalette() {
    const tabs = [['pieces', 'Pieces'], ['props', 'Props'], ['entities', 'Entities']];
    ui.palTabs.replaceChildren(...tabs.map(([k, l]) => h('button', {class: S.palTab === k ? 'on' : '', onclick: () => { S.palTab = k; renderPalette(); }}, l)));
    const body = [];
    if (S.palTab === 'pieces') {
      body.push(h('div', {class: 'dg-hint'}, 'Drag a type onto an OPEN socket (amber) in the view, or select a socket and pick a type in the inspector. An empty dungeon takes its first piece anywhere: start with an entrance.'));
      for (const kit of [S.d.style, ...DG.STYLES.filter(k => k !== S.d.style)]) {
        body.push(h('h4', {}, DG.KITS[kit].label || kit));
        for (const t of DG.listTypes(kit)) {
          const presets = T.presets(kit, t.type);
          const sub = h('div', {style: 'display:none;padding-left:12px'}, presets.map(p => palItem({t: 'piece', kit, type: t.type, preset: p.user ? null : p.name, params: p.user ? p.params : null},
            (p.user ? '★ ' : '') + p.name, '')));
          const tw = h('span', {class: 'tag', style: 'cursor:pointer', title: 'presets', onclick: (e) => { e.stopPropagation(); sub.style.display = sub.style.display ? '' : 'none'; }}, '▸ ' + presets.length);
          body.push(palItem({t: 'piece', kit, type: t.type}, t.label, t.surface ? 'surface' : '', tw), sub);
        }
      }
    } else if (S.palTab === 'props') {
      body.push(h('div', {class: 'dg-hint'}, 'Click to arm, then click a floor in the view (Esc ends) -- or drag one in. Lights are props: lantern, brazier, candles, crystal, glow fungus.'));
      for (const kit of DG.STYLES) {
        body.push(h('h4', {}, kit));
        for (const id of DG.PROP_IDS.filter(x => x.startsWith(kit + '/'))) body.push(palItem({t: 'prop', prop: id}, DG.PROPS[id].label || id, /lantern|brazier|candle|crystal|mushroom/.test(id) ? 'light' : ''));
      }
    } else {
      body.push(h('div', {class: 'dg-hint'}, 'Click to arm, then click in the view (Esc ends) -- or drag one in. Wires between levers / plates and what they work are drawn live.'));
      for (const k of E.KIND_ORDER) body.push(palItem({t: 'ref', kind: k}, E.KINDS[k].label, '', null, E.KINDS[k].col));
      body.push(h('h4', {}, 'From the pieces'),
        h('div', {class: 'dg-btns'},
          h('button', {title: 'turn every generated waynode into a waynode ref (patrol routes need refs)', onclick: adoptWaynodes}, 'Adopt waynodes'),
          h('button', {title: 'turn the pieces\' suggested doors, chests, traps and spawners into refs', onclick: () => adoptSuggestions(null)}, 'Adopt suggestions')),
        h('div', {class: 'dg-hint'}, S.prog ? S.prog.refs.length + ' suggestions, ' + S.prog.waynodes.length + ' generated waynodes (tick "suggested" / "waynodes" under the view to see them)' : ''));
    }
    ui.palBody.replaceChildren(...body);
  }
  function palItem(spec, label, tag, extra, col) {
    const armed = S.tool && JSON.stringify(spec) === JSON.stringify(S.tool.spec);
    const it = h('div', {class: 'dg-item' + (armed ? ' sel' : ''), draggable: 'true', 'data-spec': JSON.stringify(spec),
                         onclick: () => arm(spec)},
      col ? h('span', {class: 'sw', style: 'background:' + css(col)}) : null, label, extra || (tag ? h('span', {class: 'tag'}, tag) : null));
    it.addEventListener('dragstart', (e) => { e.dataTransfer.setData('text/plain', JSON.stringify(spec)); e.dataTransfer.effectAllowed = 'copy'; S.dragSpec = spec; });
    it.addEventListener('dragend', () => { S.dragSpec = null; S.drop = null; view.el.classList.remove('dg-drop'); view.dirty = true; });
    return it;
  }
  function arm(spec) {
    if (spec.t === 'piece') {
      if (S.sel && S.sel.t === 'socket') { attachAt({piece: S.sel.piece, socket: S.sel.socket}, spec); return; }
      if (!S.d.pieces.length) { firstPiece(spec); return; }
      toast('select an OPEN socket first (click an amber frame), or drag the type onto one');
      return;
    }
    S.tool = S.tool && JSON.stringify(S.tool.spec) === JSON.stringify(spec) ? null : {spec};
    renderPalette();
    T.setStatus(S.tool ? 'placing ' + (spec.kind || spec.prop) + ': click a floor or wall in the view (Esc ends)' : '');
  }

  /* =========================================================================
   * edits, history, the rebuild pipeline
   * ====================================================================== */
  function edit(label, fn, key, opts) {
    hist.push(label, key);
    fn();
    rebuild(opts || {voxels: true});
  }
  function sig() { return DG.dungeonJsonText(S.d) + '\n' + E.writeRefs(S.id || 'x', S.about, S.refs); }
  function dirty() { return sig() !== S.saved; }
  function rebuild(opts) {
    opts = opts || {};
    clearTimeout(compileT);
    pendC = true;
    compileT = setTimeout(() => {
      pendC = false;
      compileNow();
      if (opts.voxels !== false) schedulePreview(opts.fast ? 30 : 120);
      scheduleValidate();
      renderInspector();
      renderBottom();
      renderHeader();
      view.dirty = true;
    }, opts.fast ? 0 : 10);
  }
  /** The dungeon the view shows: the proposal applied when there is one. */
  function shown() { return S.proposal && S.proposal.result.pieces.length ? DG.applyProposal(S.d, S.proposal.result) : S.d; }
  function compileNow() {
    const d = shown();
    const t0 = performance.now();
    try {
      S.prog = DG.compile(d, {id: S.id || '_new'});
      S.dn = DG.normalizeDungeon(d);
      S.placed = S.dn.pieces.map(p => DG.placePiece(S.dn, p));
    } catch (e) { T.setStatus('compile failed: ' + e.message, true); return; }
    S.compileMs = performance.now() - t0;
    const A = S.prog.entrance.dungeonPos;
    if (!S.proposal && (A[0] !== S.A[0] || A[1] !== S.A[1] || A[2] !== S.A[2])) {
      // The entrance moved in the dungeon frame: the anchor frame slides, and the
      // refs (authored in it) must slide with the rock.
      const dv = [S.A[0] - A[0], S.A[1] - A[1], S.A[2] - A[2]];
      for (const r of S.refs) r.pos = r.pos.map((v, i) => v + dv[i]);
      S.A = A.slice();
    }
    // socket states
    const res = DG.resolveLinks(S.dn, S.placed);
    S.links = res.links;
    S.broken = new Set();
    for (const L of res.links) if (!L.compat.ok) { S.broken.add(L.a.piece + '#' + L.a.name); S.broken.add(L.b.piece + '#' + L.b.name); }
    for (const p of S.dn.pieces) for (const sn of Object.keys(p.links || {})) {
      const L = p.links[sn];
      const ok = res.links.some(x => !x.auto && ((x.a.piece === p.id && x.a.name === sn && x.b.piece === L.piece && x.b.name === L.socket) || (x.b.piece === p.id && x.b.name === sn && x.a.piece === L.piece && x.a.name === L.socket)));
      if (!ok) S.broken.add(p.id + '#' + sn);
    }
    const linked = new Set();
    for (const L of res.links) if (L.compat.ok) { linked.add(L.a.piece + '#' + L.a.name); linked.add(L.b.piece + '#' + L.b.name); }
    const off = S.prog.entrance.dungeonPos;
    S.sockets = DG.socketsOf(S.dn, S.placed).map(s => Object.assign({}, s, {pos: [s.pos[0] - off[0], s.pos[1] - off[1], s.pos[2] - off[2]],
      state: S.broken.has(s.piece + '#' + s.name) ? 'broken' : linked.has(s.piece + '#' + s.name) ? 'linked' : 'open'}));
    S.wayNames = new Map(S.prog.waynodes.map(w => [w.name, w]));
  }
  function schedulePreview(ms) {
    clearTimeout(previewT);
    pendP = true;
    previewT = setTimeout(() => { pendP = false; preview(); }, ms === undefined ? 120 : ms);
  }
  async function preview() {
    inPreview = true;
    try { await previewInner(); } finally { inPreview = false; }
  }
  /** THE ROI of the in-world view: the dungeon's bounds + a terrain margin,
   *  and up to the real ground over its footprint (+ a little sky) once the
   *  heights are known -- not a fixed 11 m slab of sky over a 150 m dungeon. */
  const WORLD_PAD = 96;
  function worldRoi() {
    const b = S.prog.bounds;
    let top = Math.max(b.max[1], 0) + 64;
    if (S.heights) {
      let g = -Infinity;
      const st = 16;
      for (let x = b.min[0] - WORLD_PAD; x <= b.max[0] + WORLD_PAD; x += st)
        for (let z = b.min[2] - WORLD_PAD; z <= b.max[2] + WORLD_PAD; z += st) {
          const w = S.heights.frame.pt([x + 0.5, 0, z + 0.5]), r = S.heights.hm.at(w[0], w[2]);
          if (r) g = Math.max(g, r.ground - S.heights.frame.origin[1], r.water === null ? -Infinity : r.water - S.heights.frame.origin[1]);
        }
      if (isFinite(g)) top = Math.max(b.max[1], g) + 32;
    }
    return {min: [b.min[0] - WORLD_PAD, b.min[1] - 24, b.min[2] - WORLD_PAD], max: [b.max[0] + WORLD_PAD, top, b.max[2] + WORLD_PAD]};
  }
  /** In world: the ground the WorldView culls buried rock by (the margin
   *  columns' deep regions are synthesised, never fetched), and the box it
   *  must NOT cull (the dungeon + the walls of the cut-away hole). */
  function worldCull() {
    const wv = view.wv;
    if (!wv) return;
    if (S.view !== 'world' || !S.heights || !S.prog) { wv.groundAt = null; wv.view.keepBox = null; return; }
    const H = S.heights;
    wv.groundAt = (x, z) => { const r = H.hm.at(x, z); return r ? Math.max(r.ground, r.water === null ? -Infinity : r.water) : null; };
    const b = S.prog.bounds, k = view.frame.box([b.min[0] - 48, b.min[1] - 48, b.min[2] - 48], [b.max[0] + 48, b.max[1] + 48, b.max[2] + 48]);
    wv.view.keepBox = {min: k.min.map(Math.floor), max: k.max.map(Math.ceil)};
  }
  /** READY: the program on screen IS the current one -- compiled, installed in
   *  the voxel server, its regions streamed, meshed and refined to the last
   *  pass. (The old harness test -- "the view's regions are ready" -- passed
   *  on the PREVIOUS program's regions while the new one was still being
   *  installed.) */
  function ready() {
    return !!S.prog && !pendC && !pendP && !inPreview && pv.holds(S.prog) && view.settled();
  }
  async function previewInner() {
    if (!T.isActive() || !S.prog) return;
    if (!S.d.pieces.length && !(S.proposal)) { ui.empty.style.display = ''; ui.empty.textContent = emptyText(); view.dirty = true; return; }
    ui.empty.style.display = 'none';
    if (!view.ensure()) { T.setStatus(view.error, true); return; }
    view.previewer = pv;
    // Coarse first, then refine, in world (the expensive view: a 150 m
    // dungeon + its terrain); the isolated x-ray stays one pass.
    view.progressive = S.view === 'world' ? [0.3, 0.6, 1.0] : null;
    const place = S.view === 'world' && S.world
      ? (S.world.scratch || S.world.map === '' ? {mode: 'place', at: S.world.entry.at, yOffset: S.world.entry.yOffset || 0, yaw: S.world.entry.yaw || 0}
                                                 : {mode: 'map', id: S.world.entry.id, map: S.world.map})
      : {mode: 'isolate'};
    // Which map the voxel server generates: a map placement's own (DUNGEON MAP
    // replaces that map's entry), else tuning's world.mapLayer. The regions must
    // be asked of the same map or they come back without the program.
    view.wv.mapName = place.mode === 'map' ? (place.map || '') : '';
    const slow = setTimeout(() => T.setStatus('installing the program in the voxel server: it is waiting for the machine-wide GPU lock (another sandvox run holds it)'), 1500);
    try {
      const r = await pv.install(S.prog, place);
      clearTimeout(slow);
      if (!r) return;
      T.setStatus('');
      S.info = r.info;
      const key = JSON.stringify(place);
      const moved = view.setFrame(r.info.origin, r.info.yaw || 0);
      const b = S.prog.bounds;
      if (S.view === 'world') { const R = worldRoi(); view.setRoi(R.min, R.max, 0); }
      else view.setRoi(b.min, b.max, 24);
      applyClip();
      worldCull();
      if (moved || key !== S.infoKey) { view.invalidate(); if (key !== S.infoKey || !S.framed) { frameAll(); S.framed = true; } S.infoKey = key; S.heights = null; worldCull(); maybeHeights(); }
      else view.refresh(r.boxes);
      view.extraStatus = S.prog.prims.length + ' prims · compile ' + (S.compileMs || 0).toFixed(0) + ' ms' + (r.boxes === 'all' ? '' : ' · refreshed ' + r.boxes.length + ' box(es)');
    } catch (e) {
      clearTimeout(slow);
      T.setStatus('preview failed: ' + e.message, true);
      view.extraStatus = 'preview failed: ' + e.message;
    }
  }
  function emptyText() {
    return 'An empty dungeon. Drag a piece type from the palette into this view (an entrance: caves "Cave mouth", mines "Adit entrance", ruins "Ruin entrance"), then drag more onto its open sockets.';
  }
  function applyClip() {
    if (!S.prog) return;
    const b = S.prog.bounds;
    let box = {min: b.min.map(v => v - 2), max: b.max.map(v => v + 2)};
    if (S.isolate && S.sel && (S.sel.t === 'piece' || S.sel.t === 'socket')) {
      const pc = S.prog.pieces.find(p => p.id === (S.sel.id || S.sel.piece));
      if (pc) box = {min: pc.bounds.min.map(v => v - 2), max: pc.bounds.max.map(v => v + 2)};
    }
    const lo = S.ymin === null ? -Infinity : S.ymin, hi = S.ymax === null ? Infinity : S.ymax;
    if (S.view === 'world') view.setClip('cut', box, lo, S.ymax === null ? -6 : hi);
    else view.setClip(S.view === 'solid' ? 'solid' : 'xray', box, lo, hi);
    syncY();
  }
  function syncY() {
    if (!S.prog) return;
    const b = S.prog.bounds;
    for (const r of [ui.ymin, ui.ymax]) { r.min = b.min[1] - 2; r.max = b.max[1] + 2; }
    ui.ymin.value = S.ymin === null ? b.min[1] - 2 : S.ymin;
    ui.ymax.value = S.ymax === null ? b.max[1] + 2 : S.ymax;
    ui.yout.textContent = S.ymin === null && S.ymax === null ? 'all levels' : 'y ' + fmtM(+ui.ymin.value) + ' .. ' + fmtM(+ui.ymax.value);
  }
  function frameAll() {
    if (!S.prog) return;
    const b = S.prog.bounds;
    view.frameBox(b.min, b.max, {k: 1.4});
  }

  /* ---- validation ---- */
  function scheduleValidate() { clearTimeout(validT); validT = setTimeout(validateNow, 250); }
  function validateNow() {
    let issues = [];
    try {
      const opts = {refs: E.forValidate(S.refs, S.cat), materials};
      if (S.heights) {
        opts.groundAt = (x, z) => { const w = S.heights.frame.pt([x + 0.5, 0, z + 0.5]); const r = S.heights.hm.at(w[0], w[2]); return r ? r.ground - S.heights.frame.origin[1] : Infinity; };
        opts.waterAt = (x, z) => { const w = S.heights.frame.pt([x + 0.5, 0, z + 0.5]); const r = S.heights.hm.at(w[0], w[2]); return r && r.water !== null ? r.water - S.heights.frame.origin[1] : null; };
      }
      issues = DG.validate(S.d, opts);
    } catch (e) { issues = [{severity: 'error', code: 'validate', piece: null, msg: 'validate threw: ' + e.message}]; }
    const A = S.A;
    for (const v of issues) if (v.at && v.code !== 'breach' && v.code !== 'water') v.at = [v.at[0] - A[0], v.at[1] - A[1], v.at[2] - A[2]];
    const wn = new Set(S.prog ? S.prog.waynodes.map(w => w.name) : []);
    S.issues = issues.concat(E.refProblems(S.refs, S.cat, wn));
    if (!S.heights) S.issues.push({severity: 'info', code: 'ground', piece: null, msg: 'surface / water breach is checked against the real ground once the dungeon is placed (In world, or a map placement)'});
    renderBottom();
    renderHeader();
    view.dirty = true;
  }
  async function maybeHeights() {
    if (!S.info || S.view !== 'world' || !S.prog) return;
    const b = S.prog.bounds, f = makeFrame(S.info.origin, S.info.yaw || 0);
    const wb = f.box(b.min, b.max);
    const span = Math.max(wb.max[0] - wb.min[0], wb.max[2] - wb.min[2]) + 2 * WORLD_PAD + 64;   // the in-world ROI's margin too (worldRoi, worldCull)
    try {
      const hm = await api.heights((wb.min[0] + wb.max[0]) / 2, (wb.min[2] + wb.max[2]) / 2, span, Math.min(512, Math.max(64, Math.round(span / 2))), S.world && S.world.map);
      S.heights = {hm, frame: f};
      // The ground is known now: tighten the in-world ROI to it and let the
      // view cull the buried margin (a soft change: the octree rebuilds next
      // frame, nothing resident is dropped).
      if (S.view === 'world' && S.prog && view.wv) { const R = worldRoi(); view.setRoi(R.min, R.max, 0); worldCull(); }
      validateNow();
    } catch (e) { T.setStatus('ground for validation: ' + e.message, true); }
  }

  /* =========================================================================
   * pieces: attach, grow, move, turn, delete
   * ====================================================================== */
  function specParams(spec) { return spec.params ? DG.getType(spec.kit, spec.type).normalize(spec.params) : undefined; }
  function firstPiece(spec) {
    edit('first piece', () => {
      const p = DG.newPiece(S.d, {kit: spec.kit, type: spec.type, preset: spec.preset || undefined, params: specParams(spec)});
      S.d.pieces.push(p);
      if (S.d.style !== spec.kit && !S.d.pieces.slice(0, -1).length) S.d.style = spec.kit;
      // an entrance piece's surface socket IS the entrance (the anchor the map places)
      const surf = DG.generatePiece(S.d, p).sockets.find(x => (x.tags || []).includes('surface'));
      if (surf && !S.d.entrance) S.d.entrance = {piece: p.id, socket: surf.name};
      S.sel = {t: 'piece', id: p.id};
    });
    S.framed = false;
  }
  function attachAt(ref, spec) {
    let res;
    try { res = DG.attachPiece(S.d, ref, {kit: spec.kit, type: spec.type, preset: spec.preset || undefined, params: specParams(spec), fit: true, checkOverlap: true}); }
    catch (e) {
      if (/overlap/.test(e.message) && confirm('That piece would overlap: ' + e.message + '\n\nAttach it anyway (the validation panel will keep reporting the overlap)?')) {
        try { res = DG.attachPiece(S.d, ref, {kit: spec.kit, type: spec.type, preset: spec.preset || undefined, params: specParams(spec), fit: true}); }
        catch (e2) { toast('cannot attach: ' + e2.message, true); return; }
      } else { toast('cannot attach ' + spec.type + ' to ' + ref.piece + '.' + ref.socket + ': ' + e.message, true); return; }
    }
    edit('attach ' + spec.type, () => { S.d = res.dungeon; S.sel = {t: 'piece', id: res.piece.id}; });
    T.setStatus('attached ' + res.piece.id + ' (' + spec.kit + '/' + spec.type + ') to ' + ref.piece + '.' + ref.socket);
  }
  function piece(id) { return S.d.pieces.find(p => p.id === id); }
  function deletePiece(id) {
    edit('delete ' + id, () => {
      S.d.pieces = S.d.pieces.filter(p => p.id !== id);
      for (const p of S.d.pieces) if (p.links) for (const k of Object.keys(p.links)) if (p.links[k].piece === id) delete p.links[k];
      S.d.props = (S.d.props || []).filter(x => x.piece !== id);
      if (S.d.entrance && S.d.entrance.piece === id) S.d.entrance = null;
      S.sel = null;
    });
  }
  function turnPiece(id, dq) { const p = piece(id); if (p) edit('turn ' + id, () => { p.yaw = (((p.yaw || 0) + dq * 90) % 360 + 360) % 360; }); }
  function movePiece(id, dv, key) { const p = piece(id); if (p) edit('move ' + id, () => { p.pos = p.pos.map((v, i) => v + dv[i]); }, key); }
  function resnap(id) {
    const p = piece(id);
    if (!p || !p.links) { toast('this piece has no links to snap to'); return; }
    const placed = S.dn.pieces.map(q => DG.placePiece(S.dn, q));
    for (const sn of Object.keys(p.links)) {
      const L = p.links[sn];
      const tp = placed.find(q => q.id === L.piece);
      const target = tp && tp.sockets.find(s => s.name === L.socket);
      const local = DG.generatePiece(S.dn, p).sockets.find(s => s.name === sn);
      if (!target || !local) continue;
      const sn2 = DG.snapTransform(target, local, p.yaw);
      if (!sn2) continue;
      edit('re-snap ' + id, () => { p.pos = sn2.pos; p.yaw = sn2.yaw; });
      return;
    }
    toast('no link of ' + id + ' can be met by a quarter turn');
  }
  function unlinkSocket(pid, sn) {
    edit('unlink ' + pid + '.' + sn, () => {
      const p = piece(pid);
      const L = p && p.links && p.links[sn];
      if (L) { delete p.links[sn]; const o = piece(L.piece); if (o && o.links && o.links[L.socket] && o.links[L.socket].piece === pid) delete o.links[L.socket]; }
    });
  }
  function growAt(ref, reroll) {
    const g = S.grow;
    if (reroll) g.seed = (Math.imul(g.seed || 1, 2654435761) >>> 0) % 1000000 || 1;
    let result;
    try {
      result = DG.grow(S.d, ref, {count: g.count, kit: g.kit || undefined, branch: g.branch, seed: g.seed, types: g.types && g.types.length ? g.types : undefined, close: g.close});
    } catch (e) { toast('grow: ' + e.message, true); return; }
    S.proposal = {ref, result};
    if (!result.pieces.length) toast('nothing grew: ' + (result.notes.join('; ') || 'no type fits this socket'), true);
    showGhost();
    rebuild({voxels: true, fast: true});
  }
  function showGhost() {
    const P = S.proposal;
    if (!P) { ui.ghost.style.display = 'none'; return; }
    ui.ghost.style.display = '';
    ui.ghost.replaceChildren(h('span', {}, 'PROPOSAL · ' + P.result.pieces.length + ' pieces · seed ' + S.grow.seed + (P.result.open.length ? ' · ' + P.result.open.length + ' left open' : '')),
      h('button', {class: 'dg-pri', onclick: keepProposal, title: 'Enter'}, 'Keep'), h('button', {onclick: () => growAt(P.ref, true), title: 'another seed'}, 'Reroll'),
      h('button', {onclick: discardProposal, title: 'Esc'}, 'Discard'),
      P.result.notes.length ? h('span', {style: 'color:#ffb454', title: P.result.notes.join('\n')}, ' ' + P.result.notes.length + ' note(s)') : null);
  }
  function keepProposal() {
    const P = S.proposal;
    if (!P) return;
    S.proposal = null;
    showGhost();
    edit('grow ' + P.result.pieces.length + ' pieces', () => { S.d = DG.applyProposal(S.d, P.result); S.sel = P.result.pieces.length ? {t: 'piece', id: P.result.pieces[0].id} : S.sel; });
  }
  function discardProposal() { S.proposal = null; showGhost(); rebuild({voxels: true, fast: true}); }

  /* ---- refs + props ---- */
  function refByName(n) { return S.refs.find(r => r.name === n); }
  function placeRef(kind, cell, yaw) {
    const r = E.newRef(S.refs, kind, cell, yaw);
    edit('place ' + kind, () => { S.refs.push(r); S.sel = {t: 'ref', name: r.name}; if (!S.d.refs) S.d.refs = 'refs.json'; }, null, {voxels: false});
    return r;
  }
  function pieceAtAnchor(p) {
    const A = S.A, q = [p[0] + A[0] + 0.5, p[1] + A[1] + 0.5, p[2] + A[2] + 0.5];
    for (const pc of S.placed) for (const pr of pc.prims) if (pr.op === 'carve' && primContains(pr, q)) return pc.id;
    return null;
  }
  function placeProp(propId, cell, yaw) {
    const ids = new Set((S.d.props || []).map(x => x.id));
    let n = 0; while (ids.has('x' + n)) n++;
    const A = S.A;
    const pr = {id: 'x' + n, prop: propId, params: DG.normalizeParams(DG.PROPS[propId].SCHEMA, {}), seed: 0, pos: [cell[0] + A[0], cell[1] + A[1], cell[2] + A[2]], yaw: yaw || 0, piece: pieceAtAnchor(cell)};
    edit('place ' + propId, () => { (S.d.props = S.d.props || []).push(pr); S.sel = {t: 'prop', id: pr.id}; });
  }
  function propById(id) { return (S.d.props || []).find(x => x.id === id); }
  function adoptWaynodes() {
    if (!S.prog || !S.prog.waynodes.length) { toast('the pieces generate no waynodes'); return; }
    const nm = (n) => 'wn_' + n.replace(/\//g, '_');
    edit('adopt waynodes', () => {
      for (const w of S.prog.waynodes) {
        if (w.pit) continue;
        const name = nm(w.name);
        const ex = refByName(name);
        const links = w.links.filter(l => !S.prog.waynodes.find(o => o.name === l && o.pit)).map(nm);
        if (ex) { ex.pos = w.pos.slice(); ex.props.links = links; continue; }
        S.refs.push({name, kind: 'waynode', pos: w.pos.slice(), yaw: 0, props: {links}});
      }
      if (!S.d.refs) S.d.refs = 'refs.json';
    }, null, {voxels: false});
    toast('adopted ' + S.prog.waynodes.length + ' waynodes as refs (wn_<piece>_<node>): patrol routes can name them');
  }
  const LOOT_MAP = {mine_common: 'mine_supplies', mine_camp: 'mine_supplies', tomb_offering: 'crypt_offerings', crypt_key: 'crypt_offerings', sanctum: 'crypt_offerings'};
  function adoptSuggestions(only) {
    if (!S.prog) return;
    const take = S.prog.refs.filter(r => (only ? r.id === only : ['door', 'container', 'trap', 'spawner', 'marker'].includes(r.kind)));
    if (!take.length) { toast('nothing to adopt'); return; }
    let n = 0;
    edit('adopt suggestions', () => {
      for (const s of take) {
        const name = s.id.replace(/[^A-Za-z0-9_.-]/g, '_');
        if (refByName(name)) continue;
        const props = JSON.parse(JSON.stringify(s.props || {}));
        if (s.kind === 'container' && props.loot) props.loot = S.cat.loot.includes(props.loot) ? props.loot : (LOOT_MAP[props.loot] || 'cave_cache');
        if (s.kind === 'spawner' && props.behavior && !S.cat.behaviors.includes(props.behavior)) props.behavior = 'zombie';
        if (s.kind === 'trap' && props.region && props.region.min) {
          const R = {name: name + '_area', kind: 'region', pos: props.region.min.map(Math.round), yaw: 0, props: {size: props.region.max.map((v, i) => Math.max(1, Math.round(v - props.region.min[i])))}};
          S.refs.push(R);
          props.region = R.name;
        }
        const r = {name, kind: s.kind, pos: s.pos.slice(), yaw: s.yaw || 0, props};
        if (s.kind === 'npc') r.base = 'human';
        S.refs.push(r); n++;
      }
      if (!S.d.refs) S.d.refs = 'refs.json';
    }, null, {voxels: false});
    toast('adopted ' + n + ' suggested ref(s)');
  }
  function deleteSel() {
    const s = S.sel;
    if (!s) return;
    if (s.t === 'piece') deletePiece(s.id);
    else if (s.t === 'ref') edit('delete ' + s.name, () => { S.refs = S.refs.filter(r => r.name !== s.name); S.sel = null; }, null, {voxels: false});
    else if (s.t === 'prop') edit('delete prop ' + s.id, () => { S.d.props = S.d.props.filter(x => x.id !== s.id); S.sel = null; });
  }
  function fixSel() {
    const s = S.sel;
    if (!s) return;
    if (s.t === 'piece' && !piece(s.id)) S.sel = null;
    if (s.t === 'socket' && !piece(s.piece)) S.sel = null;
    if (s.t === 'ref' && !refByName(s.name)) S.sel = null;
    if (s.t === 'prop' && !propById(s.id)) S.sel = null;
  }
  function select(sel, fly) {
    S.sel = sel;
    if (S.isolate) applyClip();
    renderInspector(); renderBottom();
    view.dirty = true;
    if (fly) flyToSel();
  }
  function flyToSel() {
    const s = S.sel;
    if (!s || !S.prog) return;
    if (s.t === 'piece') { const pc = S.prog.pieces.find(p => p.id === s.id); if (pc) view.frameBox(pc.bounds.min, pc.bounds.max, {k: 1.6, keepAngle: true}); }
    else if (s.t === 'socket') { const so = S.sockets.find(x => x.piece === s.piece && x.name === s.socket); if (so) view.flyTo(socketCentre(so), Math.max(70, so.w * 2.5)); }
    else if (s.t === 'ref') { const r = refByName(s.name); if (r) view.flyTo(r.pos, 70); }
    else if (s.t === 'prop') { const p = propById(s.id); if (p) view.flyTo(p.pos.map((v, i) => v - S.A[i]), 60); }
  }

  /* =========================================================================
   * the view: overlay, clicks, gizmo, drop
   * ====================================================================== */
  const ghostIds = () => new Set(S.proposal ? S.proposal.result.pieces.map(p => p.id) : []);
  function selOrigin() {
    const s = S.sel;
    if (!s) return null;
    if (s.t === 'piece') { const p = S.dn && S.dn.pieces.find(x => x.id === s.id); return p ? p.pos.map((v, i) => v - S.A[i]) : null; }
    if (s.t === 'ref') { const r = refByName(s.name); return r ? r.pos.slice() : null; }
    if (s.t === 'prop') { const p = propById(s.id); return p ? p.pos.map((v, i) => v - S.A[i]) : null; }
    return null;
  }
  function draw(D) {
    const P = S.prog;
    if (!P) return;
    const ghosts = ghostIds();
    const selP = S.sel && (S.sel.t === 'piece' ? S.sel.id : S.sel.t === 'socket' ? S.sel.piece : null);
    for (const pc of P.pieces) {
      const g = ghosts.has(pc.id), sel = pc.id === selP;
      const col = g ? COL.ghost : sel ? COL.sel : COL.box;
      if (sel || g || !S.isolate) D.box(pc.bounds.min, pc.bounds.max.map(v => v + 1), sel || g ? col : [col[0], col[1], col[2], 0.28]);
      const top = [(pc.bounds.min[0] + pc.bounds.max[0]) / 2, pc.bounds.max[1] + 4, (pc.bounds.min[2] + pc.bounds.max[2]) / 2];
      D.label(top, pc.id + ' ' + pc.type + (g ? ' (proposed)' : ''), g ? COL.ghost : sel ? COL.sel : [0.75, 0.8, 0.9, 0.9], {dx: -20});
      const p = S.dn.pieces.find(x => x.id === pc.id);
      if (p) D.marker(p.pos.map((v, i) => v - S.A[i] + (i === 1 ? 2 : 0.5)), 'p', g ? COL.ghost : sel ? COL.sel : [0.75, 0.8, 0.9, 1], {kind: 'piece', id: pc.id}, {sel, size: 5});
    }
    // the drag's ghost
    if (S.drag && S.drag.t === 'piece' && S.drag.dv) {
      const pc = P.pieces.find(p => p.id === S.drag.id);
      if (pc) D.box(pc.bounds.min.map((v, i) => v + S.drag.dv[i]), pc.bounds.max.map((v, i) => v + 1 + S.drag.dv[i]), [1, 1, 1, 0.9]);
    }
    for (const s of S.sockets) {
      const sel = S.sel && S.sel.t === 'socket' && S.sel.piece === s.piece && S.sel.name === undefined && S.sel.socket === s.name;
      const isDrop = S.drop && S.drop.piece === s.piece && S.drop.socket === s.name;
      const col = isDrop ? [1, 1, 1, 1] : sel ? COL.sel : s.state === 'broken' ? COL.broken : s.state === 'linked' ? COL.linked : s.tags.includes('surface') ? COL.surface : COL.open;
      const isEnt = P.entrance.piece === s.piece && P.entrance.socket === s.name;
      drawSocket(D, s, col, s.state === 'linked' && !sel && !isDrop ? null : s.piece + '.' + s.name + (isEnt ? ' (entrance)' : '') + (s.state === 'broken' ? ' BROKEN' : ''));
      D.marker(socketCentre(s), s.state === 'open' ? 'o' : s.state === 'broken' ? '!' : '', col, {kind: 'socket', piece: s.piece, socket: s.name}, {size: s.state === 'linked' ? 3 : 5, sel: sel || isDrop});
    }
    if (S.showWay) for (const w of P.waynodes) {
      for (const l of w.links) { const o = S.wayNames.get(l); if (o && o.name > w.name) D.line(c3(w.pos, 1), c3(o.pos, 1), [0.5, 0.8, 1, w.pit || o.pit ? 0.25 : 0.55]); }
    }
    if (S.showSug) for (const r of P.refs) D.marker(c3(r.pos, 2), r.kind[0].toUpperCase(), [0.85, 0.75, 1, 0.75], {kind: 'sug', id: r.id}, {size: 4, sel: S.sel && S.sel.t === 'sug' && S.sel.id === r.id});
    for (const pr of S.d.props || []) {
      const p = pr.pos.map((v, i) => v - S.A[i]);
      const sel = S.sel && S.sel.t === 'prop' && S.sel.id === pr.id;
      D.marker(c3(p, 2), '+', [0.9, 0.65, 1, 1], {kind: 'prop', id: pr.id}, {sel, size: 5});
      if (sel || S.labels) D.label(c3(p, 2), pr.id + ' ' + pr.prop, [0.9, 0.65, 1, 1], {dx: 9});
    }
    E.drawRefs(D, S.refs, {sel: S.sel && S.sel.t === 'ref' ? S.sel.name : null, schedules: S.schedules, cat: S.cat, labels: S.labels, waynodes: refWaynodes()});
    if (S.tool && S.tool.route) for (let i = 0; i + 1 < S.tool.route.list.length; i++) {
      const a = refByName(S.tool.route.list[i]), b = refByName(S.tool.route.list[i + 1]);
      if (a && b) D.line(c3(a.pos, 2), c3(b.pos, 2), [1, 1, 1, 1]);
    }
    // the gizmo
    const o = selOrigin();
    if (o && !S.proposal) {
      const L = gizmoLen();
      const base = [o[0] + 0.5, o[1] + 0.5, o[2] + 0.5];
      const ax = [[1, 0, 0, [1, 0.35, 0.35, 1]], [0, 1, 0, [0.4, 1, 0.4, 1]], [0, 0, 1, [0.45, 0.6, 1, 1]]];
      for (const [x, y, z, c] of ax) {
        const e = [base[0] + x * L, base[1] + y * L, base[2] + z * L];
        D.line(base, e, c);
        D.marker(e, x ? 'x' : y ? 'y' : 'z', c, {kind: 'gizmo', axis: [x, y, z]}, {size: 5});
      }
      if (S.sel.t === 'ref') {
        const r = refByName(S.sel.name), k = r && E.sizeKey(r);
        if (k) { const b = E.boxOf(r, r.props[k] || [8, 2, 8]); const {fwd, right} = E.frameOf(r.yaw); const s = r.props[k] || [8, 2, 8];
          const far = [0, 1, 2].map(i => r.pos[i] + right[i] * (s[0] - 1) + fwd[i] * (s[2] - 1) + 0.5);
          D.marker(far, '◢', [1, 1, 1, 1], {kind: 'handle', key: k}, {size: 5}); void b; }
      }
    }
  }
  const c3 = (p, dy) => [p[0] + 0.5, p[1] + dy, p[2] + 0.5];
  function gizmoLen() { const wv = view.wv; return wv ? Math.max(8, Math.min(80, (wv.cam.mode === 'orbit' ? wv.cam.dist : 120) * 0.12)) : 20; }
  function refWaynodes() {
    const m = new Map();
    if (S.prog) for (const w of S.prog.waynodes) m.set(w.name, w);
    return m;
  }

  function capture(e, p) {
    const m = view.markerAt(p[0], p[1]);
    if (!m || (m.kind !== 'gizmo' && m.kind !== 'handle')) return false;
    const o = selOrigin();
    if (!o) return false;
    const s = S.sel;
    if (m.kind === 'handle') {
      const r = refByName(s.name);
      hist.push('resize ' + r.name);
      S.drag = {t: 'handle', key: m.key, r};
    } else {
      const base = [o[0] + 0.5, o[1] + 0.5, o[2] + 0.5];
      const a = view.project(base), b = view.project(base.map((v, i) => v + m.axis[i] * 10));
      if (!a || !b) return false;
      const v = [(b[0] - a[0]) / 10, (b[1] - a[1]) / 10];
      if (Math.hypot(v[0], v[1]) < 0.05) return false;
      S.drag = {t: s.t, id: s.id || s.name, axis: m.axis, v, x0: p[0], y0: p[1], dv: [0, 0, 0], start: null};
      if (s.t === 'ref') { const r = refByName(s.name); hist.push('move ' + r.name); S.drag.start = r.pos.slice(); }
      if (s.t === 'prop') { const pr = propById(s.id); hist.push('move prop ' + pr.id); S.drag.start = pr.pos.slice(); }
    }
    const mv = (ev) => dragMove(ev), up = () => { window.removeEventListener('pointermove', mv); window.removeEventListener('pointerup', up); dragEnd(); };
    window.addEventListener('pointermove', mv); window.addEventListener('pointerup', up);
    return true;
  }
  function dragMove(ev) {
    const d = S.drag;
    if (!d) return;
    const r0 = view.canvas.getBoundingClientRect(), p = [ev.clientX - r0.left, ev.clientY - r0.top];
    if (d.t === 'handle') {
      const r = d.r, pt = view.rayPlaneY(p[0], p[1], r.pos[1]);
      if (!pt) return;
      const {fwd, right} = E.frameOf(r.yaw);
      const dl = [pt[0] - r.pos[0], 0, pt[2] - r.pos[2]];
      const s = (r.props[d.key] || [8, 2, 8]).slice();
      s[0] = Math.max(1, Math.round(dl[0] * right[0] + dl[2] * right[2]));
      s[2] = Math.max(1, Math.round(dl[0] * fwd[0] + dl[2] * fwd[2]));
      r.props[d.key] = s;
      view.dirty = true; renderInspectorSoon();
      return;
    }
    const dx = p[0] - d.x0, dy = p[1] - d.y0, l2 = d.v[0] * d.v[0] + d.v[1] * d.v[1];
    let k = Math.round((dx * d.v[0] + dy * d.v[1]) / l2);
    if (ev.shiftKey) k = Math.round(k / 10) * 10;
    d.dv = d.axis.map(a => a * k);
    if (d.t === 'ref') { const r = refByName(d.id); if (r) r.pos = d.start.map((v, i) => v + d.dv[i]); }
    if (d.t === 'prop') { const pr = propById(d.id); if (pr) pr.pos = d.start.map((v, i) => v + d.dv[i]); }
    view.dirty = true;
  }
  function dragEnd() {
    const d = S.drag;
    S.drag = null;
    if (!d) return;
    if (d.t === 'piece') { if (d.dv.some(v => v)) movePiece(d.id, d.dv); else view.dirty = true; return; }
    rebuild({voxels: d.t === 'prop'});
  }
  let inspT = 0;
  function renderInspectorSoon() { clearTimeout(inspT); inspT = setTimeout(renderInspector, 80); }

  function onClick(e, hit, p) {
    // a placing tool takes the click first
    if (S.tool && S.tool.route) {
      if (hit && hit.marker && hit.marker.kind === 'ref') { const r = refByName(hit.marker.name); if (r && r.kind === 'waynode') { S.tool.route.list.push(r.name); T.setStatus('route: ' + S.tool.route.list.join(' → ') + '  (Enter ends, Esc cancels)'); view.dirty = true; } }
      return;
    }
    if (S.tool && S.tool.spec && hit && hit.voxel) {
      const v = hit.voxel;
      const cell = [v.cell[0] + v.face[0], v.cell[1] + v.face[1], v.cell[2] + v.face[2]];
      if (v.face[1] === 0) cell[1] = v.cell[1];     // on a wall: stand on the floor row of the hit
      const yaw = camYaw();
      if (S.tool.spec.t === 'ref') placeRef(S.tool.spec.kind, cell, yaw);
      else if (S.tool.spec.t === 'prop') placeProp(S.tool.spec.prop, cell, yaw);
      if (!e.shiftKey) { S.tool = null; renderPalette(); T.setStatus(''); }
      return;
    }
    if (hit && hit.marker) {
      const m = hit.marker;
      if (m.kind === 'socket') return select({t: 'socket', piece: m.piece, socket: m.socket});
      if (m.kind === 'piece') return select({t: 'piece', id: m.id});
      if (m.kind === 'ref') return select({t: 'ref', name: m.name});
      if (m.kind === 'prop') return select({t: 'prop', id: m.id});
      if (m.kind === 'sug') return select({t: 'sug', id: m.id});
      return;
    }
    if (hit && hit.voxel) {
      const v = hit.voxel;
      const air = [v.cell[0] + v.face[0], v.cell[1] + v.face[1], v.cell[2] + v.face[2]];
      const id = pieceAtAnchor(air) || pieceAtAnchor(v.cell);
      if (id) return select({t: 'piece', id});
    }
    select(null);
  }
  function onHover() {}
  function camYaw() {
    const wv = view.wv; if (!wv) return 0;
    const f = wv.camForward();
    const deg = Math.atan2(f[0], f[2]) * 180 / Math.PI;      // heading: 0 = +Z, 90 = +X
    const q = view.frame.q;
    return ((Math.round(deg / 90) * 90 - q * 90) % 360 + 360) % 360;
  }

  function bindDrop() {
    const el = view.el;
    el.addEventListener('dragover', (e) => {
      e.preventDefault();
      e.dataTransfer.dropEffect = 'copy';
      el.classList.add('dg-drop');
      const spec = S.dragSpec;
      if (spec && spec.t === 'piece' && S.d.pieces.length) {
        const r = view.canvas.getBoundingClientRect(), px = e.clientX - r.left, py = e.clientY - r.top;
        let best = null, bd = 90;
        for (const s of S.sockets) {
          if (s.state === 'linked') continue;
          const q = view.project(socketCentre(s));
          if (!q) continue;
          const dd = Math.hypot(q[0] - px, q[1] - py);
          if (dd < bd) { bd = dd; best = {piece: s.piece, socket: s.name}; }
        }
        if (JSON.stringify(best) !== JSON.stringify(S.drop)) { S.drop = best; view.dirty = true; }
      }
    });
    el.addEventListener('dragleave', (e) => { if (e.target === el || !el.contains(e.relatedTarget)) { el.classList.remove('dg-drop'); S.drop = null; view.dirty = true; } });
    el.addEventListener('drop', (e) => {
      e.preventDefault();
      el.classList.remove('dg-drop');
      let spec;
      try { spec = JSON.parse(e.dataTransfer.getData('text/plain')); } catch (err) { return; }
      const r = view.canvas.getBoundingClientRect(), px = e.clientX - r.left, py = e.clientY - r.top;
      if (spec.t === 'piece') {
        const target = S.drop;
        S.drop = null;
        if (!S.d.pieces.length) { firstPiece(spec); return; }
        if (!target) { toast('drop a piece ON an open socket (an amber frame): it snaps there'); return; }
        attachAt(target, spec);
        return;
      }
      const hit = view.pickVoxel(px, py);
      if (!hit) { toast('nothing under the cursor to put it on'); return; }
      const cell = [hit.cell[0] + hit.face[0], hit.cell[1] + hit.face[1], hit.cell[2] + hit.face[2]];
      if (hit.face[1] === 0) cell[1] = hit.cell[1];
      if (spec.t === 'ref') placeRef(spec.kind, cell, camYaw());
      else if (spec.t === 'prop') placeProp(spec.prop, cell, camYaw());
    });
  }

  function onKey(e) {
    if (!T.isActive()) return;
    if (!built || T.page.offsetParent === null) return;
    const tag = document.activeElement && document.activeElement.tagName;
    if (/input|select|textarea/i.test(tag)) return;
    const k = e.key;
    if ((e.ctrlKey || e.metaKey) && k.toLowerCase() === 'z') { e.preventDefault(); e.stopPropagation(); if (e.shiftKey) hist.doRedo(); else hist.doUndo(); return; }
    if ((e.ctrlKey || e.metaKey) && k.toLowerCase() === 'y') { e.preventDefault(); hist.doRedo(); return; }
    if ((e.ctrlKey || e.metaKey) && k.toLowerCase() === 's') { e.preventDefault(); save(); return; }
    if (k === 'Escape') {
      if (S.tool && S.tool.route) { S.tool = null; T.setStatus('route cancelled'); }
      else if (S.proposal) discardProposal();
      else if (S.tool) { S.tool = null; renderPalette(); T.setStatus(''); }
      else select(null);
      view.dirty = true; return;
    }
    if (k === 'Enter') {
      if (S.tool && S.tool.route) { finishRoute(); return; }
      if (S.proposal) { keepProposal(); return; }
    }
    // camera keys belong to the view when it has focus
    if (document.activeElement === view.canvas && 'wasdqe'.includes(k.toLowerCase()) && !e.ctrlKey) return;
    const s = S.sel;
    if (k === 'f' || k === 'F') { if (s) flyToSel(); else frameAll(); return; }
    if (!s) return;
    if (k === 'Delete' || k === 'Backspace') { e.preventDefault(); deleteSel(); return; }
    const step = e.shiftKey ? 10 : 1;
    const dv = k === 'ArrowLeft' ? [-step, 0, 0] : k === 'ArrowRight' ? [step, 0, 0] : k === 'ArrowUp' ? [0, 0, -step] : k === 'ArrowDown' ? [0, 0, step]
             : k === 'PageUp' ? [0, step, 0] : k === 'PageDown' ? [0, -step, 0] : null;
    if (dv) {
      e.preventDefault();
      if (s.t === 'piece') movePiece(s.id, dv, 'key-move:' + s.id);
      else if (s.t === 'ref') { const r = refByName(s.name); edit('move ' + r.name, () => { r.pos = r.pos.map((v, i) => v + dv[i]); }, 'key-move:' + r.name, {voxels: false}); }
      else if (s.t === 'prop') { const pr = propById(s.id); edit('move ' + pr.id, () => { pr.pos = pr.pos.map((v, i) => v + dv[i]); }, 'key-move:' + pr.id); }
      return;
    }
    if (k === 'r' || k === 'R') {
      const dq = e.shiftKey ? -1 : 1;
      if (s.t === 'piece') turnPiece(s.id, dq);
      else if (s.t === 'ref') { const r = refByName(s.name); edit('turn ' + r.name, () => { r.yaw = ((r.yaw + dq * 90) % 360 + 360) % 360; }, null, {voxels: false}); }
      else if (s.t === 'prop') { const pr = propById(s.id); edit('turn ' + pr.id, () => { pr.yaw = ((pr.yaw + dq * 90) % 360 + 360) % 360; }); }
      return;
    }
    if ((k === 'g' || k === 'G') && s.t === 'socket') growAt({piece: s.piece, socket: s.socket});
  }

  /* ---- patrol route picking ---- */
  function startRoute(name, row) {
    S.tool = {route: {sched: name, row, list: []}};
    T.setStatus('route for ' + name + ' row ' + (row + 1) + ': click waynode refs in order (Enter ends, Esc cancels). Adopt waynodes first if there are none.');
    view.dirty = true;
  }
  function finishRoute() {
    const t = S.tool.route;
    S.tool = null;
    const doc = S.schedules[t.sched];
    if (doc && t.list.length) { doc.rows[t.row].extra = Object.assign({}, doc.rows[t.row].extra, {route: t.list}); S.schedDirty.add(t.sched); }
    T.setStatus(t.list.length ? 'route set: ' + t.list.join(' → ') + ' (Save schedule to keep it)' : 'no route picked');
    renderInspector(); view.dirty = true;
  }

  /* =========================================================================
   * inspector
   * ====================================================================== */
  function renderInspector() {
    const s = S.sel, out = [];
    if (!S.prog) { ui.insp.replaceChildren(); return; }
    if (!s) {
      out.push(h('h4', {}, 'Dungeon'), h('div', {class: 'dg-hint'},
        S.d.pieces.length + ' pieces · ' + S.refs.length + ' refs · ' + (S.d.props || []).length + ' props · ' + S.prog.prims.length + ' prims · ' +
        fmtM(S.prog.bounds.max[0] - S.prog.bounds.min[0]) + ' × ' + fmtM(S.prog.bounds.max[1] - S.prog.bounds.min[1]) + ' × ' + fmtM(S.prog.bounds.max[2] - S.prog.bounds.min[2])),
        h('div', {class: 'dg-hint'}, 'Click a piece, a socket frame, a ref marker or a prop. Keys: arrows / PgUp / PgDn move (Shift ×10), R turns, Delete removes, F frames, G grows from a socket, Ctrl+Z / Ctrl+Y, Ctrl+S.'));
      if (S.variant) out.push(h('h4', {}, 'Variant of ' + S.variant.base), variantSummary());
      out.push(h('h4', {}, 'Entrance'), h('div', {class: 'dg-hint'}, S.prog.entrance.piece ? S.prog.entrance.piece + '.' + S.prog.entrance.socket + (S.d.entrance ? '' : ' (the first surface socket)') : 'none yet'));
      out.push(h('h4', {}, 'Palette'), paletteEditor());
      const pl = S.list.find(x => x.id === S.id);
      out.push(h('h4', {}, 'Placed on'), h('div', {class: 'dg-hint'}, pl && pl.placements.length ? pl.placements.map(p => p.map + ': ' + p.id + ' at ' + (p.at || []).join(', ') + ' yaw ' + (p.yaw || 0)).join('\n') : 'not on any map: Environment > World map > Dungeons tool places it'));
      out.push(h('h4', {}, 'refs.json about'), (() => { const t = h('textarea', {style: 'width:100%;min-height:50px', onchange: () => edit('about', () => { S.about = t.value; }, null, {voxels: false})}); t.value = S.about; return t; })());
    } else if (s.t === 'piece') out.push(...pieceInspector(s.id));
    else if (s.t === 'socket') out.push(...socketInspector(s.piece, s.socket));
    else if (s.t === 'ref') out.push(...refInspector(s.name));
    else if (s.t === 'prop') out.push(...propInspector(s.id));
    else if (s.t === 'sug') {
      const r = S.prog.refs.find(x => x.id === s.id);
      if (r) out.push(h('h4', {}, 'Suggested ' + r.kind), h('div', {class: 'dg-hint'}, r.piece + ' suggests a ' + r.kind + ' here: ' + JSON.stringify(r.props || {})),
                      h('div', {class: 'dg-btns'}, h('button', {class: 'dg-pri', onclick: () => adoptSuggestions(r.id)}, 'Adopt as a ref')));
    }
    ui.insp.replaceChildren(...out);
  }
  function paletteEditor() {
    const K = DG.getKit(S.d.style);
    const sel = h('select', {onchange: () => edit('palette', () => { S.d.palette = Object.assign({}, S.d.palette, {variant: sel.value}); })}, Object.keys(K.PALETTES).map(v => h('option', {value: v}, v)));
    sel.value = (S.d.palette && S.d.palette.variant) || K.DEFAULT_PALETTE;
    const res = DG.resolvePalette(S.d, S.d.style);
    return h('div', {}, h('div', {class: 'dg-field'}, h('label', {}, 'variant'), sel),
      h('div', {class: 'dg-hint'}, Object.entries(res).map(([k, v]) => k + '=' + v).join(' · ')));
  }
  function pieceInspector(id) {
    const p = piece(id);
    if (!p) return [h('div', {class: 'dg-hint'}, 'piece ' + id + ' is part of the PROPOSAL: keep it to edit it')];
    const T0 = DG.getType(p.kit, p.type);
    const out = [h('h4', {}, id + ' · ' + p.kit + '/' + p.type), h('div', {class: 'dg-hint'}, T0.label + ' v' + T0.version + ' · pos ' + p.pos.join(', ') + ' (dungeon frame) · yaw ' + p.yaw + '°')];
    if (S.variant) {
      // A VARIANT: is this piece the base's, an override of it, or the variant's own?
      const st = pieceState(id);
      const what = st === 'inherited' ? 'inherited from ' + S.variant.base + ' unchanged'
                 : st === 'added' ? 'added by this variant (not in ' + S.variant.base + ')'
                 : 'OVERRIDDEN: ' + Object.keys(pieceOverride(S.variant.baseFlat.pieces.find(x => x.id === id), p) || {}).join(', ') + ' differ from ' + S.variant.base;
      out.push(h('div', {class: 'dg-field'}, h('span', {class: 'dg-chip', style: st === 'overridden' ? 'color:var(--yellow,#e8c35a)' : st === 'added' ? 'color:var(--green,#7bd88f)' : ''}, st), ' ', what,
        st === 'overridden' ? h('button', {style: 'margin-left:6px', title: 'throw away this variant\'s changes to the piece: the base\'s piece again', onclick: () => revertPiece(id)}, 'Revert to base') : null));
    }
    out.push(h('div', {class: 'dg-btns'},
      h('button', {onclick: () => turnPiece(id, 1), title: 'R'}, 'Turn ↻'), h('button', {onclick: () => turnPiece(id, -1), title: 'Shift+R'}, 'Turn ↺'),
      h('button', {onclick: () => resnap(id), title: 'put it back on its link'}, 'Re-snap'),
      h('button', {onclick: () => edit('reroll ' + id, () => { p.seed = ((Math.imul(DG.pieceSeed(S.d, p), 1664525) + 1013904223) >>> 0) || 1; }), title: 'a new piece seed'}, 'Reroll'),
      h('button', {onclick: () => edit('seed ' + id, () => { p.seed = 0; }), title: 'derive the seed from the dungeon seed + id'}, 'Seed 0'),
      h('button', {class: 'dg-bad', onclick: () => deletePiece(id)}, 'Delete')));
    const auto = h('input', {type: 'checkbox', checked: p.autoProps !== false, onchange: () => edit('autoProps ' + id, () => { if (auto.checked) delete p.autoProps; else p.autoProps = false; })});
    out.push(h('label', {class: 'dg-hint', title: 'the furniture the piece suggests (crates, carts, altars...)'}, auto, ' place the piece\'s own props'));
    const pr = h('select', {onchange: () => { if (!pr.value) return; const x = T.presets(p.kit, p.type).find(q => q.name === pr.value); if (x) edit('preset ' + id, () => { p.params = T0.normalize(x.params); }); }},
      h('option', {value: ''}, 'apply a preset…'), T.presets(p.kit, p.type).map(x => h('option', {value: x.name}, (x.user ? '★ ' : '') + x.name)));
    out.push(pr);
    let held = false;
    const sp = sliderPanel(T0.SCHEMA, p.params, (k, v, final) => {
      if (!final && !held) { held = true; view.coarse(true); }
      if (final && held) { held = false; view.coarse(false); }
      hist.push('slider ' + id + '.' + k, 'sl:' + id + ':' + k);
      p.params = T0.normalize(Object.assign({}, p.params, {[k]: v}));
      clearTimeout(compileT);
      pendC = true;
      compileT = setTimeout(() => { pendC = false; compileNow(); schedulePreview(final ? 0 : 60); scheduleValidate(); renderBottom(); renderHeader(); view.dirty = true; }, final ? 0 : 40);
    }, {closed: ['Surface', 'Age', 'Light', 'Join']});
    out.push(sp.root);
    const sk = S.sockets.filter(s => s.piece === id);
    out.push(h('h4', {}, 'Sockets'), ...sk.map(s => sockRow(s)));
    return out;
  }
  function sockRow(s) {
    const L = (piece(s.piece) || {}).links || {};
    const to = L[s.name];
    return h('div', {class: 'dg-sock ' + s.state, onclick: () => select({t: 'socket', piece: s.piece, socket: s.name}, true)},
      s.name + ' ' + s.w + '×' + s.h + ' ' + s.profile + (to ? ' → ' + to.piece + '.' + to.socket : '') + (s.state === 'broken' ? '  BROKEN' : s.state === 'open' ? '  open' : ''));
  }
  function socketInspector(pid, sn) {
    const s = S.sockets.find(x => x.piece === pid && x.name === sn);
    if (!s) return [h('div', {class: 'dg-hint'}, 'no socket')];
    const p = piece(pid);
    const out = [h('h4', {}, 'Socket ' + pid + '.' + sn), h('div', {class: 'dg-hint'}, s.w + '×' + s.h + ' ' + s.profile + ' · at ' + s.pos.join(', ') + ' · ' + s.state + (s.tags.length ? ' · tags ' + s.tags.join(', ') : ''))];
    const isEnt = S.prog.entrance.piece === pid && S.prog.entrance.socket === sn;
    out.push(h('div', {class: 'dg-btns'},
      h('button', {onclick: () => select({t: 'piece', id: pid})}, 'Select piece'),
      isEnt ? h('span', {class: 'dg-chip ok'}, 'the entrance') : h('button', {onclick: () => edit('entrance', () => { S.d.entrance = {piece: pid, socket: sn}; }), title: 'the anchor the map places'}, 'Make entrance'),
      p && p.links && p.links[sn] ? h('button', {onclick: () => unlinkSocket(pid, sn)}, 'Unlink') : null));
    if (s.state !== 'linked') {
      out.push(h('h4', {}, 'Attach a piece here'));
      for (const kit of [p ? p.kit : S.d.style, ...DG.STYLES.filter(k => k !== (p ? p.kit : S.d.style))]) {
        out.push(h('div', {class: 'dg-hint', style: 'margin-top:4px'}, kit));
        out.push(h('div', {class: 'dg-tgrid'}, DG.listTypes(kit).filter(t => !t.surface).map(t => h('button', {'data-attach': kit + '/' + t.type, onclick: () => attachAt({piece: pid, socket: sn}, {t: 'piece', kit, type: t.type})}, t.label))));
      }
      out.push(h('h4', {}, 'Grow a branch'), growForm({piece: pid, socket: sn}, p ? p.kit : S.d.style));
    }
    return out;
  }
  function growForm(ref, kit) {
    const g = S.grow;
    if (!g.kit) g.kit = kit;
    const num = (k, min, max, step) => { const i = h('input', {type: 'number', value: g[k], min, max, step, style: 'width:70px', onchange: () => { g[k] = +i.value; }}); return i; };
    const kitSel = h('select', {onchange: () => { g.kit = kitSel.value; g.types = null; renderInspector(); }}, DG.STYLES.map(k => h('option', {value: k}, k)));
    kitSel.value = g.kit;
    const types = DG.listTypes(g.kit).filter(t => !t.surface);
    const box = h('div', {class: 'dg-tgrid'}, types.map(t => {
      const c = h('input', {type: 'checkbox', checked: !g.types || g.types.includes(t.type), onchange: () => {
        const on = [...box.querySelectorAll('input')].map((x, i) => x.checked ? types[i].type : null).filter(Boolean);
        g.types = on.length === types.length ? null : on;
      }});
      return h('label', {style: 'font-size:11px'}, c, t.label);
    }));
    const close = h('input', {type: 'checkbox', checked: g.close, onchange: () => { g.close = close.checked; }});
    return h('div', {},
      h('div', {class: 'dg-field'}, h('label', {}, 'pieces'), num('count', 1, 24, 1)),
      h('div', {class: 'dg-field', title: '0 = one chain; 1 = every new socket keeps growing'}, h('label', {}, 'branch'), num('branch', 0, 1, 0.1)),
      h('div', {class: 'dg-field'}, h('label', {}, 'seed'), num('seed', 1, 999999, 1)),
      h('div', {class: 'dg-field'}, h('label', {}, 'kit'), kitSel),
      h('div', {class: 'dg-hint'}, 'types (unticked never grow)'), box,
      h('label', {class: 'dg-hint'}, close, ' cap the ends with dead-end pieces'),
      h('div', {class: 'dg-btns'}, h('button', {class: 'dg-pri', 'data-grow': '1', onclick: () => growAt(ref), title: 'G'}, 'Grow')));
  }
  function refCtx() {
    return {
      cat: S.cat, refs: S.refs,
      signals: () => [...new Set(S.refs.map(E.signalOf).filter(Boolean))].sort(),
      regions: () => S.refs.filter(r => r.kind === 'region' || r.kind === 'plate').map(r => r.name),
      waynodes: () => S.refs.filter(r => r.kind === 'waynode').map(r => r.name),
      change: (label, fn, key) => { edit(label, fn, key, {voxels: false}); },
      select: (n) => { S.sel = {t: 'ref', name: n}; },
      schedules: S.schedules,
      loadSchedule, scheduleEdited: (n, re) => { S.schedDirty.add(n); view.dirty = true; if (re) renderInspector(); },
      scheduleDirty: (n) => S.schedDirty.has(n), saveSchedule, newSchedule,
      onRoutePick: (n, row) => startRoute(n, row)
    };
  }
  function refInspector(name) {
    const r = refByName(name);
    if (!r) return [];
    const K = E.KINDS[r.kind];
    return [h('h4', {}, (K ? K.label : r.kind) + ' · ' + r.name),
            h('div', {class: 'dg-btns'}, h('button', {onclick: () => edit('turn ' + name, () => { r.yaw = (r.yaw + 90) % 360; }, null, {voxels: false})}, 'Turn ↻'),
              h('button', {onclick: () => { const c = JSON.parse(JSON.stringify(r)); c.name = E.freshName(S.refs, r.kind); c.pos = [r.pos[0] + 6, r.pos[1], r.pos[2]]; edit('duplicate ' + name, () => { S.refs.push(c); S.sel = {t: 'ref', name: c.name}; }, null, {voxels: false}); }}, 'Duplicate'),
              h('button', {class: 'dg-bad', onclick: deleteSel}, 'Delete')),
            E.refForm(r, refCtx())];
  }
  function propInspector(id) {
    const pr = propById(id);
    if (!pr) return [];
    const P = DG.PROPS[pr.prop];
    const out = [h('h4', {}, 'Prop ' + id + ' · ' + pr.prop), h('div', {class: 'dg-hint'}, 'pos ' + pr.pos.join(', ') + ' (dungeon frame) · yaw ' + pr.yaw + '° · piece ' + (pr.piece || '-'))];
    out.push(h('div', {class: 'dg-btns'}, h('button', {onclick: () => edit('turn ' + id, () => { pr.yaw = (pr.yaw + 90) % 360; })}, 'Turn ↻'),
      h('button', {onclick: () => edit('reroll ' + id, () => { pr.seed = ((pr.seed || 7) * 48271 % 2147483647) >>> 0; })}, 'Reroll'),
      h('button', {class: 'dg-bad', onclick: deleteSel}, 'Delete')));
    if (P) out.push(sliderPanel(P.SCHEMA, pr.params, (k, v, final) => {
      hist.push('prop ' + id + '.' + k, 'pp:' + id + ':' + k);
      pr.params = DG.normalizeParams(P.SCHEMA, Object.assign({}, pr.params, {[k]: v}));
      rebuild({voxels: true, fast: final});
    }).root);
    return out;
  }

  /* ---- schedules ---- */
  async function loadSchedule(name) {
    if (S.schedules[name] || S.loadingSched === name) return;
    S.loadingSched = name;
    try { S.schedules[name] = readSchedule(await api.schedule(name)); }
    catch (e) { S.schedules[name] = {extra: {}, rows: []}; }
    S.loadingSched = null;
    renderInspector(); view.dirty = true;
  }
  async function saveSchedule(name) {
    const doc = S.schedules[name];
    if (!doc) return;
    try { await api.saveSchedule(name, writeSchedule(doc)); S.schedDirty.delete(name); toast('saved assets/schedules/' + name + '.json'); renderInspector(); }
    catch (e) { toast('schedule: ' + e.message, true); }
  }
  async function newSchedule(r) {
    const name = (prompt('New schedule file name (assets/schedules/<name>.json):', (S.id || 'dungeon') + '_' + r.name) || '').trim();
    if (!/^[a-z0-9_]+$/.test(name)) { if (name) toast('a schedule name is lowercase letters, digits and _', true); return; }
    S.schedules[name] = {extra: {about: 'dungeon ' + (S.id || '') + ': ' + r.name + ' (written by the Dungeons tab)'}, rows: [{from: '00:00', to: '00:00', do: 'guard', at: '', extra: {}}]};
    S.schedDirty.add(name);
    await saveSchedule(name);
    try { S.cat = await api.catalog(true); } catch (e) {}
    edit('schedule ' + r.name, () => { r.props.schedule = name; }, null, {voxels: false});
  }

  /* =========================================================================
   * bottom panel: validation + graph
   * ====================================================================== */
  function renderBottom() {
    const errs = S.issues.filter(i => i.severity === 'error').length, warns = S.issues.filter(i => i.severity === 'warn').length;
    const tab = (k, l) => h('button', {class: S.btab === k ? 'on' : '', onclick: () => { S.btab = k; ui.bottom.classList.remove('min'); renderBottom(); }}, l);
    ui.btabs.replaceChildren(tab('valid', 'Validation ' + (errs ? errs + ' ✖ ' : '') + (warns ? warns + ' ⚠' : errs ? '' : '✔')), tab('graph', 'Graph'),
      h('span', {class: 'grow'}),
      h('label', {style: 'font-size:11px;color:var(--faint)'}, h('input', {type: 'checkbox', checked: S.showInfo, onchange: (e) => { S.showInfo = e.target.checked; renderBottom(); }}), 'info'),
      h('button', {onclick: () => ui.bottom.classList.toggle('min')}, '▁'));
    if (S.btab === 'valid') {
      const list = S.issues.filter(i => S.showInfo || i.severity !== 'info');
      const rank = {error: 0, warn: 1, info: 2};
      list.sort((a, b) => rank[a.severity] - rank[b.severity]);
      const box = h('div', {class: 'dg-issues'});
      if (!list.length) box.append(h('div', {class: 'ok'}, '✔ no problems' + (S.heights ? '' : ' (ground not checked: not placed)')));
      for (const v of list) box.append(h('div', {class: v.severity[0], onclick: () => gotoIssue(v), title: v.code},
        (v.severity === 'error' ? '✖ ' : v.severity === 'warn' ? '⚠ ' : '· ') + (v.piece || v.ref ? '[' + (v.piece || v.ref) + '] ' : '') + v.msg));
      ui.bbody.replaceChildren(box);
    } else {
      if (!ui.graph) { ui.graph = h('canvas', {class: 'dg-graph'}); ui.graph.addEventListener('click', graphClick); }
      ui.bbody.replaceChildren(ui.graph);
      drawGraph();
    }
  }
  function gotoIssue(v) {
    if (v.ref && refByName(v.ref)) { select({t: 'ref', name: v.ref}, true); return; }
    if (v.piece && piece(v.piece)) { S.sel = {t: 'piece', id: v.piece}; renderInspector(); }
    else if (v.piece && refByName(v.piece)) { select({t: 'ref', name: v.piece}, true); return; }
    if (v.at) view.flyTo(v.at, 80); else flyToSel();
    view.dirty = true;
  }
  // levels as bands, pieces as nodes (x = hops from the entrance), links as edges
  function graphLayout() {
    if (!S.prog) return null;
    const P = S.prog, ids = P.pieces.map(p => p.id);
    const floorY = new Map(S.dn.pieces.map(p => [p.id, p.pos[1] - S.A[1]]));
    const ys = [...new Set([...floorY.values()])].sort((a, b) => b - a);
    const bands = [];
    for (const y of ys) { const b = bands.find(x => Math.abs(x.y - y) <= 16); if (b) b.ys.push(y); else bands.push({y, ys: [y]}); }
    const bandOf = (id) => bands.findIndex(b => b.ys.includes(floorY.get(id)));
    const adj = new Map(ids.map(i => [i, new Set()]));
    for (const L of S.links) { adj.get(L.a.piece) && adj.get(L.a.piece).add(L.b.piece); adj.get(L.b.piece) && adj.get(L.b.piece).add(L.a.piece); }
    const hop = new Map(), q = [P.entrance.piece || ids[0]];
    if (q[0]) hop.set(q[0], 0);
    while (q.length) { const c = q.shift(); for (const n of adj.get(c) || []) if (!hop.has(n)) { hop.set(n, hop.get(c) + 1); q.push(n); } }
    let maxHop = 0; for (const v of hop.values()) maxHop = Math.max(maxHop, v);
    for (const i of ids) if (!hop.has(i)) hop.set(i, ++maxHop);
    const slots = new Map(), pos = new Map();
    for (const i of ids) { const k = bandOf(i) + ':' + hop.get(i); const n = slots.get(k) || 0; slots.set(k, n + 1); pos.set(i, {band: bandOf(i), col: hop.get(i), sub: n}); }
    return {bands, pos, maxHop};
  }
  function drawGraph() {
    const cv = ui.graph, L = graphLayout();
    if (!cv || !L) return;
    const W = cv.clientWidth || 600, H = cv.clientHeight || 150, dpr = Math.min(2, window.devicePixelRatio || 1);
    cv.width = W * dpr; cv.height = H * dpr;
    const g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = '#0b0e13'; g.fillRect(0, 0, W, H);
    g.font = '11px "Cascadia Code", Consolas, monospace'; g.textBaseline = 'middle';
    const nb = Math.max(1, L.bands.length), bh = (H - 8) / nb, cw = Math.max(60, (W - 100) / Math.max(1, L.maxHop + 1));
    S.graphHit = [];
    L.bands.forEach((b, i) => {
      g.fillStyle = i % 2 ? '#10141b' : '#0d1117'; g.fillRect(0, 4 + i * bh, W, bh);
      g.fillStyle = '#5c667a'; g.fillText('y ' + fmtM(b.y), 6, 4 + i * bh + bh / 2);
      S.graphHit.push({x0: 0, y0: 4 + i * bh, x1: 80, y1: 4 + (i + 1) * bh, band: b});
    });
    // Node boxes: left to right by hops in each band, never overlapping (a
    // later node in the same band starts after the one before it ends).
    const rect = new Map(), cursor = new Map();
    const label = (pc) => pc.id + ' ' + pc.type;
    const order = S.prog.pieces.slice().sort((x, y) => { const a = L.pos.get(x.id), b = L.pos.get(y.id); return a.band - b.band || a.col - b.col || a.sub - b.sub; });
    for (const pc of order) {
      const p = L.pos.get(pc.id), w = Math.max(54, g.measureText(label(pc)).width + 12);
      const x0 = Math.max(90 + p.col * cw, cursor.get(p.band) || 0);
      cursor.set(p.band, x0 + w + 10);
      rect.set(pc.id, {x0, w, y: 4 + p.band * bh + bh / 2});
    }
    const at = (id) => { const r = rect.get(id); return [r.x0 + r.w / 2, r.y]; };
    for (const Lk of S.links) {
      const a = at(Lk.a.piece), b = at(Lk.b.piece);
      g.strokeStyle = !Lk.compat.ok ? '#ff6b6b' : Lk.compat.taper ? '#ffb454' : '#7cf03a'; g.lineWidth = 2;
      g.beginPath(); g.moveTo(Math.round(a[0]) + 0.5, Math.round(a[1]) + 0.5); g.lineTo(Math.round(b[0]) + 0.5, Math.round(b[1]) + 0.5); g.stroke();
    }
    const errOf = new Set(S.issues.filter(i => i.severity === 'error').map(i => i.piece));
    const ghosts = ghostIds();
    for (const pc of S.prog.pieces) {
      const [x, y] = at(pc.id);
      const sel = S.sel && ((S.sel.t === 'piece' && S.sel.id === pc.id) || (S.sel.t === 'socket' && S.sel.piece === pc.id));
      const kitCol = {caves: '#5fa8d3', mines: '#d39a5f', ruins: '#b7a6e8'}[pc.kit] || '#aaa';
      const w = rect.get(pc.id).w;
      g.fillStyle = sel ? '#1d3a6e' : ghosts.has(pc.id) ? '#2a1d44' : '#1a2029'; g.fillRect(Math.round(x - w / 2), Math.round(y - 9), Math.round(w), 18);
      g.strokeStyle = sel ? '#6ea8fe' : kitCol; g.lineWidth = sel ? 2 : 1; g.strokeRect(Math.round(x - w / 2) + 0.5, Math.round(y - 9) + 0.5, Math.round(w), 18);
      g.fillStyle = kitCol; g.fillRect(Math.round(x - w / 2) + 2, Math.round(y - 7), 3, 14);
      g.fillStyle = '#d7dde6'; g.textAlign = 'center'; g.fillText(pc.id + ' ' + pc.type, Math.round(x) + 2, Math.round(y)); g.textAlign = 'left';
      const open = S.sockets.filter(s => s.piece === pc.id && s.state === 'open').length;
      if (open) { g.fillStyle = '#ffb454'; g.fillRect(Math.round(x + w / 2) - 2, Math.round(y - 11), 7, 7); }
      if (errOf.has(pc.id)) { g.fillStyle = '#ff6b6b'; g.fillRect(Math.round(x - w / 2) - 4, Math.round(y - 11), 7, 7); }
      S.graphHit.push({x0: x - w / 2, y0: y - 9, x1: x + w / 2, y1: y + 9, id: pc.id});
    }
  }
  function graphClick(e) {
    const r = ui.graph.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
    const hit = (S.graphHit || []).filter(o => x >= o.x0 && x <= o.x1 && y >= o.y0 && y <= o.y1).pop();
    if (!hit) return;
    if (hit.id) { select({t: 'piece', id: hit.id}, true); return; }
    if (hit.band) { const ys = hit.band.ys; S.ymin = Math.min(...ys) - 4; S.ymax = Math.max(...ys) + 70; applyClip(); }
  }

  /* =========================================================================
   * files
   * ====================================================================== */
  function renderHeader() {
    const dt = dirty();
    ui.status.textContent = (S.id ? 'assets/dungeons/' + S.id + '/' : '(not saved yet)') + (dt ? ' · unsaved' : ' · saved');
    ui.status.classList.toggle('dirty', dt);
    ui.save.disabled = !dt && !!S.id;
    ui.name.value = S.d.name; ui.style.value = S.d.style; ui.seed.value = S.d.seed;
    ui.undo.disabled = !hist.undo.length; ui.redo.disabled = !hist.redo.length;
    T.setDirty(dt);
  }
  async function refreshList() {
    try { const j = await api.list(); S.list = j.dungeons; S.gameMap = j.gameMap; } catch (e) { S.list = []; T.setStatus('dungeon list: ' + e.message, true); }
    ui.listSel.replaceChildren(h('option', {value: ''}, S.id ? '' : '(new, unsaved)'), ...S.list.map(d => h('option', {value: d.id}, d.id + (d.name && d.name !== d.id ? ' — ' + d.name : '') + (d.extends ? '  (variant of ' + d.extends + ')' : '') + (d.placements.length ? ' ⌂' : ''))));
    ui.listSel.value = S.id;
  }
  /* =========================================================================
   * VARIANTS (dungeon.json `extends`, package D8's inheritance)
   * ======================================================================
   * A variant's dungeon.json is the base's id + OVERRIDES: name / seed /
   * palette (and style / entrance / props when changed), `pieces: {<id>:
   * {params: {partial}, seed?, type?, ..., remove: true}}`, `add: [piece]`,
   * `refsPatch: {add: [ref], remove: [name], set: {<name>: {fields}}}`. The
   * page edits the FLATTENED dungeon (S.d = base + overrides, what compiles
   * and previews) and writes the overrides back on save -- never the
   * flattening, so a later change to the base still reaches the variant.
   * dungeongen.js resolveDungeon(d, loadBase) is the resolver (D8). Without it
   * (a tree where D8 has not landed) a variant opens READ-ONLY through the
   * local resolver below, which reads the same fields for display only. */
  const canResolve = typeof DG.resolveDungeon === 'function';
  const stable = (v) => JSON.stringify(v, (k, x) => x && typeof x === 'object' && !Array.isArray(x) ? Object.keys(x).sort().reduce((o, kk) => (o[kk] = x[kk], o), {}) : x);
  const same = (a, b) => stable(a === undefined ? null : a) === stable(b === undefined ? null : b);
  const clone = (v) => v === undefined ? undefined : JSON.parse(JSON.stringify(v));
  async function rawOf(id) {
    const j = await api.get(id);
    return {raw: JSON.parse(j.dungeon), refs: j.refs};
  }
  async function localResolve(raw, depth) {
    if (!raw.extends) return clone(raw);
    if ((depth || 0) > 8) throw new Error('extends chain deeper than 8 (a cycle?)');
    const out = await localResolve((await rawOf(raw.extends)).raw, (depth || 0) + 1);
    delete out.extends;
    for (const k of ['name', 'seed', 'palette', 'style', 'entrance', 'props']) if (raw[k] !== undefined) out[k] = clone(raw[k]);
    const ov = raw.pieces && !Array.isArray(raw.pieces) ? raw.pieces : {};
    out.pieces = (out.pieces || []).filter(p => !(ov[p.id] && ov[p.id].remove)).map(p => {
      const o = ov[p.id];
      if (!o) return p;
      const q = Object.assign({}, p, clone(o));
      delete q.remove;
      q.params = Object.assign({}, p.params || {}, clone(o.params) || {});
      return q;
    });
    for (const p of raw.add || []) out.pieces.push(clone(p));
    return out;
  }
  async function flatten(raw) {
    const r = canResolve ? await DG.resolveDungeon(raw, async (bid) => (await rawOf(bid)).raw) : await localResolve(raw);
    const d = DG.normalizeDungeon(r);
    delete d.extends;
    return d;
  }
  function applyRefsPatch(refs, rp) {
    if (!rp) return refs;
    const gone = new Set(rp.remove || []);
    const out = refs.filter(r => !gone.has(r.name)).map(r => rp.set && rp.set[r.name] ? Object.assign({}, r, clone(rp.set[r.name])) : r);
    for (const r of rp.add || []) out.push(clone(r));
    return out;
  }
  /** The overrides of piece p against the base's piece b (null = none). */
  function pieceOverride(b, p) {
    if (!b || !p) return null;
    const o = {}, params = {};
    for (const k of new Set([...Object.keys(b.params || {}), ...Object.keys(p.params || {})]))
      if (!same((b.params || {})[k], (p.params || {})[k])) params[k] = clone(p.params[k]);
    if (Object.keys(params).length) o.params = params;
    for (const k of new Set([...Object.keys(b), ...Object.keys(p)])) {
      if (k === 'id' || k === 'params') continue;
      if (!same(b[k], p[k])) o[k] = clone(p[k]);
    }
    return Object.keys(o).length ? o : null;
  }
  function pieceState(id) {
    const b = S.variant && S.variant.baseFlat.pieces.find(x => x.id === id);
    if (!b) return 'added';
    return pieceOverride(b, piece(id)) ? 'overridden' : 'inherited';
  }
  function revertPiece(id) {
    const b = S.variant && S.variant.baseFlat.pieces.find(x => x.id === id);
    if (!b) return;
    edit('revert ' + id + ' to base', () => { const i = S.d.pieces.findIndex(x => x.id === id); if (i >= 0) S.d.pieces[i] = DG.normalizePiece(clone(b)); });
    renderInspector();
  }
  function refsPatchOf(base, refs) {
    const bn = new Map(base.map(r => [r.name, r])), rn = new Map(refs.map(r => [r.name, r]));
    const rp = {add: [], remove: [], set: {}};
    for (const r of base) if (!rn.has(r.name)) rp.remove.push(r.name);
    for (const r of refs) {
      const b = bn.get(r.name);
      if (!b) { rp.add.push(clone(r)); continue; }
      const d = {};
      for (const k of new Set([...Object.keys(b), ...Object.keys(r)])) if (!same(b[k], r[k])) d[k] = clone(r[k]);
      if (Object.keys(d).length) rp.set[r.name] = d;
    }
    if (!rp.add.length) delete rp.add;
    if (!rp.remove.length) delete rp.remove;
    if (!Object.keys(rp.set).length) delete rp.set;
    return Object.keys(rp).length ? rp : null;
  }
  /** The variant's dungeon.json object: the base id + what differs from it. */
  function variantRaw() {
    const V = S.variant, B = V.baseFlat, D = S.d;
    const RE = ['extends', 'name', 'seed', 'palette', 'style', 'entrance', 'props', 'pieces', 'add', 'refsPatch'];
    const o = {};
    for (const [k, v] of Object.entries(V.raw)) if (!RE.includes(k)) o[k] = v;     // version, refs, label...
    o.extends = V.base;
    for (const k of ['name', 'seed', 'palette', 'style', 'entrance', 'props']) if (!same(D[k], B[k])) o[k] = clone(D[k]);
    const pieces = {}, add = [], dp = new Set(D.pieces.map(p => p.id));
    for (const b of B.pieces) if (!dp.has(b.id)) pieces[b.id] = {remove: true};
    for (const p of D.pieces) {
      const b = B.pieces.find(x => x.id === p.id);
      if (!b) { add.push(clone(p)); continue; }
      const ov = pieceOverride(b, p);
      if (ov) pieces[p.id] = ov;
    }
    if (Object.keys(pieces).length) o.pieces = pieces;
    if (add.length) o.add = add;
    const rp = refsPatchOf(V.baseRefs, S.refs);
    if (rp) o.refsPatch = rp;
    if (S.refs.length && !o.refs) o.refs = 'refs.json';
    return o;
  }
  function variantSummary() {
    const V = S.variant;
    const st = S.d.pieces.map(p => [p.id, pieceState(p.id)]);
    const n = (s) => st.filter(x => x[1] === s).map(x => x[0]);
    const removed = V.baseFlat.pieces.filter(b => !S.d.pieces.some(p => p.id === b.id)).map(b => b.id);
    const rp = refsPatchOf(V.baseRefs, S.refs) || {};
    return h('div', {class: 'dg-hint'},
      (V.readOnly ? 'READ-ONLY here: this tree\'s dungeongen.js has no resolveDungeon (package D8\'s inheritance is not merged), so the variant is shown through a display-only resolver and cannot be saved. ' : '') +
      'Pieces: ' + n('inherited').length + ' inherited, ' + n('overridden').length + ' overridden' + (n('overridden').length ? ' (' + n('overridden').join(', ') + ')' : '') +
      ', ' + n('added').length + ' added' + (n('added').length ? ' (' + n('added').join(', ') + ')' : '') +
      ', ' + removed.length + ' removed' + (removed.length ? ' (' + removed.join(', ') + ')' : '') +
      '. Refs: +' + (rp.add || []).length + ' −' + (rp.remove || []).length + ' ~' + Object.keys(rp.set || {}).length +
      '. Saving writes only these OVERRIDES to dungeon.json (plus the compiled shell + refs); edit ' + V.base + ' itself to change what every variant inherits.');
  }
  async function newVariant() {
    if (S.variant) { toast('a variant of a variant: open ' + S.variant.base + ' or save this one first, then make the variant of it'); }
    if (!S.id || !S.list.some(x => x.id === S.id)) { toast('save the dungeon first: a variant extends a dungeon on disk', true); return; }
    if (dirty() && !confirm('The variant extends the SAVED ' + S.id + '; your unsaved changes stay out of it. Continue?')) return;
    if (!canResolve) { toast('this tree\'s dungeongen.js has no resolveDungeon (D8\'s dungeon inheritance): variants can be viewed here but not made', true); return; }
    const id = (prompt('New variant of ' + S.id + ': its id (assets/dungeons/<id>/):', S.id + '_b') || '').trim();
    if (!id) return;
    if (!ID_RE.test(id) || id === 'props' || id === 'presets' || S.list.some(x => x.id === id)) { toast('bad or existing id', true); return; }
    const base = S.id;
    let b;
    try { b = await rawOf(base); } catch (e) { toast('variant: ' + e.message, true); return; }
    const raw = {extends: base, name: (S.d.name || base) + ' (variant)'};
    try { await openVariant(id, raw, b); } catch (e) { toast('variant: ' + e.message, true); return; }
    S.saved = '';
    ui.listSel.replaceChildren(h('option', {value: id}, id + ' (new variant)'), ...S.list.map(x => h('option', {value: x.id}, x.id)));
    ui.listSel.value = id;
    renderHeader();
    toast('new variant ' + id + ' of ' + base + ': change what differs and Save (only the overrides are written)');
  }
  /** Load variant `id` whose raw dungeon.json is `raw`; `b` = its base's {raw, refs}. */
  async function openVariant(id, raw, b, ownRefs) {
    const baseFlat = await flatten(b.raw);
    const flat = await flatten(raw);
    let baseRefs = [];
    try { baseRefs = E.readRefs(b.refs).refs; } catch (e) { baseRefs = []; }
    let rr;
    try { rr = ownRefs ? E.readRefs(ownRefs) : {about: '', refs: applyRefsPatch(baseRefs, raw.refsPatch)}; }
    catch (e) { rr = {about: '', refs: applyRefsPatch(baseRefs, raw.refsPatch)}; }
    load(id, flat, rr);
    S.variant = {raw, base: raw.extends, baseFlat, baseRefs, readOnly: !canResolve};
    if (!canResolve) T.setStatus(id + ' extends ' + raw.extends + ': shown READ-ONLY (this tree has no dungeongen.js resolveDungeon)', true);
    renderInspector();
  }

  async function open(id) {
    if (!id) return;
    if (dirty() && (S.d.pieces.length || S.refs.length) && !confirm('Leave the unsaved changes to ' + (S.id || 'this dungeon') + '?')) { ui.listSel.value = S.id; return; }
    let j;
    try { j = await api.get(id); } catch (e) { toast('open ' + id + ': ' + e.message, true); return; }
    let d, rr;
    try {
      const raw = JSON.parse(j.dungeon);
      if (raw && raw.extends) {
        await openVariant(id, raw, await rawOf(raw.extends), j.refs);
        if (S.view === 'world') setViewMode(placementOf() ? 'world' : 'xray');
        return;
      }
    } catch (e) {
      toast(id + ': the variant does not resolve: ' + e.message, true);
      T.setStatus(id + ': variant does not resolve (' + e.message + ')', true);
      ui.listSel.value = S.id;
      return;
    }
    try { d = DG.normalizeDungeon(JSON.parse(j.dungeon)); }
    catch (e) {
      // e.g. D1's _fixture: a hand-written shell whose pieces name no kit
      toast(id + ' cannot be edited here: ' + e.message + ' (its shell.json is hand-written, not built from the kits)', true);
      T.setStatus(id + ': not built from the kits (' + e.message + ')', true);
      ui.listSel.value = S.id;
      return;
    }
    try { rr = E.readRefs(j.refs); } catch (e) { toast(id + '/refs.json does not parse: ' + e.message, true); rr = {about: '', refs: []}; }
    load(id, d, rr);
    if (S.view === 'world') setViewMode(placementOf() ? 'world' : 'xray');
  }
  function load(id, d, rr) {
    S.variant = null;          // openVariant sets it after the load
    S.id = id; S.d = d; S.refs = rr.refs; S.about = rr.about; S.sel = null; S.proposal = null; S.tool = null; showGhost();
    try { S.A = DG.compile(d, {}).entrance.dungeonPos.slice(); } catch (e) { S.A = [0, 0, 0]; }
    S.saved = sig(); S.framed = false; S.infoKey = ''; S.ymin = S.ymax = null;
    hist.clear();
    ui.listSel.value = id;
    for (const r of S.refs) if ((r.kind === 'npc' || r.kind === 'spawner') && r.props.schedule) loadSchedule(r.props.schedule);
    renderPalette();
    rebuild({voxels: true, fast: true});
  }
  function newDungeon() {
    if (dirty() && !confirm('Leave the unsaved changes?')) return;
    const id = (prompt('New dungeon id (assets/dungeons/<id>/: a-z 0-9 _ -):', 'new_dungeon') || '').trim();
    if (!id) return;
    if (!ID_RE.test(id) || id === 'props' || id === 'presets') { toast('an id is a-z 0-9 _ - (not props / presets)', true); return; }
    if (S.list.some(x => x.id === id)) { toast(id + ' already exists: open it from the list', true); return; }
    const style = (prompt('Style (caves / mines / ruins):', S.d.style || 'mines') || '').trim();
    const d = DG.newDungeon({name: id.replace(/_/g, ' '), style: DG.STYLES.includes(style) ? style : 'mines', seed: (Date.now() % 900000) + 1});
    load(id, d, {about: '', refs: []});
    S.saved = '';
    ui.listSel.replaceChildren(h('option', {value: id}, id + ' (new)'), ...S.list.map(x => h('option', {value: x.id}, x.id)));
    ui.listSel.value = id;
    renderHeader();
  }
  async function duplicate() {
    const id = (prompt('Save a copy as (new id):', (S.id || 'dungeon') + '_copy') || '').trim();
    if (!id) return;
    if (!ID_RE.test(id) || S.list.some(x => x.id === id)) { toast('bad or existing id', true); return; }
    S.id = id;
    await save();
    await refreshList();
  }
  function revert() {
    if (!S.id || !S.list.some(x => x.id === S.id)) { toast('nothing saved to revert to'); return; }
    if (dirty() && !confirm('Throw away the unsaved changes?')) return;
    S.saved = '';
    const id = S.id; S.id = '';
    open(id);
  }
  function b64(u8) { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); }
  async function save() {
    if (!S.id) { const id = (prompt('Save as (dungeon id):', 'new_dungeon') || '').trim(); if (!id || !ID_RE.test(id)) return; S.id = id; }
    if (S.proposal) { toast('keep or discard the grow proposal first', true); return; }
    if (S.variant && S.variant.readOnly) { toast(S.id + ' is a variant and this tree\'s dungeongen.js cannot resolve variants (no resolveDungeon): not saved', true); return; }
    if (S.refs.length && !S.d.refs) S.d.refs = 'refs.json';
    // A VARIANT writes its overrides, not the flattening (variantRaw); the
    // shell + refs it writes beside them are the flattened dungeon's, which is
    // what compile_dungeon.mjs resolves the overrides to (the --check below).
    const text = S.variant ? JSON.stringify(variantRaw(), null, 2) + '\n' : DG.dungeonJsonText(S.d);
    const prog = DG.compile(S.d, {id: S.id, sourceText: text, materials});
    const errs = S.issues.filter(i => i.severity === 'error');
    if ((prog.errors.length || errs.length) && !confirm((prog.errors.length + errs.length) + ' error(s) in the validation panel (first: ' + ((prog.errors[0] || errs[0]).msg) + ').\nThe engine refuses a shell over its limits. Save anyway?')) return;
    try {
      const r = await api.save(S.id, {dungeon: text, shellJson: DG.emitShellJson(prog), shellBin: b64(DG.emitShellBin(prog)),
                                       refs: S.refs.length || S.d.refs ? E.writeRefs(S.id, S.about, S.refs) : null});
      S.saved = sig();
      for (const n of [...S.schedDirty]) await saveSchedule(n);
      toast('saved assets/dungeons/' + S.id + '/ (' + r.written.join(', ') + ')' + (r.current === false ? ' -- BUT compile_dungeon.mjs --check says: ' + r.check : ''), r.current === false);
      T.setStatus(r.check || '');
      await refreshList();
      renderHeader();
    } catch (e) { toast('save failed: ' + e.message, true); }
  }

  /* ---- playtest ---- */
  async function playtest() {
    const pl = S.view === 'world' && S.world && !S.world.scratch ? S.world : placementOf();   // the placement on screen, on whichever map
    if (!S.id || !pl) { toast('Playtest needs the dungeon SAVED and PLACED on a map (Environment > World map > Dungeons tool)', true); return; }
    if (dirty()) { if (!confirm('Save first? The game loads the files on disk.')) return; await save(); }
    try {
      const info = await api.preview({mode: 'map', id: pl.entry.id, map: pl.map, src: 'dungeons/' + S.id + '/dungeon.json'});
      pv.forget(); S.infoKey = '';
      const yaw = info.yaw || 0, q = ((Math.round(yaw / 90) % 4) + 4) % 4;
      const dl = S.prog.entrance.dir;
      const rot = (x, z) => [[x, z], [z, -x], [-x, -z], [-z, x]][q];
      const dw = rot(dl[0], dl[2]);
      const a = info.anchor;
      let at, yawDeg, pitch;
      if (dl[1] !== 0) { at = [a[0] + 0.5, a[1] + 24, a[2] + 0.5]; yawDeg = 0; pitch = -70; }
      else { at = [a[0] + 0.5 + dw[0] * 26, a[1] + 30, a[2] + 0.5 + dw[1] * 26]; yawDeg = Math.atan2(-dw[1], -dw[0]) * 180 / Math.PI; pitch = -8; }
      const r = await api.play({mode: 'dungeon', at, yaw: yawDeg, pitch, map: pl.map});
      toast('launched the game (pid ' + r.pid + ') at the entrance of ' + S.id + ' on ' + pl.map + ': --at ' + at.map(v => v.toFixed(1)).join(','));
      schedulePreview(0);
    } catch (e) { toast('playtest: ' + e.message, true); }
  }

  /* =========================================================================
   * page API
   * ====================================================================== */
  let initP = null, opened = false;
  async function init() {
    build(); built = true;
    S.saved = sig();            // the untouched scratch dungeon is not "unsaved work"
    materials = await materialNames();
    try { S.cat = await api.catalog(); if (T.userPresets) T.userPresets(S.cat.presets || {}); } catch (e) { T.setStatus('catalog: ' + e.message, true); }
    await refreshList();
  }
  async function activate(opts) {
    if (!initP) initP = init();
    await initP;
    if (opts && opts.open && opts.open !== S.id) { opened = true; await open(opts.open); }
    else if (!opened) {
      opened = true;
      const first = (S.list.find(x => !x.id.startsWith('_')) || S.list[0] || {}).id;
      if (first) await open(first); else rebuild({voxels: true});
    } else if (pv.stale()) { S.infoKey = ''; schedulePreview(0); }
    if (opts && opts.world) setViewMode('world', typeof opts.world === 'object' ? opts.world : null);
    view.start();
    renderPalette();
  }
  function deactivate() { view.stop(); }
  /** A new dungeon without the prompts (the harness, scripts). */
  function create(id, style, name) {
    load(id, DG.newDungeon({name: name || id, style: style || 'mines', seed: 1}), {about: '', refs: []});
    S.saved = '';
    renderHeader();
  }
  return {activate, deactivate, view, S, ready, pv, open, save, attachAt, growAt, create, firstPiece, adoptWaynodes, adoptSuggestions, keepProposal, discardProposal, select, hist, placeRef, placeProp,
          refreshList, setViewMode, presetsChanged: () => { if (built) renderPalette(); }, dirty, compileNow, validateNow};
}
