/* ui_maplayer.js — the DUNGEONS layer of the World map page
 * (docs/PLAN_dungeons.md §5.3, package D4).
 *
 * map.json `dungeons[]` = {id, name, dungeon, at: [x, z], yOffset, yaw} (§3.1).
 * Drawn on the map: each placement's FOOTPRINT per level (every piece's
 * compiled bounds, turned by the placement's quarter turn about the anchor,
 * shaded by depth band), its ANCHOR badge and its depth range. The Dungeon
 * tool: click a badge to select it, drag it to move the anchor, R turns it a
 * quarter, click open ground to place the dungeon picked in the tool strip,
 * Delete removes it. The panel edits id / name / dungeon / at / yOffset /
 * yaw and opens the dungeon in the Dungeons tab (its "in world" view shows
 * the real terrain round the entrance). Everything is map.json: one undo
 * step per change, saved with the map.
 *
 * The footprint comes from the dungeon's SAVED shell.json (pieces[].bounds,
 * anchor frame), fetched once per dungeon through /api/dungeon.
 */
'use strict';

const ID_RE = /^[a-z0-9_-]+$/;
// 3.4.1 R(q): (x, z) -> q1 (z, -x), q2 (-x, -z), q3 (-z, x)
const rotQ = (q, x, z) => [[x, z], [z, -x], [-x, -z], [-z, x]][q & 3];

