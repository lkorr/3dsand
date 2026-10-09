/* maprange.js — THE RANGE TOOL of the World map page (docs/PLAN_worldbuilder.md
 * K1 item 5): a mountain range drawn as a SPLINE. Each knot carries the
 * crest height there (absolute voxels, shown in metres above the sea) and the
 * range's HALF-WIDTH (crest to foot); the range's own sliders say how much
 * peak-and-saddle variation the crest gets, how strong the spurs are, how far
 * the foothills reach, how much the stream-power erosion cuts it, how much the
 * crest wanders. terraingen.js makes the ground (rangeField / rangeBake): a
 * designed envelope grown into a dendritic, eroded range.
 *
 * The range lives in map.json `ranges[]` (EDITOR-ONLY, docs/PLAN_worldbuilder.md
 * §2.4: the engine reads only the posts a bake wrote):
 *   {id, name, points: [[x, z, crestY, halfWidth], ...], profile, seed,
 *    peaks, spurs, foothills, erode, wander, baked}
 * Drawing: click to add knots (double-click / Enter ends), drag a knot, drag
 * a knot's WIDTH HANDLE (the small diamond beside it) to set its half-width,
 * click the selected range's crest to insert a knot, shift+click a knot to
 * delete it. Every change runs a LIVE PREVIEW (a coarse run in the worker,
 * drawn over the map); Bake writes it into the height plane (one undo step,
 * in a worker with a progress bar); Rebake / Unbake work from this session's
 * bake record, like the Line tool's. */

import * as MT from './maptiles.js';
import * as TG from './terraingen.js';
import * as SPL from './spline.js';