export function createDungeonLayer(C) {
  const st = {sel: null, drag: null, list: [], shells: new Map(), pick: '', loading: new Set()};
  const els = {};
  const J = () => C.map().json;
  const entries = () => (C.map() && Array.isArray(J().dungeons)) ? J().dungeons : [];
  const q = (e) => ((Math.round((e.yaw || 0) / 90) % 4) + 4) % 4;

  async function loadList() {
    try {
      const r = await fetch('/api/dungeons', {cache: 'no-store'});
      const j = await r.json();
      st.list = (j.dungeons || []).filter(d => !d.id.startsWith('_') || d.id === '_fixture' || d.id === '_d2_sample');
      if (!st.pick && st.list.length) st.pick = (st.list.find(d => !d.id.startsWith('_')) || st.list[0]).id;
      syncOpts();
    } catch (e) { /* no server: the layer still draws what the map says */ }
  }
  function shellOf(dungeon) {
    if (st.shells.has(dungeon)) return st.shells.get(dungeon);
    if (!st.loading.has(dungeon)) {
      st.loading.add(dungeon);
      fetch('/api/dungeon?id=' + encodeURIComponent(dungeon), {cache: 'no-store'}).then(r => r.json()).then(j => {
        let s = null;
        try { s = j.ok && j.shell ? JSON.parse(j.shell) : null; } catch (e) { s = null; }
        st.shells.set(dungeon, s ? {pieces: s.pieces || [], bounds: s.bounds, name: s.name} : {pieces: [], bounds: null, missing: !j.ok});
        C.paint();
      }).catch(() => st.shells.set(dungeon, {pieces: [], bounds: null, missing: true})).finally(() => st.loading.delete(dungeon));
    }
    return null;
  }
  /** World xz rect of a local inclusive box under placement e. */
  function rectOf(e, mn, mx) {
    const a = rotQ(q(e), mn[0], mn[2]), b = rotQ(q(e), mx[0], mx[2]);
    return [e.at[0] + Math.min(a[0], b[0]), e.at[1] + Math.min(a[1], b[1]), e.at[0] + Math.max(a[0], b[0]) + 1, e.at[1] + Math.max(a[1], b[1]) + 1];
  }

  /* ---- painting ---- */
  const BAND = ['#ffd27a', '#e8a35a', '#c07a52', '#9a6a8a', '#6a6aa8', '#4a6ac0'];
  function paint(g) {
    if (!C.map()) return;
    g.save();
    g.font = '11px monospace';
    for (const e of entries()) {
      if (!Array.isArray(e.at)) continue;
      const sel = st.sel === e.id;
      const sh = shellOf(e.dungeon || e.id);
      if (sh && sh.pieces.length) {
        const ys = [...new Set(sh.pieces.map(p => Math.round(p.bounds.min[1] / 24)))].sort((a, b) => b - a);
        for (const p of sh.pieces) {
          const band = ys.indexOf(Math.round(p.bounds.min[1] / 24));
          const r = rectOf(e, p.bounds.min, p.bounds.max);
          const x0 = C.SX(r[0]), z0 = C.SZ(r[1]), w = (r[2] - r[0]) * C.view.scale, hh = (r[3] - r[1]) * C.view.scale;
          g.globalAlpha = sel ? 0.30 : 0.16; g.fillStyle = BAND[Math.min(BAND.length - 1, band)];
          g.fillRect(x0, z0, Math.max(1, w), Math.max(1, hh));
          g.globalAlpha = sel ? 0.95 : 0.5; g.strokeStyle = p.surface ? '#9cf0c8' : BAND[Math.min(BAND.length - 1, band)]; g.lineWidth = 1;
          g.strokeRect(Math.round(x0) + 0.5, Math.round(z0) + 0.5, Math.round(w), Math.round(hh));
        }
        g.globalAlpha = 1;
      }
      // the anchor badge: a pixel "D" door
      const x = Math.round(C.SX(e.at[0])), z = Math.round(C.SZ(e.at[1]));
      const ring = sel ? '#ffd866' : '#d08aff';
      g.fillStyle = ring; g.fillRect(x - 11, z - 24, 22, 22);
      g.fillStyle = '#120d1a'; g.fillRect(x - 10, z - 23, 20, 20);
      g.fillStyle = ring; g.fillRect(x - 5, z - 19, 10, 14); g.fillStyle = '#120d1a'; g.fillRect(x - 3, z - 17, 6, 12);
      g.fillStyle = ring; g.fillRect(x - 1, z - 2, 3, 3);
      // facing tick (the placement's +Z: heading = yaw)
      const f = rotQ(q(e), 0, 1);
      g.fillRect(x + f[0] * 14 - 1, z + f[1] * 14 - 1, 3, 3);
      const depth = sh && sh.bounds ? '  depth ' + (-sh.bounds.min[1] / 10).toFixed(1) + ' m' + (sh.bounds.max[1] > 4 ? ', up ' + (sh.bounds.max[1] / 10).toFixed(1) + ' m' : '') : sh && sh.missing ? ' (no shell)' : '';
      g.fillStyle = sel ? '#ffd866' : '#ead6ff';
      g.fillText((e.name || e.id) + depth + (e.yOffset ? ' yOff ' + e.yOffset : ''), x + 14, z - 12);
    }
    g.restore();
  }

  /* ---- events ---- */
  function badgeAt(fx, fz) {
    const s = C.view.scale;
    for (const e of entries()) {
      if (!Array.isArray(e.at)) continue;
      const dx = (fx - e.at[0]) * s, dz = (fz - e.at[1]) * s;
      if (dx >= -12 && dx <= 12 && dz >= -26 && dz <= 2) return e;
    }
    return null;
  }
  function edit(fn) { C.pushUndo(C.snapshot()); fn(); C.markDirty(true); sync(); C.paint(); }
  function down(ev, wx, wz) {
    if (!C.map()) return false;
    const [fx, fz] = C.worldAtF(ev);
    const hit = badgeAt(fx, fz);
    if (hit) {
      st.sel = hit.id;
      st.drag = {id: hit.id, snap: C.snapshot(), x0: hit.at[0], z0: hit.at[1], fx, fz, moved: false};
      sync(); C.paint();
      return true;
    }
    if (!st.pick) { C.toast('no dungeon to place: build one in the Dungeons tab first', true); return true; }
    const used = new Set(entries().map(e => e.id));
    let id = st.pick.replace(/^_+/, '') || 'dungeon';
    for (let i = 2; used.has(id); i++) id = st.pick.replace(/^_+/, '') + '_' + i;
    const d = st.list.find(x => x.id === st.pick);
    const e = {id, name: d ? d.name : id, dungeon: st.pick, at: [Math.round(wx), Math.round(wz)], yOffset: 0, yaw: 0};
    edit(() => { if (!Array.isArray(J().dungeons)) J().dungeons = []; J().dungeons.push(e); st.sel = id; });
    C.status('placed dungeon "' + id + '" (' + st.pick + ') at ' + e.at.join(', ') + ': drag the badge to move it, R turns it; save the map');
    return true;
  }
  function move(ev) {
    const d = st.drag;
    if (!d) return;
    const [fx, fz] = C.worldAtF(ev);
    const e = entries().find(x => x.id === d.id);
    if (!e) return;
    e.at = [Math.round(d.x0 + fx - d.fx), Math.round(d.z0 + fz - d.fz)];
    d.moved = true;
    C.paint();
  }
  function up() {
    const d = st.drag;
    st.drag = null;
    if (d && d.moved) { C.pushUndo(d.snap); C.markDirty(true); sync(); }
  }
  function onKey(ev) {
    const e = entries().find(x => x.id === st.sel);
    if (!e) return false;
    if (ev.key === 'r' || ev.key === 'R') { edit(() => { e.yaw = (((e.yaw || 0) + (ev.shiftKey ? -90 : 90)) % 360 + 360) % 360; }); return true; }
    if (ev.key === 'Delete') { edit(() => { J().dungeons = entries().filter(x => x !== e); st.sel = null; }); return true; }
    return false;
  }

  /* ---- the panel + the tool strip ---- */
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'mapdungeons'});
    det.append(el('summary', {}, 'Dungeons — placed dungeons (map.json dungeons[])'));
    els.body = el('div', {class: 'maptools-panel'});
    det.append(els.body);
    loadList();
    return det;
  }
  function syncOpts() {
    if (!els.pickSel) return;
    const el = C.el;
    els.pickSel.replaceChildren(...(st.list.length ? st.list.map(d => el('option', {value: d.id}, d.id + (d.name && d.name !== d.id ? ' — ' + d.name : ''))) : [el('option', {value: ''}, '(no dungeons)')]));
    els.pickSel.value = st.pick;
  }
  function opts() {
    const el = C.el;
    els.pickSel = el('select', {title: 'the dungeon a click on open ground places'});
    els.pickSel.addEventListener('change', () => { st.pick = els.pickSel.value; });
    syncOpts();
    return el('span', {class: 'mapopts'}, 'dungeon ', els.pickSel,
              el('span', {class: 'mapnote'}, ' · click open ground places it · drag a badge moves its anchor · R turns · Delete removes'));
  }
  function sync() {
    if (!els.body) return;
    const el = C.el, list = entries();
    const rows = [];
    if (!list.length) rows.push(el('div', {class: 'mapnote'}, 'No dungeons on this map. Pick the Dungeon tool, choose one in the strip above the map and click where its entrance goes.'));
    for (const e of list) {
      const b = el('div', {class: 'mapsiterow' + (st.sel === e.id ? ' sel' : ''), style: 'cursor:pointer;font-family:monospace;font-size:12px;padding:1px 4px' + (st.sel === e.id ? ';background:#1d3a6e' : '')},
        (e.name || e.id) + '  ·  ' + (e.dungeon || e.id) + '  ·  ' + (e.at || []).join(', ') + '  yaw ' + (e.yaw || 0));
      b.addEventListener('click', () => { st.sel = e.id; C.setTool('dungeon'); sync(); C.paint(); });
      rows.push(b);
    }
    const e = list.find(x => x.id === st.sel);
    if (e) {
      const num = (v, on, w = 80) => { const i = el('input', {type: 'number', value: v, style: 'width:' + w + 'px'}); i.addEventListener('change', () => on(Math.round(+i.value || 0))); return i; };
      const txt = (v, on) => { const i = el('input', {type: 'text', value: v || '', style: 'width:150px'}); i.addEventListener('change', () => on(i.value.trim())); return i; };
      const dsel = el('select', {}, ...st.list.map(d => el('option', {value: d.id}, d.id)));
      dsel.value = e.dungeon || e.id;
      dsel.addEventListener('change', () => edit(() => { e.dungeon = dsel.value; }));
      const ysel = el('select', {}, ...[0, 90, 180, 270].map(y => el('option', {value: String(y)}, y + '°')));
      ysel.value = String(e.yaw || 0);
      ysel.addEventListener('change', () => edit(() => { e.yaw = +ysel.value; }));
      const openBtn = el('button', {title: 'the Dungeons tab, in world: the real terrain round the entrance, the dungeon x-rayed beneath (save the map first)'}, 'Open in Dungeons tab');
      openBtn.addEventListener('click', () => {
        if (C.check && C.savedJson && JSON.stringify(C.map().json.dungeons || []) !== JSON.stringify((C.savedJson() || {}).dungeons || [])) C.toast('the map has unsaved dungeon changes: the Dungeons tab reads the SAVED map.json', true);
        // {map, id}: THIS map's entry, whichever map the page is on (the tab shows that placement in world).
        if (window.__tunerDungeons) window.__tunerDungeons.open(e.dungeon || e.id, {world: {map: C.map().name, id: e.id}}); else C.toast('the Dungeons tab did not load', true);
      });
      const del = el('button', {}, 'Delete');
      del.addEventListener('click', () => edit(() => { J().dungeons = entries().filter(x => x !== e); st.sel = null; }));
      const lbl = (t, x) => el('label', {style: 'display:block;margin:2px 0'}, t + ' ', x);
      rows.push(el('div', {style: 'border-top:1px solid #2a3240;margin-top:4px;padding-top:4px'},
        lbl('id', txt(e.id, v => { if (!ID_RE.test(v) || entries().some(x => x !== e && x.id === v)) { C.toast('a placement id is a-z 0-9 _ - and unique', true); sync(); return; } edit(() => { e.id = v; st.sel = v; }); })),
        lbl('name', txt(e.name, v => edit(() => { e.name = v; }))),
        lbl('dungeon', dsel),
        lbl('at x', num(e.at[0], v => edit(() => { e.at = [v, e.at[1]]; }))), lbl('at z', num(e.at[1], v => edit(() => { e.at = [e.at[0], v]; }))),
        lbl('yOffset', num(e.yOffset || 0, v => edit(() => { e.yOffset = v; }), 70)), lbl('yaw', ysel),
        el('div', {class: 'mapnote'}, 'The entrance socket sits at the bare ground at `at` + yOffset (voxels); yaw turns the whole dungeon about it. Underground pieces must stay under the ground: the Dungeons tab’s validation checks it against the real heights.'),
        el('div', {}, openBtn, ' ', del)));
    }
    els.body.replaceChildren(...rows);
  }
  function afterRestore() { if (st.sel && !entries().some(e => e.id === st.sel)) st.sel = null; st.drag = null; sync(); }

  C.registerTool({
    key: 'dungeon', label: 'Dungeon', group: 'locale', order: 40,
    title: 'DUNGEONS (map.json dungeons[]): click open ground to place the dungeon picked in the strip; drag a badge to move its anchor; R turns it a quarter; Delete removes; the footprint per level is drawn from its saved shell',
    opts, down, move, up, paint, onKey,
    guide: ['Dungeon — where a dungeon\'s entrance meets the world',
      'A dungeon (assets/dungeons/<id>/, built in the Dungeons tab) is carved into the world by worldgen; this places it. Its ENTRANCE socket sits at the bare ground of the anchor column + yOffset, and the whole dungeon turns about it by yaw (quarter turns).',
      'Click open ground: the dungeon picked in the strip is placed there. Drag a badge to move its anchor, R / Shift+R turns it, Delete removes it. The panel edits the numbers and opens the dungeon in the Dungeons tab (in world: the real terrain round the entrance).',
      'The footprint is each piece\'s box from the SAVED shell.json, shaded by depth band; recompile (save in the Dungeons tab) to refresh it. Saving the map moves the world hash (a dungeon is worldgen input).']
  });

  return {st, buildPanel, sync, afterRestore, paint, loadList, entries, ctx: C, down, move, up, onKey,
          refresh: () => { st.shells.clear(); loadList(); C.paint(); }};
}