export function createRangeLayer(C) {
  const st = {sel: null, drawing: false, drag: null, live: true, timer: 0, bakes: new Map(), previewOf: null, pjob: null};
  // what a NEW range / knot takes (metres; the sliders are percents)
  const NEW = {crestM: 1500, widthM: 3000, profile: 'alpine', seed: 1, peaks: 60, spurs: 65, foothills: 50, erode: 70, wander: 50};
  const els = {};
  const J = () => C.map().json;
  const ranges = () => (J().ranges || (J().ranges = []));
  const selObj = () => (st.sel ? ranges().find(r => r.id === st.sel) || null : null);
  const v = () => C.vpm(), sea = () => C.seaY();
  const px = d => d * C.view.scale;
  const knots = r => (r.points || []).filter(p => Array.isArray(p) && p.length >= 2);
  const tessCache = new WeakMap();
  function tess(r) {
    const key = JSON.stringify(r.points) + '|' + Math.round(Math.log2(Math.max(1e-6, C.view.scale)));
    const c = tessCache.get(r);
    if (c && c.key === key) return c.t;
    const t = SPL.tessellate(knots(r).map(p => [p[0], p[1], +p[2] || 0, +p[3] || 0]), {step: Math.max(64, 6 / Math.max(1e-6, C.view.scale))});
    tessCache.set(r, {key, t});
    return t;
  }
  function freshId() {
    const rs = ranges();
    let n = rs.length, id = 'range_' + n;
    while (rs.some(r => r.id === id)) id = 'range_' + (++n);
    return id;
  }
  /** The right-hand normal of the range at knot k (unit). */
  function normalAt(r, k) {
    const P = knots(r);
    if (P.length < 2) return [0, 1];
    const a = P[Math.max(0, k - 1)], b = P[Math.min(P.length - 1, k + 1)];
    const dx = b[0] - a[0], dz = b[1] - a[1], L = Math.hypot(dx, dz) || 1;
    return [-dz / L, dx / L];
  }
  const handleAt = (r, k) => { const p = knots(r)[k], [nx, nz] = normalAt(r, k); return [p[0] + nx * p[3], p[1] + nz * p[3]]; };
  function edited(r) { if (r.baked) r.baked = false; schedulePreview(); }
  function edit(fn, r = selObj()) {
    C.pushUndo(C.snapshot());
    fn();
    if (r) edited(r);
    C.markDirty(true); sync(); C.paint();
  }
  const newKnot = (wx, wz, prev) => [Math.round(wx), Math.round(wz), prev ? prev[2] : Math.round(sea() + NEW.crestM * v()), prev ? prev[3] : Math.round(NEW.widthM * v())];

  /* ---- pointer ---------------------------------------------------------------------- */
  function down(ev, fx, fz) {
    const wx = Math.round(fx), wz = Math.round(fz), grab = 7;
    const r0 = selObj();
    // 1. a width handle of the selected range
    if (r0) for (let k = 0; k < knots(r0).length; k++) {
      const [hx, hz] = handleAt(r0, k);
      if (Math.hypot(px(hx - fx), px(hz - fz)) <= grab) { st.drag = {r: r0, k, kind: 'width', snap: C.snapshot(), moved: false}; return true; }
    }
    // 2. a knot (the selected range's first)
    const order = r0 ? [r0, ...ranges().filter(r => r !== r0)] : ranges();
    for (const r of order)
      for (let k = 0; k < knots(r).length; k++) {
        const p = knots(r)[k];
        if (Math.hypot(px(p[0] - fx), px(p[1] - fz)) > grab) continue;
        if (ev.shiftKey) {
          edit(() => {
            r.points.splice(r.points.indexOf(p), 1);
            if (!knots(r).length) { ranges().splice(ranges().indexOf(r), 1); if (st.sel === r.id) { st.sel = null; st.drawing = false; } }
          }, r);
          return true;
        }
        if (st.drawing && r === r0 && k === knots(r).length - 1 && k > 0) { finish(); return true; }
        st.sel = r.id;
        // Alt+drag: the knot's CREST HEIGHT follows the mouse up / down (5 m a pixel)
        st.drag = {r, k, kind: ev.altKey ? 'crest' : 'knot', snap: C.snapshot(), moved: false, y0: ev.clientY, c0: p[2]};
        sync(); C.paint();
        return true;
      }
    // 3. drawing: the next knot
    if (st.drawing && r0) {
      edit(() => { const P = knots(r0); r0.points.push(newKnot(wx, wz, P[P.length - 1])); }, r0);
      return true;
    }
    // 4. on the selected range's crest: insert a knot there
    if (r0 && knots(r0).length >= 2) {
      const c = SPL.closest(tess(r0), fx, fz);
      if (c && px(c.dist) <= 6) {
        edit(() => {
          const ins = SPL.insertKnot(r0.points.map(p => [p[0], p[1], p[2], p[3]]), c.s, {step: 64});
          r0.points = ins.points.map(p => [Math.round(p[0]), Math.round(p[1]), Math.round(p[2]), Math.round(p[3])]);
          st.drag = {r: r0, k: ins.index, kind: 'knot', snap: null, moved: false};
        }, r0);
        return true;
      }
    }
    // 5. another range's crest: select it
    for (const r of ranges()) {
      if (knots(r).length < 2) continue;
      const c = SPL.closest(tess(r), fx, fz);
      if (c && px(c.dist) <= 6) { st.sel = r.id; st.drawing = false; sync(); C.paint(); schedulePreview(); return true; }
    }
    // 6. open map: a new range, drawing
    C.pushUndo(C.snapshot());
    const r = {id: freshId(), name: '', profile: NEW.profile, seed: NEW.seed, peaks: NEW.peaks / 100, spurs: NEW.spurs / 100,
               foothills: NEW.foothills / 100, erode: NEW.erode / 100, wander: NEW.wander / 100, points: [newKnot(wx, wz, null)]};
    ranges().push(r);
    st.sel = r.id; st.drawing = true;
    C.markDirty(true); sync(); C.paint();
    C.status(`new range ${r.id}: click along the crest to add knots (double-click / Enter ends); drag a knot's diamond to set its half-width; Bake in the Ranges panel`);
    return true;
  }
  function move(ev, fx, fz) {
    const d = st.drag;
    if (!d) return;
    const p = knots(d.r)[d.k];
    if (!p) return;
    if (d.kind === 'knot') { p[0] = Math.round(fx); p[1] = Math.round(fz); }
    else if (d.kind === 'crest') p[2] = Math.round(d.c0 + (d.y0 - ev.clientY) * 5 * v());
    else p[3] = Math.max(Math.round(C.map().H.sp * 4), Math.round(Math.hypot(fx - p[0], fz - p[1])));
    d.moved = true;
    if (d.r.baked) d.r.baked = false;
    C.markDirty(true);
    if (!d.redraw) { d.redraw = true; requestAnimationFrame(() => { d.redraw = false; syncInspector(); }); }
  }
  function up() {
    const d = st.drag;
    if (!d) return;
    st.drag = null;
    if (d.moved) { if (d.snap) C.pushUndo(d.snap); C.markDirty(true); edited(d.r); }
    sync();
  }
  function finish() {
    const r = selObj();
    st.drawing = false;
    if (r) {
      const P = knots(r);
      // a double click's second click put a knot on top of the last: drop it
      if (P.length >= 2 && Math.hypot(px(P[P.length - 1][0] - P[P.length - 2][0]), px(P[P.length - 1][1] - P[P.length - 2][1])) < 4) r.points.splice(r.points.indexOf(P[P.length - 1]), 1);
    }
    sync(); C.paint(); schedulePreview();
  }

  /* ---- preview + bake ---------------------------------------------------------------- */
  function schedulePreview() {
    clearTimeout(st.timer);
    if (!st.live) return;
    st.timer = setTimeout(runPreview, 350);
  }
  function runPreview() {
    const r = selObj();
    if (!r || knots(r).length < 2 || !C.map()) { if (C.preview() && C.preview().owner === 'range') C.setPreview(null); return; }
    const box = TG.rangeBox(r), sp = C.map().H.sp;
    // ~51.2 m cells, at most ~90k of them
    const wv = (box[2] - box[0]) / sp, hv = (box[3] - box[1]) / sp;
    let f = Math.max(1, Math.round(512 / sp));
    while ((wv / f) * (hv / f) > 90000) f *= 2;
    const g = C.coarseGrid(box, f);
    if (!g) return;
    const job = {job: 'range', mode: 'preview', range: JSON.parse(JSON.stringify(r)), grid: g.grid, seaY: sea(), iters: 70};
    const run = C.runJob(job, {quiet: true, key: 'range-preview'});
    st.pjob = run;
    run.then(res => {
      if (run.cancelled || st.sel !== r.id) return;
      C.setPreview(res, `${r.id} (live, ${(g.grid.cell / v()).toFixed(0)} m cells: Bake writes it at 6.4 m)`, 'range');
    }).catch(e => { if (!run.cancelled) C.status('range preview: ' + e.message); });
  }
  const bakeSwap = (id, rec) => ({swap() {
    const cur = st.bakes.get(id) || null;
    if (rec) st.bakes.set(id, rec); else st.bakes.delete(id);
    sync();
    return bakeSwap(id, cur);
  }});
  async function bake(r) {
    if (!r || knots(r).length < 2) { C.toast('a range needs two knots', true); return; }
    if (C.jobBusy()) { C.toast('another terrain job is running', true); return; }
    const P = C.map().H, box = TG.rangeBox(r);
    const g = C.postsGrid(box);
    if (!g) { C.toast('the range is outside the map', true); return; }
    if (C.rectBlocked(g.rect)) { C.toast('tiles under the range are still loading; try again in a moment', true); return; }
    if (C.pending()) C.commitPending();
    const snap = C.snapshot(), undoTiles = new Map(), prev = st.bakes.get(r.id) || null;
    // a REBAKE: this session's last bake comes out first (strokes since stay as offsets)
    let since = 0;
    if (prev) since = MT.unbake(P, prev, undoTiles);
    g.grid.h = C.readPosts(...g.rect);
    let run;
    try { run = C.runJob({job: 'range', mode: 'bake', range: JSON.parse(JSON.stringify(r)), grid: g.grid, seaY: sea()}, {label: (prev ? 'rebaking ' : 'baking ') + r.id}); }
    catch (e) { if (prev) MT.swapTiles(P, undoTiles); C.toast(e.message, true); return; }
    const t0 = performance.now();
    let res;
    try { res = await run; }
    catch (e) { if (prev) MT.swapTiles(P, undoTiles); if (!run.cancelled) C.toast('range bake: ' + e.message, true); C.paint(); return; }
    if (run.cancelled || !C.map() || C.map().H !== P) { if (prev && C.map() && C.map().H === P) MT.swapTiles(P, undoTiles); C.paint(); return; }
    const h = res.h;
    for (let n = 0; n < h.length; n++) if (!res.changed[n]) h[n] = NaN;
    const opBefore = new Map();
    const w = MT.writeValues(P, ...g.rect, h, opBefore);
    for (const [k, t] of opBefore) if (!undoTiles.has(k)) undoTiles.set(k, t);
    st.bakes.set(r.id, {before: opBefore, after: MT.snapTiles(P, opBefore.keys())});
    r.baked = true;
    C.pushUndo({tiles: {height: undoTiles}, json: snap.json, custom: bakeSwap(r.id, prev)});
    C.planeChanged(); C.markDirty(true);
    if (C.preview() && C.preview().owner === 'range') C.setPreview(null);
    sync(); C.paint();
    C.status(`${prev ? 'rebaked' : 'baked'} ${r.id}: ${w.changed} posts over ${opBefore.size} tile(s) in ${((performance.now() - t0) / 1000).toFixed(1)} s (proxy ${res.factor * P.sp / v()} m)` +
             (prev && since ? `; ${since} posts painted since the last bake kept as offsets` : '') +
             (w.skipped ? `; ${w.skipped} posts in tiles still loading were SKIPPED (undo and bake again)` : '') + ' — Ctrl+Z takes it back');
  }
  function unbake(r) {
    const prev = st.bakes.get(r.id);
    if (!prev) return;
    const snap = C.snapshot(), undoTiles = new Map();
    const since = MT.unbake(C.map().H, prev, undoTiles);
    st.bakes.delete(r.id);
    r.baked = false;
    C.pushUndo({tiles: {height: undoTiles}, json: snap.json, custom: bakeSwap(r.id, prev)});
    C.planeChanged(); C.markDirty(true); sync(); C.paint();
    C.status(`unbaked ${r.id}` + (since ? ` (${since} posts painted since kept as offsets)` : ''));
  }

  /* ---- drawing ------------------------------------------------------------------------- */
  function paint(ctx) {
    const rs = (C.map() && J().ranges) || [];
    if (!rs.length) return;
    const SX = C.SX, SZ = C.SZ;
    ctx.font = '11px monospace';
    for (const r of rs) {
      const P = knots(r);
      if (!P.length) continue;
      const sel = r.id === st.sel, T = tess(r);
      const colr = '#e8a060';
      // the footprint: both flanks' feet (the half-width), faint
      if (T.pts.length >= 2) {
        ctx.strokeStyle = sel ? 'rgba(232,160,96,0.55)' : 'rgba(232,160,96,0.25)'; ctx.lineWidth = 1; ctx.setLineDash([2, 4]);
        for (const side of [1, -1]) {
          ctx.beginPath();
          T.pts.forEach((p, i) => {
            const a = T.pts[Math.max(0, i - 1)], b = T.pts[Math.min(T.pts.length - 1, i + 1)];
            const dx = b[0] - a[0], dz = b[1] - a[1], L = Math.hypot(dx, dz) || 1;
            const x = p[0] - dz / L * p[3] * side, z = p[1] + dx / L * p[3] * side;
            if (i) ctx.lineTo(SX(x), SZ(z)); else ctx.moveTo(SX(x), SZ(z));
          });
          ctx.stroke();
        }
        ctx.setLineDash([]);
        // the crest: solid when the plane holds it, dashed when not (or no longer) baked
        ctx.strokeStyle = sel ? '#ffd0a0' : colr; ctx.lineWidth = sel ? 2.5 : 1.5;
        if (!r.baked) ctx.setLineDash([6, 3]);
        ctx.beginPath();
        T.pts.forEach((p, i) => (i ? ctx.lineTo(SX(p[0]), SZ(p[1])) : ctx.moveTo(SX(p[0]), SZ(p[1]))));
        ctx.stroke();
        ctx.setLineDash([]);
      }
      // knots: pixel triangles (peaks), crest heights beside them when selected
      P.forEach((p, k) => {
        const x = Math.round(SX(p[0])), z = Math.round(SZ(p[1]));
        ctx.fillStyle = sel ? '#ffffff' : colr;
        ctx.beginPath(); ctx.moveTo(x, z - 5); ctx.lineTo(x + 5, z + 4); ctx.lineTo(x - 5, z + 4); ctx.closePath(); ctx.fill();
        ctx.fillStyle = '#0e1219'; ctx.fillRect(x - 1, z, 3, 2);
        if (sel) {
          ctx.fillStyle = colr; ctx.fillText(C.fmtM((p[2] - sea()) / v()), x + 7, z - 4);
          const [hx, hz] = handleAt(r, k), hxs = Math.round(SX(hx)), hzs = Math.round(SZ(hz));
          ctx.strokeStyle = 'rgba(255,208,160,0.6)'; ctx.lineWidth = 1;
          ctx.beginPath(); ctx.moveTo(x, z); ctx.lineTo(hxs, hzs); ctx.stroke();
          ctx.fillStyle = '#ffd0a0';
          ctx.beginPath(); ctx.moveTo(hxs, hzs - 4); ctx.lineTo(hxs + 4, hzs); ctx.lineTo(hxs, hzs + 4); ctx.lineTo(hxs - 4, hzs); ctx.closePath(); ctx.fill();
        }
      });
      ctx.fillStyle = colr;
      ctx.fillText(r.id + (r.name ? ' “' + r.name + '”' : '') + (r.baked ? '' : ' (not baked)'), SX(P[0][0]) + 8, SZ(P[0][1]) + 14);
    }
  }
  function cursor(ctx, wx, wz) {
    if (st.drag) return;
    const r = selObj(), SX = C.SX, SZ = C.SZ;
    ctx.fillStyle = '#ffd866'; ctx.font = '11px monospace';
    const prev = r && st.drawing ? knots(r)[knots(r).length - 1] : null;
    const crest = prev ? (prev[2] - sea()) / v() : NEW.crestM, wid = prev ? prev[3] / v() : NEW.widthM;
    ctx.strokeStyle = 'rgba(255,208,160,0.5)'; ctx.setLineDash([2, 3]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(SX(wx), SZ(wz), Math.max(2, wid * v() * C.view.scale), 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]);
    if (prev) { ctx.beginPath(); ctx.moveTo(SX(prev[0]), SZ(prev[1])); ctx.lineTo(SX(wx), SZ(wz)); ctx.stroke(); }
    ctx.fillText(`${st.drawing ? 'next knot' : r ? 'click the crest to insert, open map for a new range' : 'new range'}: crest ${C.fmtM(crest)} · half-width ${C.fmtM(wid)}`, SX(wx) + 10, SZ(wz) - 10);
  }

  /* ---- the panel ---------------------------------------------------------------------- */
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'maptools', open: true});
    det.append(el('summary', {}, 'Ranges — mountain ranges from a spline (map.json ranges[]: the plane holds what is BAKED)'));
    els.list = el('div', {class: 'mapsites-list'});
    els.insp = el('div', {class: 'maptools-panel'});
    det.append(els.list, els.insp);
    return det;
  }
  function optsStrip() {
    const el = C.el;
    const num = (key, min, max, step, w, title) => {
      const i = el('input', {type: 'number', min, max, step, value: NEW[key], style: `width:${w}px`, title});
      i.addEventListener('change', () => { NEW[key] = Math.max(min, Math.min(max, +i.value || 0)); i.value = NEW[key]; });
      return i;
    };
    const prof = el('select', {title: 'a NEW range’s cross-section: alpine (steep upper walls, concave flanks), sharp (knife edge), round (old, worn), massif (a broad high block)'},
                    ...TG.RANGE_PROFILE_NAMES.map(n => el('option', {value: n}, n)));
    prof.value = NEW.profile;
    prof.addEventListener('change', () => { NEW.profile = prof.value; });
    const live = el('input', {type: 'checkbox', title: 'redraw a coarse preview of the selected range after every change (in a worker)'});
    live.checked = st.live;
    live.addEventListener('change', () => { st.live = live.checked; if (st.live) schedulePreview(); else if (C.preview() && C.preview().owner === 'range') C.setPreview(null); });
    els.optBake = el('button', {class: 'mapgo', title: 'bake the selected range into the height plane (a worker; one undo step)'}, 'Bake');
    els.optBake.addEventListener('click', () => bake(selObj()));
    return el('span', {class: 'mapopts'}, 'new: crest ', num('crestM', -500, 3000, 50, 60, 'a NEW knot’s crest height, metres above sea (a later knot copies the one before it)'), ' m',
              ' half-width ', num('widthM', 100, 30000, 250, 64, 'a NEW knot’s half-width: crest to the foot of the range, metres ([ and ])'), ' m ', prof,
              el('label', {}, ' ', live, 'live preview'), ' ', els.optBake);
  }
  function slider(r, key, title) {
    const el = C.el;
    const i = el('input', {type: 'range', min: 0, max: 100, value: Math.round((r[key] == null ? TG.RANGE_DEFAULTS[key] : r[key]) * 100), style: 'width:110px', title});
    const out = el('span', {class: 'mapunit'}, i.value + '%');
    i.addEventListener('input', () => { out.textContent = i.value + '%'; });
    i.addEventListener('change', () => edit(() => { r[key] = +i.value / 100; }, r));
    return el('label', {title}, key + ' ', i, out);
  }
  function syncInspector() {
    if (!els.insp || !C.map()) return;
    const el = C.el, r = selObj(), box = els.insp;
    box.replaceChildren();
    if (!r) { box.append(el('div', {class: 'mapnote'}, 'Pick the Range tool and click along a crest to draw one; select a range to edit its knots and sliders, preview it and bake it.')); return; }
    const idIn = el('input', {type: 'text', value: r.id, class: 'mapid', title: 'the id (unique)'});
    idIn.addEventListener('change', () => {
      const nid = idIn.value.trim();
      if (!nid || ranges().some(o => o !== r && o.id === nid)) { idIn.value = r.id; C.toast('that id is empty or taken', true); return; }
      const rec = st.bakes.get(r.id);
      edit(() => { if (rec) { st.bakes.delete(r.id); st.bakes.set(nid, rec); } r.id = nid; st.sel = nid; }, null);
    });
    const nameIn = el('input', {type: 'text', value: r.name || '', class: 'mapid', placeholder: 'name', title: 'a name ("the Greyspine")'});
    nameIn.addEventListener('change', () => edit(() => { r.name = nameIn.value.trim(); }, null));
    const prof = el('select', {title: 'cross-section'}, ...TG.RANGE_PROFILE_NAMES.map(n => el('option', {value: n}, n)));
    prof.value = r.profile || 'alpine';
    prof.addEventListener('change', () => edit(() => { r.profile = prof.value; }, r));
    const seed = el('input', {type: 'number', min: 0, max: 999999, step: 1, value: r.seed | 0, style: 'width:64px', title: 'the seed: the same knots + seed give the same range'});
    seed.addEventListener('change', () => edit(() => { r.seed = Math.max(0, Math.round(+seed.value || 0)); }, r));
    box.append(el('div', {class: 'mapbar'}, el('b', {}, 'range'), idIn, nameIn),
               el('div', {class: 'mapbar'}, 'profile ', prof, ' seed ', seed),
               el('div', {class: 'mapgrid2'},
                 slider(r, 'peaks', 'PEAKS: how much the crest rises and falls along its length -- peaks at the knots’ heights, saddles between (0 = an even crest)'),
                 slider(r, 'spurs', 'SPURS: the side ridges running down the flanks and the valleys between them'),
                 slider(r, 'foothills', 'FOOTHILLS: how far the rough hills reach out past the foot of the range, and how high'),
                 slider(r, 'erode', 'ERODE: the stream-power erosion -- dendritic valleys cut into the range, a jagged crest of peaks and cols (0 = the designed shape only)'),
                 slider(r, 'wander', 'WANDER: how far the crest line and the spurs bend away from the drawn spline')));
    // the knots
    const tbl = el('div', {class: 'maplinepts'});
    knots(r).forEach((p, k) => {
      const hIn = el('input', {type: 'number', step: 10, value: Math.round((p[2] - sea()) / v()), style: 'width:64px', title: 'crest height at this knot, metres above sea'});
      hIn.addEventListener('change', () => edit(() => { p[2] = Math.round(sea() + (+hIn.value || 0) * v()); }, r));
      const wIn = el('input', {type: 'number', step: 50, min: 50, value: Math.round(p[3] / v()), style: 'width:60px', title: 'half-width at this knot, metres (crest to foot)'});
      wIn.addEventListener('change', () => edit(() => { p[3] = Math.max(Math.round(C.map().H.sp * 4), Math.round((+wIn.value || 0) * v())); }, r));
      const g = MT.heightAt(C.map().H, p[0], p[1]);
      tbl.append(el('div', {class: 'mapsite'}, el('span', {class: 'mapwhere'}, `${k} · ground ${C.fmtM((g - sea()) / v())}`), hIn, el('span', {class: 'mapunit'}, 'm'), wIn, el('span', {class: 'mapunit'}, 'm w')));
    });
    box.append(tbl);
    const allIn = el('input', {type: 'number', step: 50, value: knots(r).length ? Math.round((knots(r)[0][2] - sea()) / v()) : NEW.crestM, style: 'width:64px'});
    const allBtn = el('button', {title: 'every knot’s crest to this height (m above sea)'}, 'all crests to');
    allBtn.addEventListener('click', () => edit(() => { for (const p of knots(r)) p[2] = Math.round(sea() + (+allIn.value || 0) * v()); }, r));
    const wAll = el('input', {type: 'number', step: 250, value: knots(r).length ? Math.round(knots(r)[0][3] / v()) : NEW.widthM, style: 'width:64px'});
    const wBtn = el('button', {title: 'every knot’s half-width to this (m)'}, 'all widths to');
    wBtn.addEventListener('click', () => edit(() => { for (const p of knots(r)) p[3] = Math.max(Math.round(C.map().H.sp * 4), Math.round((+wAll.value || 0) * v())); }, r));
    box.append(el('div', {class: 'mapbar'}, allBtn, allIn, 'm ', wBtn, wAll, 'm'));
    const rec = st.bakes.get(r.id);
    const bk = el('button', {class: 'mapgo', title: rec ? 'REBAKE: take this session’s last bake out (strokes painted since stay, as offsets) and bake the range as it is now' :
                                                  'write the range into the height plane: the full erosion at 25.6 m, brought to the 6.4 m posts (a worker; one undo step)'}, rec ? 'Rebake' : 'Bake');
    bk.addEventListener('click', () => bake(r));
    const parts = [bk];
    if (rec) { const ub = el('button', {title: 'take the bake back out of the plane (strokes painted since stay, as offsets)'}, 'Unbake'); ub.addEventListener('click', () => unbake(r)); parts.push(ub); }
    const fin = el('button', {title: 'stop / resume adding knots (Enter / double-click)'}, st.drawing ? 'Finish' : 'Continue');
    fin.addEventListener('click', () => { if (st.drawing) finish(); else { st.drawing = true; C.setTool('range'); sync(); } });
    const pv = el('button', {title: 'redraw the coarse preview now'}, 'Preview');
    pv.addEventListener('click', runPreview);
    parts.push(fin, pv);
    const L = tess(r).length;
    box.append(el('div', {class: 'mapbar'}, ...parts),
               el('div', {class: 'mapnote'}, `${knots(r).length} knots · ${C.fmtM(L / v())} of crest · ` +
                  (rec ? (r.baked ? 'baked this session' : 'edited since its bake') : r.baked ? 'baked in an earlier session: no pre-bake copy here, so Bake bakes on top' : 'not baked')));
  }
  function sync() {
    if (!els.list || !C.map()) return;
    const el = C.el, rs = ranges();
    els.list.replaceChildren();
    for (const r of rs) {
      const top = Math.max(...knots(r).map(p => p[2]));
      const row = el('div', {class: 'mapsite' + (r.id === st.sel ? ' on' : '')},
        el('span', {class: 'mapkind mapkind-range'}, 'range'),
        el('span', {class: 'mapwhere'}, `${r.id}${r.name ? ' “' + r.name + '”' : ''} · ${knots(r).length} knots · ${isFinite(top) ? C.fmtM((top - sea()) / v()) : '?'}` + (r.baked ? '' : ' · not baked')));
      const sel = el('button', {title: 'select it (and centre the map on it)'}, r.id === st.sel ? 'selected' : 'select');
      sel.addEventListener('click', () => {
        st.sel = r.id; st.drawing = false;
        const P = knots(r), p = P[Math.floor(P.length / 2)];
        if (p) { C.view.cx = p[0]; C.view.cz = p[1]; }
        sync(); C.paint(); schedulePreview();
      });
      const del = el('button', {title: 'delete the RANGE (the plane keeps what it baked; Unbake first to take that out)'}, '×');
      del.addEventListener('click', () => edit(() => { rs.splice(rs.indexOf(r), 1); if (st.sel === r.id) { st.sel = null; st.drawing = false; } }, null));
      row.append(sel, del);
      els.list.append(row);
    }
    if (!rs.length) els.list.append(el('div', {class: 'mapnote'}, 'no ranges — pick the Range tool and click along a crest'));
    if (els.optBake) { els.optBake.disabled = !selObj(); els.optBake.textContent = selObj() && st.bakes.get(st.sel) ? 'Rebake' : 'Bake'; }
    syncInspector();
  }

  C.registerTool({
    key: 'range', label: 'Range', group: 'height', order: 35,
    title: 'MOUNTAIN RANGE from a spline: click along the crest (double-click / Enter ends); each knot has a crest height (Alt+drag it up / down) and a half-width (drag its diamond); peaks, spurs, foothills and erosion in the Ranges panel; live preview, then Bake',
    opts: optsStrip, down, move, up, paint, cursor,
    statusAt() {
      const d = st.drag;
      if (!d || d.kind === 'knot') return null;
      const p = knots(d.r)[d.k];
      return p ? `${d.r.id} knot ${d.k}: ` + (d.kind === 'crest' ? `crest ${C.fmtM((p[2] - sea()) / v())} above sea` : `half-width ${C.fmtM(p[3] / v())}`) : null;
    },
    resize(f) { NEW.widthM = Math.max(100, Math.min(30000, Math.round(NEW.widthM * f / 50) * 50)); },
    onKey(ev) {
      if ((ev.key === 'Enter' || ev.key === 'Escape') && st.drawing) { finish(); return true; }
      return false;
    },
    dblclick() { if (!st.drawing) return false; finish(); return true; },
    guide: ['Range — a mountain range from a spline',
      'Draws a real mountain range: a crest with peaks and saddles, side ridges (spurs) with valleys between them, foothills fading out, and stream-power erosion that grows branching (dendritic) valleys up to a jagged crest. The heights are the knots’ crest heights; the range only raises the ground.',
      'Click along the crest to add knots (double-click / Enter ends). Each knot: drag it to move it, Alt+drag it up / down to set its crest height (5 m a pixel), drag its diamond to set the half-width (crest to foot), shift+click deletes it; click the selected crest to insert one. The Ranges panel sets each knot’s crest (m above sea) and width, the profile, the seed and the peaks / spurs / foothills / erode / wander sliders. With "live preview" on, every change redraws a coarse version over the map. Bake writes it into the height plane (a worker with a progress bar; one undo step); Rebake replaces this session’s bake, Unbake takes it out.',
      'Editor-only: map.json ranges[] keeps the spline so you can rebake; the game reads only the baked heights. The preview runs at ~51 m, the bake at 25.6 m brought to the 6.4 m posts with rock detail. A wide range (a 10 km crest, 3 km half-width) bakes in about 5-10 s. Rebake / Unbake reach only a bake made this session.'],
    sync,
  });

  return {
    st, buildPanel, sync, paint, bake, unbake, runPreview,
    reset() { st.sel = null; st.drawing = false; st.drag = null; st.bakes = new Map(); clearTimeout(st.timer); },
    afterRestore() { if (st.sel && !selObj()) { st.sel = null; st.drawing = false; } sync(); schedulePreview(); },
    select(id) { st.sel = id; sync(); C.paint(); },
  };
}
