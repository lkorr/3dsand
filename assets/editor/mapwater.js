/* mapwater.js — THE RIVER AND LAKE TOOLS of the World map page
 * (docs/PLAN_worldbuilder.md E2 items 1-2; formats §2.1 / §2.2). A layer like
 * maprange.js: it owns map.json `rivers[]` and `lakes[]`, registers the River
 * and Lake tools and draws both, and is handed the page as a context.
 *
 * RIVER: a spline SOURCE -> MOUTH. Click to add knots (double-click / Enter
 *   ends); the first and the last knot carry the width + depth (w, d; the
 *   knots between take them by arc length unless set in the table); drag a
 *   knot, drag the selected river's WIDTH diamond, Alt+drag a knot up / down
 *   for its depth, click the selected river's curve to insert a knot,
 *   shift+click a knot to delete it. On release the MOUTH snaps: onto another
 *   river's channel (sets `joins`), into a lake polygon, or onto the coast
 *   (ground under the sea); riverbake.snapMouth.
 *   Drawn LIVE with the ENGINE's bake ported to JS (riverbake.js, pinned to
 *   the C++ by tests/river_vectors.json): the water ribbon at its width, the
 *   REACH boundaries (ticks across it), the WATERFALLS (a step of more than
 *   2 m), and in the inspector the longitudinal profile (ground vs the water
 *   surface, the engine's own bake of the saved river dashed beside it). The
 *   preview's ground is the HEIGHT PLANE; the engine's adds the seeded terrain
 *   noise and the sculpt layer, so the two differ by exactly that.
 *   DERIVE: hydrology's D8 network over the Area (or the whole map) on a
 *   ~20-50 m proxy, a catchment threshold in km², previewed coloured by
 *   order; click one to pick it, Accept picked / Accept all >= order N writes
 *   them into rivers[] with `joins` and widths from the catchment.
 *   MEANDER: a sum of sines along the normal (wavelength ~12 x width, the
 *   amplitude growing as the slope falls), the ends held.
 *   The engine CARVES the channel at load: nothing here touches the planes.
 *
 * LAKE: click inside a basin -> its Priority-Flood SPILL level and the
 *   outline of the water at that level (a coarse whole-map flood for the
 *   level, the posts around it for the outline) -> a lakes[] record, smooth;
 *   the inspector's level slider re-floods the outline live. Polygon mode:
 *   click a polygon by hand (double-click / Enter ends). Drag a vertex,
 *   shift+click deletes one, click the selected lake's edge to insert one.
 *   The SILL overlay marks shore-band columns outside the polygon below
 *   level + 1: the engine raises those to hold the water (riverbake.sillCells).
 *
 * Roads crossing rivers get a FORD marker (the engine: the road deck wins
 * inside its width, the river's water above it). */

import * as SPL from './spline.js';
import * as RB from './riverbake.js';
import * as HY from './hydrology.js';

const BEDS = ['gravel', 'sand', 'shore_mud', 'stone', 'clay', 'dirt'];
const ORDER_COL = ['#9fd8ff', '#5ab4ff', '#2f86e8', '#2a5fd0', '#7a4fe0', '#c050d0'];

export function createWaterLayer(C) {
  const st = {
    sel: null,              // {kind: 'river' | 'lake', id}
    drawing: false,         // clicks add river knots / lake vertices
    drag: null,
    lakeMode: 'basin',      // 'basin' | 'polygon'
    derive: null,           // {recs, picked: Set, grid, label}
    deriveKm2: 4, deriveOrder: 2, show: true,
    lastSnap: null, probe: null, seeds: new Map(), reflood: true,
  };
  // what a NEW river takes (metres)
  const NEW = {srcW: 3, srcD: 0.6, mouthW: 8, mouthD: 1.5, bed: 'gravel', meander: 1, seed: 1};
  const els = {};
  const J = () => C.map().json;
  const rivers = () => (J().rivers || (J().rivers = []));
  const lakes = () => (J().lakes || (J().lakes = []));
  const v = () => C.vpm(), sea = () => C.seaY();
  const px = d => d * C.view.scale;
  const fmtM = m => C.fmtM ? C.fmtM(m) : m.toFixed(1) + ' m';
  const selRiver = () => (st.sel && st.sel.kind === 'river' ? rivers().find(r => r.id === st.sel.id) || null : null);
  const selLake = () => (st.sel && st.sel.kind === 'lake' ? lakes().find(l => l.id === st.sel.id) || null : null);
  const ground = (x, z) => { const g = C.heightAt(x, z); return isFinite(g) ? g : sea(); };
  function freshId(arr, base) {
    let n = arr.length, id = base + '_' + n;
    while (arr.some(o => o.id === id)) id = base + '_' + (++n);
    return id;
  }
  function edit(fn) {
    C.pushUndo(C.snapshot());
    fn();
    C.markDirty(true); sync(); C.paint();
  }

  /* ---- geometry caches ------------------------------------------------------------------- */
  const tessCache = new Map();
  function tessOf(points, smooth, closed) {
    const key = (closed ? 'c' : 'o') + (smooth ? 's' : 'p') + JSON.stringify(points);
    let t = tessCache.get(key);
    if (!t) {
      t = SPL.tessellate((points || []).map(p => [p[0], p[1], p.length >= 4 ? p[2] : undefined, p.length >= 4 ? p[3] : undefined]), {step: 16, smooth, closed});
      if (tessCache.size > 400) tessCache.clear();
      tessCache.set(key, t);
    }
    return t;
  }
  const riverTess = r => tessOf(r.points || [], r.smooth === true, false);
  const lakeRing = l => (l.smooth === true && (l.points || []).length >= 3 ? tessOf(l.points, true, true).pts : (l.points || []));
  // THE LIVE BAKE: every river + lake, the engine's rules on the plane's ground;
  // rebuilt when the records or the height plane change (the plane's gen counter:
  // a brush stroke under a river moves its ground).
  // A bake of the default map's 15 rivers is ~100 ms. While the plane is BUSY
  // (a brush stroke is down -- every dab moves the plane's gen -- or tiles are
  // streaming in, each one a new gen) and only the ground moved, the bake is
  // redone at most every BAKE_BUSY_MS, and once more the moment the plane
  // settles (the stroke's release / the last tile repaints): a stroke no
  // longer pays 100 ms a frame. A change to the records re-bakes at once.
  const BAKE_BUSY_MS = 400;
  let bakeC = {key: '', gen: -1, t: 0, out: null};
  function bake() {
    if (!C.map()) return null;
    const j = J(), gen = C.map().H.gen, now = performance.now();
    const key = JSON.stringify([j.rivers || [], j.lakes || [], j.seaLevelY, (j.roads || []).map(r => [r.points, r.smooth])]);
    if (bakeC.out && bakeC.key === key) {
      if (bakeC.gen === gen) return bakeC.out;
      if (C.planeBusy && C.planeBusy() && now - bakeC.t < BAKE_BUSY_MS) { C.schedulePaint(); return bakeC.out; }
    }
    const out = RB.bakeAll({seaLevelY: sea(), rivers: j.rivers || [], lakes: j.lakes || []}, {ground: (x, z) => ground(x, z)});
    out.byId = new Map(out.rivers.map(b => [b.id, b]));
    out.fords = fords(out);
    bakeC = {key, gen, t: now, out};
    return out;
  }
  // Every road segment against every river segment was ~40 s of a 2-minute
  // pan / zoom bench on the default map (scripts/bench_mappage.sh): the rivers
  // are baked every 4 voxels, so tens of thousands of segments, and a road's
  // whole bounding box (kilometres) let most of them through. Both polylines
  // are cut into CHUNKS of consecutive segments with a bounding box, and only
  // overlapping chunks, then overlapping segments, reach segX. Two segments
  // whose boxes are disjoint cannot cross, and the loops still run road ->
  // river -> i -> k ascending, so the list is the same, in the same order.
  const FORD_CHUNK = 32;
  function segChunks(L) {
    const out = [];
    for (let s = 0; s + 1 < L.length; s += FORD_CHUNK) {
      const e = Math.min(L.length - 1, s + FORD_CHUNK);   // segments s .. e-1
      let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
      for (let i = s; i <= e; i++) { const p = L[i]; if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < z0) z0 = p[1]; if (p[1] > z1) z1 = p[1]; }
      out.push({s, e, x0, z0, x1, z1});
    }
    return out;
  }
  const boxesMeet = (a, b) => !(a.x1 < b.x0 || a.x0 > b.x1 || a.z1 < b.z0 || a.z0 > b.z1);
  /** Where a road's centreline crosses a baked river's: [{x, z, road, river}]. */
  function fords(B) {
    const out = [], roads = J().roads || [];
    const riverChunks = B.rivers.map(b => segChunks(b.line));
    for (const rd of roads) {
      const P = rd.smooth === true && (rd.points || []).length >= 2 ? SPL.smoothRoadPoints(rd.points) : (rd.points || []);
      if (P.length < 2) continue;
      let rx0 = Infinity, rz0 = Infinity, rx1 = -Infinity, rz1 = -Infinity;
      for (const p of P) { rx0 = Math.min(rx0, p[0]); rx1 = Math.max(rx1, p[0]); rz0 = Math.min(rz0, p[1]); rz1 = Math.max(rz1, p[1]); }
      const roadBox = {x0: rx0, z0: rz0, x1: rx1, z1: rz1}, roadChunks = segChunks(P);
      B.rivers.forEach((b, bi) => {
        const L = b.line;
        for (const rc of riverChunks[bi]) {
          if (!boxesMeet(rc, roadBox)) continue;
          const near = roadChunks.filter(q => boxesMeet(q, rc));
          if (!near.length) continue;
          for (let i = rc.s; i < rc.e; i++) {
            const a = L[i], c = L[i + 1];
            const sx0 = Math.min(a[0], c[0]), sx1 = Math.max(a[0], c[0]), sz0 = Math.min(a[1], c[1]), sz1 = Math.max(a[1], c[1]);
            if (sx1 < rx0 || sx0 > rx1 || sz1 < rz0 || sz0 > rz1) continue;
            for (const q of near) {
              if (sx1 < q.x0 || sx0 > q.x1 || sz1 < q.z0 || sz0 > q.z1) continue;
              for (let k = q.s; k < q.e; k++) {
                const p0 = P[k], p1 = P[k + 1];
                if (sx1 < Math.min(p0[0], p1[0]) || sx0 > Math.max(p0[0], p1[0]) || sz1 < Math.min(p0[1], p1[1]) || sz0 > Math.max(p0[1], p1[1])) continue;
                const hit = segX(a, c, p0, p1);
                if (hit) out.push({x: hit[0], z: hit[1], road: rd.id, river: b.id});
              }
            }
          }
        }
      });
    }
    return out;
  }
  function segX(a, b, c, d) {
    const rx = b[0] - a[0], rz = b[1] - a[1], sx = d[0] - c[0], sz = d[1] - c[1], den = rx * sz - rz * sx;
    if (Math.abs(den) < 1e-9) return null;
    const t = ((c[0] - a[0]) * sz - (c[1] - a[1]) * sx) / den, u = ((c[0] - a[0]) * rz - (c[1] - a[1]) * rx) / den;
    return t >= 0 && t < 1 && u >= 0 && u <= 1 ? [a[0] + rx * t, a[1] + rz * t] : null;
  }

  /* ---- the MOUTH snap ------------------------------------------------------------------- */
  function snapMouth(r, apply) {
    const P = r.points, e = P[P.length - 1];
    const s = RB.snapMouth(J(), r.id, e[0], e[1], {ground, reach: Math.max(24, 8 / C.view.scale)});
    st.lastSnap = s;
    if (apply) {
      if (s.kind === 'river') { e[0] = s.x; e[1] = s.z; r.joins = s.joins; }
      else if (r.joins) delete r.joins;
    }
    return s;
  }
  const snapText = s => (s.kind === 'river' ? `joins "${s.joins}"` : s.kind === 'lake' ? `drains into lake "${s.lake}"` : s.kind === 'sea' ? 'reaches the sea' :
    'ends on open ground (its water stays at its last reach; snap the mouth onto a river, a lake or the coast)');

  /* ---- RIVER pointer ---------------------------------------------------------------------- */
  const knotsOf = r => (r.points || []);
  function widthHandle(r, k) {
    const T = riverTess(r), P = knotsOf(r), p = P[k];
    const c = SPL.closest(T, p[0], p[1]);
    const i = c ? c.i : 0, a = T.pts[Math.max(0, i)], b = T.pts[Math.min(T.pts.length - 1, i + 1)];
    const dx = b[0] - a[0], dz = b[1] - a[1], L = Math.hypot(dx, dz) || 1;
    const w = (c && c.attrs[0] != null ? c.attrs[0] : 16) / 2 + Math.max(4, 6 / C.view.scale);
    return [p[0] - dz / L * w, p[1] + dx / L * w, c && c.attrs[0] != null ? c.attrs[0] : 16];
  }
  function riverDown(ev, fx, fz) {
    if (!C.map()) return false;
    const grab = 7, r0 = selRiver();
    // 0. a derived river (the preview) under the cursor: pick / unpick it
    if (st.derive && !st.drawing && !ev.shiftKey) {
      const hit = deriveHit(fx, fz);
      if (hit) { if (st.derive.picked.has(hit.id)) st.derive.picked.delete(hit.id); else st.derive.picked.add(hit.id); syncDerive(); C.paint(); return true; }
    }
    // 1. the selected river's width diamonds
    if (r0 && !st.drawing) for (let k = 0; k < knotsOf(r0).length; k++) {
      if (knotsOf(r0)[k].length < 4) continue;
      const [hx, hz] = widthHandle(r0, k);
      if (Math.hypot(px(hx - fx), px(hz - fz)) <= grab) { st.drag = {r: r0, k, kind: 'width', snap: C.snapshot(), moved: false}; return true; }
    }
    // 2. a knot (the selected river's first)
    // (while drawing only the river being drawn counts: a click on another
    // river's knot is the next knot -- its mouth snaps onto that river)
    const order = st.drawing && r0 ? [r0] : r0 ? [r0, ...rivers().filter(r => r !== r0)] : rivers();
    for (const r of order)
      for (let k = 0; k < knotsOf(r).length; k++) {
        const p = knotsOf(r)[k];
        if (Math.hypot(px(p[0] - fx), px(p[1] - fz)) > grab) continue;
        if (st.drawing && r === r0 && k === knotsOf(r).length - 1 && k > 0) { finish(); return true; }
        if (st.drawing) continue;
        if (ev.shiftKey) { deleteKnot(r, k); return true; }
        st.sel = {kind: 'river', id: r.id}; st.drawing = false;
        st.drag = {r, k, kind: ev.altKey ? 'depth' : 'knot', snap: C.snapshot(), moved: false, y0: ev.clientY,
                   d0: p.length >= 4 ? p[3] : null, alt: ev.altKey};
        sync(); C.paint();
        return true;
      }
    // 3. drawing: the next knot (the width + depth move to the new last knot)
    if (st.drawing && r0) {
      edit(() => {
        const P = r0.points, last = P[P.length - 1];
        const nk = [Math.round(fx), Math.round(fz), last[2], last[3]];
        if (P.length >= 2) P[P.length - 1] = [last[0], last[1]];
        else { nk[2] = Math.round(NEW.mouthW * v()); nk[3] = Math.round(NEW.mouthD * v()); }
        P.push(nk);
      });
      return true;
    }
    // 4. the selected river's curve: insert a knot there and drag it
    if (r0 && knotsOf(r0).length >= 2) {
      const c = SPL.closest(riverTess(r0), fx, fz);
      if (c && px(c.dist) <= 6) {
        const snap = C.snapshot();
        const ins = SPL.insertKnot(r0.points.map(p => [p[0], p[1]]), c.s, {step: 16, smooth: r0.smooth === true});
        r0.points.splice(ins.index, 0, [ins.points[ins.index][0], ins.points[ins.index][1]]);
        st.drag = {r: r0, k: ins.index, kind: 'knot', snap, moved: true};
        C.markDirty(true); sync(); C.paint();
        return true;
      }
    }
    // 5. another river's curve: select it
    for (const r of rivers()) {
      if (knotsOf(r).length < 2) continue;
      const c = SPL.closest(riverTess(r), fx, fz);
      const hw = c && c.attrs[0] != null ? c.attrs[0] / 2 : 8;
      if (c && px(Math.max(0, c.dist - hw)) <= 6) { st.sel = {kind: 'river', id: r.id}; st.drawing = false; sync(); C.paint(); return true; }
    }
    // 6. open map: a new river, its source here
    edit(() => {
      const r = {id: freshId(rivers(), 'river'), name: '', smooth: true, bed: NEW.bed,
                 points: [[Math.round(fx), Math.round(fz), Math.round(NEW.srcW * v()), Math.round(NEW.srcD * v())]]};
      rivers().push(r);
      st.sel = {kind: 'river', id: r.id}; st.drawing = true;
    });
    C.status('new river: its SOURCE is here; click along its course down to the MOUTH (double-click / Enter ends: the mouth snaps onto a river, a lake or the coast)');
    return true;
  }
  function deleteKnot(r, k) {
    edit(() => {
      const P = r.points;
      if (P.length <= 2) { rivers().splice(rivers().indexOf(r), 1); if (st.sel && st.sel.id === r.id) { st.sel = null; st.drawing = false; } C.status(`river "${r.id}" had too few knots left and was deleted`); return; }
      // an end knot carries the width + depth: the new end takes them
      const T = riverTess(r);
      const at = (q) => { const c = SPL.closest(T, P[q][0], P[q][1]); return c ? c.attrs : [16, 4]; };
      if (k === 0) { const a = at(1); P[1] = [P[1][0], P[1][1], Math.round(a[0] == null ? P[0][2] : a[0]), Math.round(a[1] == null ? P[0][3] : a[1])]; }
      if (k === P.length - 1) { const a = at(k - 1); P[k - 1] = [P[k - 1][0], P[k - 1][1], Math.round(a[0] == null ? P[k][2] : a[0]), Math.round(a[1] == null ? P[k][3] : a[1])]; }
      P.splice(k, 1);
    });
  }
  function riverMove(ev, fx, fz) {
    const d = st.drag;
    if (!d || !d.r) return;
    const p = d.r.points[d.k];
    if (!p) return;
    if (d.kind === 'knot') { p[0] = Math.round(fx); p[1] = Math.round(fz); }
    else if (d.kind === 'depth') {
      if (p.length < 4) { const c = SPL.closest(riverTess(d.r), p[0], p[1]); p[2] = Math.round(c && c.attrs[0] != null ? c.attrs[0] : 16); p[3] = Math.round(c && c.attrs[1] != null ? c.attrs[1] : 4); d.d0 = p[3]; }
      p[3] = Math.max(RB.K.depthMin, Math.min(RB.K.depthMax, Math.round(d.d0 + (d.y0 - ev.clientY) * 0.5)));
    } else {
      const w = 2 * Math.hypot(fx - p[0], fz - p[1]) - 2 * Math.max(4, 6 / C.view.scale);
      p[2] = Math.max(RB.K.widthMin, Math.min(RB.K.widthMax, Math.round(w)));
    }
    d.moved = true;
    C.markDirty(true);
    if (!d.redraw) { d.redraw = true; requestAnimationFrame(() => { d.redraw = false; syncInspector(); }); }
  }
  function riverUp() {
    const d = st.drag;
    if (!d || !d.r) return;
    st.drag = null;
    if (d.moved && d.kind === 'knot' && d.k === d.r.points.length - 1 && !d.alt) {
      const s = snapMouth(d.r, true);
      C.status(`${d.r.id}: the mouth ${snapText(s)}`);
    }
    if (d.moved) { if (d.snap) C.pushUndo(d.snap); C.markDirty(true); }
    sync(); C.paint();
  }
  function finish() {
    if (!st.drawing) return;
    st.drawing = false;
    const r = selRiver(), l = selLake();
    if (r) {
      const P = r.points;
      if (P.length >= 2 && Math.hypot(px(P[P.length - 1][0] - P[P.length - 2][0]), px(P[P.length - 1][1] - P[P.length - 2][1])) < 4) {
        const last = P.pop();
        P[P.length - 1] = [P[P.length - 1][0], P[P.length - 1][1], last[2], last[3]];
        if (P.length === 1) P[0] = [P[0][0], P[0][1], Math.round(NEW.srcW * v()), Math.round(NEW.srcD * v())];
      }
      if (RB.parseRiver(r).error) {
        rivers().splice(rivers().indexOf(r), 1); st.sel = null;
        C.status('a river needs a source and a mouth (two distinct knots); the unfinished one was dropped');
      } else {
        const s = snapMouth(r, true);
        C.markDirty(true);
        C.status(`${r.id}: ${r.points.length} knots; the mouth ${snapText(s)}`);
      }
    } else if (l) {
      if (RB.parseLake(l).error) { lakes().splice(lakes().indexOf(l), 1); st.sel = null; C.status('a lake polygon needs three distinct vertices; the unfinished one was dropped'); }
      else {
        // a hand polygon's level: its lowest vertex's ground + 1 m (the inspector sets it)
        if (st.newLake === l.id) { l.level = Math.round(Math.min(...l.points.map(p => ground(p[0], p[1]))) + v()); st.newLake = null; }
        C.markDirty(true);
      }
    }
    sync(); C.paint();
  }

  /* ---- DERIVE ------------------------------------------------------------------------------ */
  function deriveBox() { const a = C.area(); return a ? a.slice() : null; }
  async function derive() {
    if (!C.map()) return;
    const box = deriveBox(), H = C.map().H;
    const g0 = C.postsGrid(box);
    if (!g0) { C.toast('the Area holds no posts', true); return; }
    if (C.rectBlocked(g0.rect)) { C.toast('tiles are still loading; derive again in a moment', true); return; }
    const [, , w, hh] = g0.rect;
    let f = 1;
    while ((w / f) * (hh / f) > 250000) f++;
    const g = C.coarseGrid(box, f);
    const cellKm2 = (g.grid.cell / v()) * (g.grid.cell / v()) / 1e6;
    const thr = Math.max(2, Math.round(st.deriveKm2 / cellKm2));
    st.derive = {recs: [], picked: new Set(), running: true, label: `${box ? 'the Area' : 'the whole map'} at ${(g.grid.cell / v()).toFixed(0)} m cells, threshold ${st.deriveKm2} km² (${thr} cells)`};
    syncDerive();
    let res;
    try { res = await C.runJob({job: 'rivers', grid: g.grid, seaY: sea(), threshold: thr, minLength: 4}, {quiet: true, key: 'rivers-derive'}); }
    catch (e) { st.derive = null; syncDerive(); C.toast('derive rivers: ' + e.message, true); return; }
    // simplify each polyline in world units (half a cell) so the knots are few
    const G = {x0: g.grid.x0, z0: g.grid.z0, cell: g.grid.cell};
    const recs = RB.derivedToRecords(res.list, G, {vpm: v(), taken: rivers().map(r => r.id)});
    for (const r of recs) {
      const keep = HY.simplify(r.points, g.grid.cell * 0.35);
      r.points = keep.length >= 2 ? keep : r.points;
    }
    st.derive.recs = recs.filter(r => r.points.length >= 2);
    st.derive.running = false;
    st.derive.H = H;
    syncDerive(); C.paint();
    C.status(`derived ${st.derive.recs.length} rivers over ${st.derive.label}: click one to pick it (River tool), or Accept all >= order N`);
    return st.derive;
  }
  function deriveHit(fx, fz) {
    if (!st.derive) return null;
    let best = null, bd = 6;
    for (const r of st.derive.recs) {
      const P = r.points;
      for (let i = 0; i + 1 < P.length; i++) {
        const a = P[i], b = P[i + 1], ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
        const t = L2 > 0 ? Math.max(0, Math.min(1, ((fx - a[0]) * ex + (fz - a[1]) * ez) / L2)) : 0;
        const d = px(Math.hypot(fx - a[0] - ex * t, fz - a[1] - ez * t));
        if (d < bd) { bd = d; best = r; }
      }
    }
    return best;
  }
  /** Accept derived rivers into rivers[]: `which` = 'picked' or an order. */
  function accept(which) {
    if (!st.derive || !st.derive.recs.length) return 0;
    const take = st.derive.recs.filter(r => (which === 'picked' ? st.derive.picked.has(r.id) : r.order >= which));
    if (!take.length) { C.toast(which === 'picked' ? 'no derived river is picked: click one with the River tool' : 'no derived river is of that order or higher', true); return 0; }
    const ids = new Set(take.map(r => r.id));
    edit(() => {
      for (const r of take) {
        const rec = {id: r.id, name: '', smooth: true, bed: NEW.bed, points: r.points.map(p => p.slice()),
                     about: `derived: catchment ${r.areaKm2.toFixed(1)} km², order ${r.order}`};
        // a tributary whose parent is not taken: its mouth snaps instead
        if (r.joins && ids.has(r.joins)) rec.joins = r.joins;
        rivers().push(rec);
      }
      for (const r of take) if (!r.joins || !ids.has(r.joins)) { const rec = rivers().find(o => o.id === r.id); if (rec) snapMouth(rec, true); }
      st.derive.recs = st.derive.recs.filter(r => !ids.has(r.id));
      for (const id of ids) st.derive.picked.delete(id);
      if (!st.derive.recs.length) st.derive = null;
      st.sel = {kind: 'river', id: take[0].id};
    });
    C.status(`accepted ${take.length} river(s) into rivers[] (one undo step)`);
    return take.length;
  }

  /* ---- MEANDER ----------------------------------------------------------------------------- */
  function meander(r, amount, seed) {
    if (!r || RB.parseRiver(r).error) return;
    edit(() => {
      const pts = r.points.map(p => p.slice());
      r.points = RB.meander(pts, {amount, seed, ground});
    });
    C.status(`${r.id}: meandered (${r.points.length} knots; wavelength ~12 x its width, wider loops where it is flat) — Ctrl+Z takes it back`);
  }

  /* ---- LAKE pointer ------------------------------------------------------------------------- */
  /** The basin under (x, z): the level from a coarse flood of the whole map,
   *  the outline from the posts round it (or the coarse cells for a huge one). */
  function basinAt(x, z, level = null) {
    const P = C.map().H;
    const [nx, nz] = P.posts;
    let f = 1;
    while ((nx / f) * (nz / f) > 250000) f++;
    const g = C.coarseGrid(null, f);
    if (!g) return null;
    let lv = level;
    if (lv == null) {
      const filled = HY.priorityFlood(g.grid.h, g.grid.w, g.grid.hh, {seaY: sea(), epsilon: 0});
      const i = Math.round((x - g.grid.x0) / g.grid.cell), j = Math.round((z - g.grid.z0) / g.grid.cell);
      if (i < 0 || j < 0 || i >= g.grid.w || j >= g.grid.hh) return null;
      lv = Math.floor(filled[j * g.grid.w + i]);
      if (!(lv - g.grid.h[j * g.grid.w + i] > 0.5)) {
        // the coarse cell drains: try the posts round the click (a small pond)
        const fine = fineGrid([x - 2048, z - 2048, x + 2048, z + 2048]);
        if (!fine) return null;
        const rec = RB.lakeFromGrid(fine, x, z, {seaY: sea()});
        return rec && rec.level - ground(x, z) > 0.5 ? rec : null;
      }
    }
    const coarse = RB.lakeFromGrid(g.grid, x, z, {level: lv});
    if (!coarse) return null;
    let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
    for (const p of coarse.points) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
    const m = g.grid.cell * 2;
    const fine = (x1 - x0) * (z1 - z0) / (P.sp * P.sp) < 1.2e6 ? fineGrid([x0 - m, z0 - m, x1 + m, z1 + m]) : null;
    const rec = fine ? RB.lakeFromGrid(fine, x, z, {level: lv, tolerance: 1.2}) : null;
    return rec || coarse;
  }
  function fineGrid(box) {
    const g = C.postsGrid(box);
    if (!g || C.rectBlocked(g.rect)) return null;
    g.grid.h = C.readPosts(...g.rect);
    return g.grid;
  }
  function lakeHit(fx, fz) {
    for (let i = lakes().length - 1; i >= 0; i--) {
      const l = lakes()[i];
      const p = RB.parseLake(l);
      if (p.error) continue;
      if (RB.lakeInside(RB.lakePoly(p).poly, Math.round(fx), Math.round(fz))) return l;
    }
    return null;
  }
  function lakeDown(ev, fx, fz) {
    if (!C.map()) return false;
    const grab = 7, l0 = selLake();
    // 1. a vertex (the selected lake's first)
    const order = l0 ? [l0, ...lakes().filter(l => l !== l0)] : lakes();
    for (const l of order)
      for (let k = 0; k < (l.points || []).length; k++) {
        const p = l.points[k];
        if (Math.hypot(px(p[0] - fx), px(p[1] - fz)) > grab) continue;
        if (st.drawing && l === l0) { if (k === 0 && l.points.length >= 3) { finish(); return true; } continue; }
        if (ev.shiftKey) {
          edit(() => { if (l.points.length <= 3) { lakes().splice(lakes().indexOf(l), 1); st.sel = null; } else l.points.splice(k, 1); });
          return true;
        }
        st.sel = {kind: 'lake', id: l.id};
        st.drag = {l, k, kind: 'vertex', snap: C.snapshot(), moved: false};
        sync(); C.paint();
        return true;
      }
    // 2. drawing a hand polygon: the next vertex
    if (st.drawing && l0) { edit(() => { l0.points.push([Math.round(fx), Math.round(fz)]); }); return true; }
    // 3. the selected lake's edge: insert a vertex and drag it
    if (l0 && (l0.points || []).length >= 3) {
      const P = l0.points;
      for (let k = 0; k < P.length; k++) {
        const a = P[k], b = P[(k + 1) % P.length], ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
        const t = L2 > 0 ? Math.max(0, Math.min(1, ((fx - a[0]) * ex + (fz - a[1]) * ez) / L2)) : 0;
        if (t > 0 && t < 1 && px(Math.hypot(fx - a[0] - ex * t, fz - a[1] - ez * t)) <= 5) {
          const snap = C.snapshot();
          P.splice(k + 1, 0, [Math.round(a[0] + ex * t), Math.round(a[1] + ez * t)]);
          st.drag = {l: l0, k: k + 1, kind: 'vertex', snap, moved: true};
          C.markDirty(true); sync(); C.paint();
          return true;
        }
      }
    }
    // 4. inside a lake: select it
    const hit = lakeHit(fx, fz);
    if (hit && !ev.ctrlKey) { st.sel = {kind: 'lake', id: hit.id}; st.drawing = false; sync(); C.paint(); return true; }
    // 5. a new lake: polygon mode starts one; basin mode floods the click
    if (st.lakeMode === 'polygon') {
      edit(() => {
        const l = {id: freshId(lakes(), 'lake'), name: '', level: 0, smooth: false, bed: 'shore_mud', shore: 'sand', points: [[Math.round(fx), Math.round(fz)]]};
        st.newLake = l.id;
        lakes().push(l);
        st.sel = {kind: 'lake', id: l.id}; st.drawing = true;
      });
      C.status('new lake polygon: click its vertices (click the first one, double-click or Enter ends); its level starts 1 m over the lowest vertex');
      return true;
    }
    const rec = basinAt(fx, fz);
    if (!rec) { C.toast('no basin here: the ground drains away (a lake needs a hollow; or use polygon mode)', true); return true; }
    edit(() => {
      const l = {id: freshId(lakes(), 'lake'), name: '', level: rec.level, smooth: true, bed: 'shore_mud', shore: 'sand', points: rec.points};
      lakes().push(l);
      st.seeds.set(l.id, [Math.round(fx), Math.round(fz)]);
      st.sel = {kind: 'lake', id: l.id};
    });
    C.status(`new lake at its spill level ${fmtM((rec.level - sea()) / v())} above sea: ${rec.points.length} outline points, ${(rec.area / v() / v() / 1e6).toFixed(3)} km², ${fmtM(rec.maxDepth / v())} deep — the level slider re-floods it`);
    return true;
  }
  /** Re-flood the selected lake's outline at `level` (the slider). */
  function reflood(l, level) {
    const seed = st.seeds.get(l.id) || lowestInside(l);
    if (!seed) return false;
    const rec = basinAt(seed[0], seed[1], level);
    if (!rec || rec.points.length < 3) return false;
    l.points = rec.points;
    l.level = Math.round(level);
    return true;
  }
  function lowestInside(l) {
    const p = RB.parseLake(l);
    if (p.error) return null;
    const L = RB.lakePoly(p), stp = Math.max(16, Math.round(Math.max(L.x1 - L.x0, L.z1 - L.z0) / 40));
    let best = null, bg = Infinity;
    for (let z = L.z0; z <= L.z1; z += stp) for (let x = L.x0; x <= L.x1; x += stp)
      if (RB.lakeInside(L.poly, x, z)) { const g = ground(x, z); if (g < bg) { bg = g; best = [x, z]; } }
    return best;
  }
  function lakeMove(ev, fx, fz) {
    const d = st.drag;
    if (!d || !d.l) return;
    const p = d.l.points[d.k];
    p[0] = Math.round(fx); p[1] = Math.round(fz);
    d.moved = true;
    C.markDirty(true);
  }
  function lakeUp() {
    const d = st.drag;
    if (!d || !d.l) return;
    st.drag = null;
    if (d.moved) { if (d.snap) C.pushUndo(d.snap); C.markDirty(true); }
    sync(); C.paint();
  }
  const sillCache = new Map();
  function sillOf(l) {
    const key = JSON.stringify([l.points, l.level, l.band, l.smooth]) + '|' + Math.round(Math.log2(Math.max(1e-6, C.view.scale))) + '|' + C.map().H.gen;
    const c = sillCache.get(l.id);
    if (c && c.key === key) return c.cells;
    const p = RB.parseLake(l);
    let cells = [];
    if (!p.error) {
      const L = {...p, ...RB.lakePoly(p)};
      const span = Math.max(L.x1 - L.x0, L.z1 - L.z0) + 2 * L.band;
      const step = Math.max(8, Math.round(Math.max(3 / C.view.scale, span / 160)));
      cells = RB.sillCells(L, ground, step);
      cells.step = step;
    }
    sillCache.set(l.id, {key, cells});
    return cells;
  }

  /* ---- drawing ------------------------------------------------------------------------------ */
  // per baked river (the bake's own objects, so a rebake drops them): its widest
  // half-width and its longest sample step, for the view culling below
  const bakeMeta = new WeakMap();
  function paint(g) {
    if (!C.map() || !st.show) return;
    const B = bake();
    if (!B) return;
    const SX = C.SX, SZ = C.SZ;
    // THE VIEW in world voxels: what lies wholly outside it (plus a margin for
    // labels, glyphs and line widths) is not drawn. Only the cost changes: the
    // canvas would have clipped all of it.
    const sc = C.view.scale, vw = g.canvas.width, vh = g.canvas.height;
    const wx0 = C.view.cx - vw / 2 / sc, wx1 = C.view.cx + vw / 2 / sc, wz0 = C.view.cz - vh / 2 / sc, wz1 = C.view.cz + vh / 2 / sc;
    const offView = (x0, z0, x1, z1, mPx) => { const m = mPx / sc; return x1 < wx0 - m || x0 > wx1 + m || z1 < wz0 - m || z0 > wz1 + m; };
    g.save();
    g.font = '11px monospace';
    // ---- lakes
    for (const l of J().lakes || []) {
      const ring = lakeRing(l);
      if (ring.length < 2) continue;
      const sel = st.sel && st.sel.kind === 'lake' && st.sel.id === l.id;
      if (!sel) {
        // the ring's box (the label hangs right of the first point: 400 px of margin covers it)
        let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
        for (const p of ring) { if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < z0) z0 = p[1]; if (p[1] > z1) z1 = p[1]; }
        for (const p of l.points || []) { if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < z0) z0 = p[1]; if (p[1] > z1) z1 = p[1]; }
        if (offView(x0, z0, x1, z1, 400)) continue;
      }
      g.beginPath();
      ring.forEach((p, k) => (k ? g.lineTo(SX(p[0]), SZ(p[1])) : g.moveTo(SX(p[0]), SZ(p[1]))));
      g.closePath();
      g.globalAlpha = sel ? 0.6 : 0.45; g.fillStyle = '#3c7fd8'; g.fill();
      g.globalAlpha = 1; g.strokeStyle = sel ? '#ffffff' : '#9fd0ff'; g.lineWidth = sel ? 2 : 1; g.stroke();
      if (sel) {
        const cells = sillOf(l), s = Math.max(2, Math.min(6, (cells.step || 8) * C.view.scale));
        g.fillStyle = '#ff5a3a';
        for (const c of cells) g.fillRect(Math.round(SX(c[0]) - s / 2), Math.round(SZ(c[1]) - s / 2), Math.ceil(s), Math.ceil(s));
      }
      if (sel || C.tool() === 'lake') (l.points || []).forEach(p => {
        const x = Math.round(SX(p[0])), z = Math.round(SZ(p[1]));
        g.fillStyle = sel ? '#ffffff' : '#9fd0ff'; g.fillRect(x - 2, z - 2, 5, 5);
        g.fillStyle = '#0e1219'; g.fillRect(x - 1, z - 1, 3, 3);
      });
      const p0 = (l.points || [])[0];
      if (p0 && (sel || C.view.scale >= 0.05)) {
        g.fillStyle = sel ? '#ffffff' : '#bfe0ff';
        g.fillText(`${l.name || l.id} · ${fmtM((l.level - sea()) / v())}`, Math.round(SX(p0[0])) + 6, Math.round(SZ(p0[1])) - 6);
      }
    }
    // ---- rivers: the ribbon at its width, reach ticks, waterfalls
    for (const b of B.rivers) {
      const r = rivers().find(o => o.id === b.id);
      if (!r) continue;
      const sel = st.sel && st.sel.kind === 'river' && st.sel.id === b.id;
      const L = b.line, N = L.length;
      const stepI = Math.max(1, Math.floor(3 / Math.max(1e-6, px(4))));
      const idx = [];
      for (let i = 0; i < N; i += stepI) idx.push(i);
      if (idx[idx.length - 1] !== N - 1) idx.push(N - 1);
      const nrm = i => {
        const a = L[Math.max(0, i - 2)], c = L[Math.min(N - 1, i + 2)], dx = c[0] - a[0], dz = c[1] - a[1], l = Math.hypot(dx, dz) || 1;
        return [-dz / l, dx / l];
      };
      let bm = bakeMeta.get(b);
      if (!bm) {
        let m = 0;
        for (let i = 1; i < N; i++) m = Math.max(m, Math.hypot(L[i][0] - L[i - 1][0], L[i][1] - L[i - 1][1]));
        bakeMeta.set(b, bm = {hwMax: Math.max(...b.samples.hw), segMax: m});
      }
      const hwMax = bm.hwMax;
      const wpx = px(2 * hwMax);
      // RUNS: the stretches of the (decimated) line near the view. A sample is
      // near when it lies within the half-width, a tick, one decimated step and
      // a few pixels of it, so every drawn segment that can touch the canvas
      // is in a run and every cut between two runs is off the canvas. All the
      // runs go into ONE path (one fill, one stroke), like the whole line did.
      const mW = hwMax + Math.max(2, 3 / sc) + stepI * bm.segMax + 8 / sc;
      const near = i => L[i][0] >= wx0 - mW && L[i][0] <= wx1 + mW && L[i][1] >= wz0 - mW && L[i][1] <= wz1 + mW;
      const runs = [];
      {
        const Q = idx.length, vis = new Uint8Array(Q);
        for (let q = 0; q < Q; q++) vis[q] = near(idx[q]) ? 1 : 0;
        let cur = null;
        for (let q = 0; q < Q; q++) {
          if (vis[q] || (q > 0 && vis[q - 1]) || (q + 1 < Q && vis[q + 1])) { if (!cur) runs.push(cur = []); cur.push(idx[q]); }
          else cur = null;
        }
      }
      if (wpx >= 1.5 && runs.length) {
        g.beginPath();
        for (const run of runs) {
          run.forEach((i, q) => { const [nx, nz] = nrm(i), h = L[i][3]; const x = SX(L[i][0] + nx * h), z = SZ(L[i][1] + nz * h); if (q) g.lineTo(x, z); else g.moveTo(x, z); });
          for (let q = run.length - 1; q >= 0; q--) { const i = run[q], [nx, nz] = nrm(i), h = L[i][3]; g.lineTo(SX(L[i][0] - nx * h), SZ(L[i][1] - nz * h)); }
          g.closePath();
        }
        g.globalAlpha = sel ? 0.85 : 0.7; g.fillStyle = '#2f7fe0'; g.fill(); g.globalAlpha = 1;
        if (sel) { g.strokeStyle = '#ffffff'; g.lineWidth = 1; g.stroke(); }
      }
      g.strokeStyle = sel ? '#ffffff' : '#7cc0ff'; g.lineWidth = wpx >= 1.5 ? 1 : 2;
      if (wpx >= 1.5) g.globalAlpha = 0.5;
      g.beginPath();
      for (const run of runs) run.forEach((i, q) => (q ? g.lineTo(SX(L[i][0]), SZ(L[i][1])) : g.moveTo(SX(L[i][0]), SZ(L[i][1]))));
      g.stroke(); g.globalAlpha = 1;
      // reach boundaries: a tick across the water at each step (selected, or wide enough to read)
      if (sel || wpx >= 6) {
        g.strokeStyle = 'rgba(230,245,255,0.85)'; g.lineWidth = 1;
        for (let k = 1; k < b.starts.length; k++) {
          const i = b.starts[k];
          if (!near(i)) continue;
          const [nx, nz] = nrm(i), h = L[i][3] + Math.max(2, 3 / C.view.scale);
          g.beginPath(); g.moveTo(SX(L[i][0] + nx * h), SZ(L[i][1] + nz * h)); g.lineTo(SX(L[i][0] - nx * h), SZ(L[i][1] - nz * h)); g.stroke();
        }
      }
      // waterfalls: a white pixel-art fall glyph (a bar with three drops), the drop beside it when selected
      for (const f of b.falls) {
        const x = Math.round(SX(f[0])), z = Math.round(SZ(f[1]));
        g.fillStyle = '#0e1219'; g.fillRect(x - 4, z - 5, 9, 10);
        g.fillStyle = '#ffffff'; g.fillRect(x - 3, z - 4, 7, 2);
        g.fillRect(x - 3, z - 1, 1, 4); g.fillRect(x, z - 1, 1, 5); g.fillRect(x + 3, z - 1, 1, 4);
        if (sel) { g.fillStyle = '#ffffff'; g.fillText('↓' + fmtM(f[2] / v()), x + 7, z + 4); }
      }
      // knots + width diamonds
      if (sel || C.tool() === 'river') knotsOf(r).forEach((p, k) => {
        const x = Math.round(SX(p[0])), z = Math.round(SZ(p[1]));
        const end = k === 0 || k === knotsOf(r).length - 1;
        g.fillStyle = sel ? '#ffffff' : '#7cc0ff'; g.fillRect(x - 3, z - 3, 7, 7);
        g.fillStyle = end ? (k === 0 ? '#5ad87a' : '#2f7fe0') : '#0e1219'; g.fillRect(x - 2, z - 2, 5, 5);
        if (sel && p.length >= 4 && !st.drawing) {
          const [hx, hz] = widthHandle(r, k), hxs = Math.round(SX(hx)), hzs = Math.round(SZ(hz));
          g.strokeStyle = 'rgba(200,230,255,0.6)'; g.beginPath(); g.moveTo(x, z); g.lineTo(hxs, hzs); g.stroke();
          g.fillStyle = '#c8e6ff';
          g.beginPath(); g.moveTo(hxs, hzs - 4); g.lineTo(hxs + 4, hzs); g.lineTo(hxs, hzs + 4); g.lineTo(hxs - 4, hzs); g.closePath(); g.fill();
        }
      });
      if (sel || C.view.scale >= 0.05) {
        const m = L[Math.floor(N / 2)];
        g.fillStyle = sel ? '#ffffff' : '#bfe0ff';
        g.fillText(r.name || r.id, Math.round(SX(m[0])) + 8, Math.round(SZ(m[1])) - 8);
      }
      // the junction
      if (b.joins) { const e = L[N - 1]; g.strokeStyle = '#ffd866'; g.lineWidth = 1; g.strokeRect(Math.round(SX(e[0])) - 4.5, Math.round(SZ(e[1])) - 4.5, 9, 9); }
    }
    // rivers the engine would refuse (no bake): dashed red knot lines
    for (const r of J().rivers || []) {
      if (B.byId.has(r.id) || !(r.points || []).length) continue;
      g.strokeStyle = '#ff6a5a'; g.setLineDash([4, 3]); g.lineWidth = 1.5;
      g.beginPath(); r.points.forEach((p, k) => (k ? g.lineTo(SX(p[0]), SZ(p[1])) : g.moveTo(SX(p[0]), SZ(p[1])))); g.stroke(); g.setLineDash([]);
      r.points.forEach(p => { g.fillStyle = '#ff6a5a'; g.fillRect(Math.round(SX(p[0])) - 2, Math.round(SZ(p[1])) - 2, 5, 5); });
    }
    // fords: a pixel stepping-stone glyph where a road crosses a river
    for (const f of B.fords) {
      const x = Math.round(SX(f.x)), z = Math.round(SZ(f.z));
      g.fillStyle = '#0e1219'; g.fillRect(x - 5, z - 4, 11, 9);
      g.fillStyle = '#c8b48a'; g.fillRect(x - 4, z - 3, 3, 3); g.fillRect(x + 2, z - 3, 3, 3); g.fillRect(x - 1, z + 1, 3, 3);
      if (C.view.scale >= 0.05) { g.fillStyle = '#e8dcc0'; g.fillText('ford', x + 7, z + 4); }
    }
    // the derive preview, coloured by order; picked ones white-edged
    if (st.derive) for (const r of st.derive.recs) {
      const P = r.points;
      const picked = st.derive.picked.has(r.id);
      g.strokeStyle = picked ? '#ffffff' : ORDER_COL[Math.min(ORDER_COL.length - 1, r.order - 1)];
      g.lineWidth = Math.max(1, Math.min(5, r.order)) + (picked ? 1 : 0);
      g.globalAlpha = picked ? 1 : 0.85;
      g.beginPath(); P.forEach((p, k) => (k ? g.lineTo(SX(p[0]), SZ(p[1])) : g.moveTo(SX(p[0]), SZ(p[1])))); g.stroke();
      g.globalAlpha = 1;
    }
    // the mouth snap target while dragging the last knot / drawing
    const hv = C.hover && C.hover();
    const r0 = selRiver();
    if (r0 && hv && ((st.drag && st.drag.r === r0 && st.drag.k === r0.points.length - 1 && st.drag.kind === 'knot') || (st.drawing && C.tool() === 'river'))) {
      const s = RB.snapMouth(J(), r0.id, hv[0], hv[1], {ground, reach: Math.max(24, 8 / C.view.scale)});
      if (s.kind !== 'none') {
        const x = Math.round(SX(s.x)), z = Math.round(SZ(s.z));
        g.strokeStyle = '#ffd866'; g.lineWidth = 1; g.strokeRect(x - 5.5, z - 5.5, 11, 11);
        g.fillStyle = '#ffd866'; g.fillText(s.kind === 'river' ? 'joins ' + s.joins : s.kind === 'lake' ? 'into ' + s.lake : 'the coast', x + 8, z - 6);
      }
      if (st.drawing) {
        const a = r0.points[r0.points.length - 1];
        g.strokeStyle = 'rgba(255,255,255,0.7)'; g.setLineDash([3, 3]);
        g.beginPath(); g.moveTo(SX(a[0]), SZ(a[1])); g.lineTo(SX(hv[0]), SZ(hv[1])); g.stroke(); g.setLineDash([]);
      }
    }
    if (st.probe) { const x = Math.round(SX(st.probe[0])), z = Math.round(SZ(st.probe[1])); g.strokeStyle = '#ff5a5a'; g.lineWidth = 2; g.strokeRect(x - 4, z - 4, 9, 9); }
    g.restore();
  }

  /* ---- the panel ------------------------------------------------------------------------------ */
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'maptools', open: true});
    const show = el('input', {type: 'checkbox', title: 'draw the rivers and lakes'});
    show.checked = st.show;
    show.addEventListener('change', () => { st.show = show.checked; C.paint(); });
    det.append(el('summary', {}, 'Water — rivers and lakes (map.json rivers[] / lakes[]: the engine carves and fills them at load)'),
               el('div', {class: 'mapbar'}, el('label', {}, show, ' show')));
    els.list = el('div', {class: 'mapsites-list'});
    els.derive = el('div', {class: 'maptools-panel'});
    els.insp = el('div', {class: 'maptools-panel'});
    det.append(els.list, el('div', {class: 'mapsub'}, 'Derive rivers from the terrain'), els.derive, els.insp);
    return det;
  }
  function riverOpts() {
    const el = C.el;
    const num = (key, min, max, step, title) => {
      const i = el('input', {type: 'number', min, max, step, value: NEW[key], style: 'width:52px', title});
      i.addEventListener('change', () => { NEW[key] = Math.max(min, Math.min(max, +i.value || 0)); i.value = NEW[key]; });
      return i;
    };
    return el('span', {class: 'mapopts'}, 'source w ', num('srcW', 0.8, 102, 0.5, 'a NEW river’s width at its source, metres'), ' d ', num('srcD', 0.2, 12, 0.1, 'its depth at the source, metres'),
              ' · mouth w ', num('mouthW', 0.8, 102, 0.5, 'a NEW river’s width at its mouth, metres'), ' d ', num('mouthD', 0.2, 12, 0.1, 'its depth at the mouth, metres'),
              el('span', {class: 'mapnote'}, ' · click source → mouth, double-click ends · drag a knot / its ◆ (width), Alt+drag = depth · shift+click deletes'));
  }
  function lakeOpts() {
    const el = C.el;
    const mode = el('select', {title: 'basin: click a hollow and the lake fills it to its spill level; polygon: click the outline yourself'},
                    el('option', {value: 'basin'}, 'basin (click a hollow)'), el('option', {value: 'polygon'}, 'polygon (by hand)'));
    mode.value = st.lakeMode;
    mode.addEventListener('change', () => { st.lakeMode = mode.value; });
    els.lakeMode = mode;
    return el('span', {class: 'mapopts'}, 'new lake ', mode, el('span', {class: 'mapnote'}, ' · click inside a lake selects it · drag a vertex, click an edge to add one, shift+click deletes'));
  }
  function sync() {
    if (!els.list || !C.map()) return;
    const el = C.el;
    els.list.replaceChildren();
    const B = bake();
    for (const r of J().rivers || []) {
      const b = B && B.byId.get(r.id);
      const T = riverTess(r);
      const row = el('div', {class: 'mapsite' + (st.sel && st.sel.id === r.id ? ' on' : '')},
        el('span', {class: 'mapkind mapkind-river'}, 'river'),
        el('span', {class: 'mapwhere'}, `${r.name || r.id} · ${fmtM(T.length / v())}` + (b ? ` · ${b.reaches} reaches${b.falls.length ? ' · ' + b.falls.length + ' falls' : ''}` + (r.joins ? ' → ' + r.joins : b.lakeOut ? ' → ' + b.lakeOut : '') : ' · INVALID')));
      const sel = el('button', {title: 'select it and centre the map on it'}, 'select');
      sel.addEventListener('click', () => { selectById('river', r.id, true); });
      const del = el('button', {title: 'delete this river (its tributaries keep their joins: they become invalid until re-snapped)'}, '×');
      del.addEventListener('click', () => edit(() => { rivers().splice(rivers().indexOf(r), 1); if (st.sel && st.sel.id === r.id) st.sel = null; }));
      row.append(sel, del);
      els.list.append(row);
    }
    for (const l of J().lakes || []) {
      const row = el('div', {class: 'mapsite' + (st.sel && st.sel.id === l.id ? ' on' : '')},
        el('span', {class: 'mapkind mapkind-lake'}, 'lake'),
        el('span', {class: 'mapwhere'}, `${l.name || l.id} · ${fmtM((l.level - sea()) / v())} asl · ${(l.points || []).length} pts`));
      const sel = el('button', {title: 'select it and centre the map on it'}, 'select');
      sel.addEventListener('click', () => { selectById('lake', l.id, true); });
      const del = el('button', {title: 'delete this lake'}, '×');
      del.addEventListener('click', () => edit(() => { lakes().splice(lakes().indexOf(l), 1); if (st.sel && st.sel.id === l.id) st.sel = null; }));
      row.append(sel, del);
      els.list.append(row);
    }
    if (!(J().rivers || []).length && !(J().lakes || []).length) els.list.append(el('div', {class: 'mapnote'}, 'no rivers or lakes — pick the River tool (click source → mouth) or the Lake tool (click a hollow)'));
    syncDerive();
    syncInspector();
  }
  function selectById(kind, id, centre) {
    const o = (kind === 'river' ? rivers() : lakes()).find(x => x.id === id);
    if (!o) return false;
    st.sel = {kind, id}; st.drawing = false;
    if (centre && (o.points || []).length) {
      let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
      for (const p of o.points) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
      C.view.cx = (x0 + x1) / 2; C.view.cz = (z0 + z1) / 2;
    }
    sync(); C.paint();
    return true;
  }
  function syncDerive() {
    const box = els.derive;
    if (!box || !C.map()) return;
    const el = C.el;
    box.replaceChildren();
    const lg = el('input', {type: 'range', min: -1, max: 3, step: 0.05, value: Math.log10(st.deriveKm2), style: 'width:120px',
                            title: 'the catchment a channel needs to count as a river, km² (log scale 0.1 .. 1000)'});
    const out = el('span', {class: 'mapunit'}, st.deriveKm2 + ' km²');
    lg.addEventListener('input', () => { st.deriveKm2 = +(10 ** +lg.value).toPrecision(2); out.textContent = st.deriveKm2 + ' km²'; });
    const go = el('button', {class: 'mapgo', title: 'run the D8 flow network over the Area (or the whole map when there is none) in a worker and preview the rivers it finds'}, 'Derive');
    go.addEventListener('click', () => derive());
    box.append(el('div', {class: 'mapbar'}, 'threshold ', lg, out, ' ', go, el('span', {class: 'mapnote'}, C.area() ? ' over the Area' : ' over the whole map')));
    const D = st.derive;
    if (!D) { box.append(el('div', {class: 'mapnote'}, 'Derive previews the river network the terrain drains into, coloured by order (1 = a headwater). Pick rivers by clicking them with the River tool, then accept.')); return; }
    if (D.running) { box.append(el('div', {class: 'mapnote'}, 'deriving over ' + D.label + '…')); return; }
    const byOrd = new Map();
    for (const r of D.recs) byOrd.set(r.order, (byOrd.get(r.order) || 0) + 1);
    const ords = [...byOrd.keys()].sort((a, b) => a - b);
    const ordSel = el('select', {title: 'accept every derived river of this order or higher'}, ...ords.map(o => el('option', {value: o}, '≥ order ' + o)));
    if (ords.includes(st.deriveOrder)) ordSel.value = st.deriveOrder;
    ordSel.addEventListener('change', () => { st.deriveOrder = +ordSel.value; });
    const allB = el('button', {title: 'write every derived river of the chosen order or higher into rivers[] (joins kept between accepted ones; one undo step)'}, 'Accept all');
    allB.addEventListener('click', () => accept(+ordSel.value));
    const pkB = el('button', {title: 'write the picked rivers into rivers[] (one undo step)'}, `Accept picked (${D.picked.size})`);
    pkB.addEventListener('click', () => accept('picked'));
    const clr = el('button', {title: 'drop the preview'}, 'Clear');
    clr.addEventListener('click', () => { st.derive = null; syncDerive(); C.paint(); });
    box.append(el('div', {class: 'mapnote'}, `${D.recs.length} rivers over ${D.label}: ` + ords.map(o => `order ${o} ×${byOrd.get(o)}`).join(', ')),
               el('div', {class: 'mapbar'}, ordSel, allB, pkB, clr));
  }

  /* ---- the inspector ---------------------------------------------------------------------- */
  function field(label, input, title) { return C.el('label', {title}, label, ' ', input); }
  function syncInspector() {
    const box = els.insp;
    if (!box || !C.map()) return;
    const el = C.el;
    box.replaceChildren();
    const r = selRiver(), l = selLake();
    if (!r && !l) { box.append(el('div', {class: 'mapnote'}, 'Select a river or a lake to edit it.')); return; }
    const o = r || l, kind = r ? 'river' : 'lake', arr = r ? rivers() : lakes();
    const idIn = el('input', {type: 'text', value: o.id, class: 'mapid', title: 'the id (unique; tributaries name their parent by it)'});
    idIn.addEventListener('change', () => {
      const nid = idIn.value.trim();
      if (!nid || arr.some(x => x !== o && x.id === nid)) { idIn.value = o.id; C.toast('that id is empty or taken', true); return; }
      edit(() => {
        if (r) for (const t of rivers()) if (t.joins === o.id) t.joins = nid;
        if (st.seeds.has(o.id)) { st.seeds.set(nid, st.seeds.get(o.id)); st.seeds.delete(o.id); }
        o.id = nid; st.sel.id = nid;
      });
    });
    const nameIn = el('input', {type: 'text', value: o.name || '', class: 'mapid', placeholder: 'name'});
    nameIn.addEventListener('change', () => edit(() => { if (nameIn.value.trim()) o.name = nameIn.value.trim(); else delete o.name; }));
    const smooth = el('input', {type: 'checkbox', title: 'smooth: a centripetal Catmull-Rom through the points (the engine tessellates it the same way); off = straight segments'});
    smooth.checked = o.smooth === true;
    smooth.addEventListener('change', () => edit(() => { o.smooth = smooth.checked; }));
    const matIn = (key, def, title) => {
      const i = el('input', {type: 'text', value: o[key] || def, list: 'mapwater-beds', style: 'width:90px', title});
      i.addEventListener('change', () => edit(() => { const s = i.value.trim(); if (s && s !== def) o[key] = s; else delete o[key]; }));
      return i;
    };
    const dl = el('datalist', {id: 'mapwater-beds'}, ...BEDS.map(b => el('option', {value: b})));
    box.append(dl, el('div', {class: 'mapbar'}, el('b', {}, kind), idIn, nameIn, el('label', {}, smooth, ' smooth')));
    if (r) riverInspector(r, box, matIn); else lakeInspector(l, box, matIn);
  }
  function riverInspector(r, box, matIn) {
    const el = C.el, B = bake(), b = B && B.byId.get(r.id);
    const num = (key, def, min, max, title, blankAuto) => {
      const i = el('input', {type: 'number', min, max, step: 1, value: r[key] == null ? (blankAuto ? '' : def) : r[key], placeholder: blankAuto ? 'auto' : '', style: 'width:54px', title});
      i.addEventListener('change', () => edit(() => { if (i.value === '' && blankAuto) delete r[key]; else { const n = Math.max(min, Math.min(max, Math.round(+i.value || 0))); if (n === def && !blankAuto) delete r[key]; else r[key] = n; } }));
      return i;
    };
    const prof = el('select', {title: 'cross-section: U (parabolic, wide and slow) or V (narrow, steep)'}, el('option', {value: 'U'}, 'U'), el('option', {value: 'V'}, 'V'));
    prof.value = r.profile === 'V' ? 'V' : 'U';
    prof.addEventListener('change', () => edit(() => { if (prof.value === 'V') r.profile = 'V'; else delete r.profile; }));
    const joins = el('select', {title: 'the river this one is a tributary of (its mouth must lie on that river’s channel)'},
                     el('option', {value: ''}, '(none)'), ...rivers().filter(o => o !== r).map(o => el('option', {value: o.id}, o.id)));
    joins.value = r.joins || '';
    joins.addEventListener('change', () => edit(() => { if (joins.value) r.joins = joins.value; else delete r.joins; }));
    box.append(el('div', {class: 'mapgrid2'},
      field('bed', matIn('bed', 'gravel', 'the channel bed: a solid or a powder material (default gravel)'), ''),
      field('stepVox', num('stepVox', 4, 1, 64, 'how far the bed falls below a reach before the surface steps down (voxels; default 4): bigger = longer, flatter pools, bigger steps'), ''),
      field('bank', num('bank', 0, 0, 1024, 'the bank: how far either side the ground blends back to the terrain (voxels); blank = auto (1.5 x the width)', true), ''),
      field('profile', prof, ''), field('joins', joins, '')));
    // the knots: x, z, and w / d in metres (blank = interpolated; the ends must carry them)
    const tbl = el('div', {class: 'maplinepts'});
    const vv = v();
    r.points.forEach((p, k) => {
      const end = k === 0 || k === r.points.length - 1;
      const xyz = q => {
        const i = el('input', {type: 'number', step: 1, value: p[q], style: 'width:62px'});
        i.addEventListener('change', () => edit(() => { p[q] = Math.round(+i.value || 0); }));
        return i;
      };
      const wd = (q, title) => {
        const i = el('input', {type: 'number', step: 0.1, min: 0, value: p.length >= 4 ? +(p[q] / vv).toFixed(2) : '', placeholder: 'auto', style: 'width:50px', title});
        i.addEventListener('change', () => edit(() => {
          if (i.value === '' && !end) { p.length = 2; return; }
          if (p.length < 4) { const c = SPL.closest(riverTess(r), p[0], p[1]); p[2] = Math.round(c && c.attrs[0] != null ? c.attrs[0] : 16); p[3] = Math.round(c && c.attrs[1] != null ? c.attrs[1] : 4); }
          p[q] = Math.round(Math.max(0, +i.value || 0) * vv);
          p[2] = Math.max(RB.K.widthMin, Math.min(RB.K.widthMax, p[2])); p[3] = Math.max(RB.K.depthMin, Math.min(RB.K.depthMax, p[3]));
        }));
        return i;
      };
      const label = k === 0 ? 'src' : k === r.points.length - 1 ? 'mouth' : String(k);
      tbl.append(el('div', {class: 'mapsite'}, el('span', {class: 'mapwhere'}, label), xyz(0), xyz(1),
        wd(2, 'channel width here, metres (blank = interpolated)'), el('span', {class: 'mapunit'}, 'm w'), wd(3, 'water depth at the centreline here, metres'), el('span', {class: 'mapunit'}, 'm d')));
    });
    box.append(tbl);
    // actions
    const amt = el('input', {type: 'range', min: 0, max: 200, value: Math.round(NEW.meander * 100), style: 'width:80px', title: 'meander amplitude (100 % = up to ~1.5 x the width on flat ground)'});
    amt.addEventListener('input', () => { NEW.meander = +amt.value / 100; });
    const seedIn = el('input', {type: 'number', min: 0, step: 1, value: NEW.seed, style: 'width:44px', title: 'the meander seed: same river + seed = same bends'});
    seedIn.addEventListener('change', () => { NEW.seed = Math.max(0, Math.round(+seedIn.value || 0)); });
    const mb = el('button', {title: 'bend the river: a sum of sines along its normal, wavelength ~12 x its width, wider where the ground is flat; the source and mouth stay (one undo step)'}, 'Meander');
    mb.addEventListener('click', () => meander(r, NEW.meander, NEW.seed));
    const sn = el('button', {title: 'snap the mouth again: onto a river (joins it), into a lake or onto the coast'}, 'Snap mouth');
    sn.addEventListener('click', () => { edit(() => { const s = snapMouth(r, true); C.status(`${r.id}: the mouth ${snapText(s)}`); }); });
    const cont = el('button', {title: 'add knots after the mouth (Enter / double-click ends)'}, st.drawing ? 'Finish' : 'Continue');
    cont.addEventListener('click', () => { if (st.drawing) finish(); else { st.drawing = true; C.setTool('river'); sync(); } });
    box.append(el('div', {class: 'mapbar'}, mb, amt, ' seed ', seedIn, sn, cont));
    if (!b) { box.append(el('div', {class: 'mapnote bad'}, 'the engine would REFUSE this river: ' + (RB.parseRiver(r).error || '?'))); return; }
    const fordsHere = (B.fords || []).filter(f => f.river === r.id);
    box.append(el('div', {class: 'mapnote'},
      `${fmtM(b.samples.s[b.samples.s.length - 1] / v())} · ${b.reaches} reaches · ${b.pieces} pieces · ${b.falls.length} waterfall(s)` +
      ` · ${fmtM((b.samples.S[0] - sea()) / v())} → ${fmtM((b.endLevel - sea()) / v())} asl · drains to ` +
      (b.drainsTo === 'river' ? `"${r.joins}"` : b.drainsTo === 'lake' ? `lake "${b.lakeOut}"` : 'the sea level') +
      (b.lakeIn ? ` · sourced in lake "${b.lakeIn}"` : '') + (fordsHere.length ? ` · ${fordsHere.length} ford(s): ${[...new Set(fordsHere.map(f => f.road))].join(', ')}` : '')));
    for (const w of b.warnings) box.append(el('div', {class: 'mapwarn'}, w));
    box.append(profilePanel(r, b));
  }
  function lakeInspector(l, box, matIn) {
    const el = C.el, vv = v();
    const p = RB.parseLake(l);
    let lo = l.level - 50 * vv, hi = l.level + 50 * vv;
    if (!p.error) {
      const L = RB.lakePoly(p), stp = Math.max(16, Math.round(Math.max(L.x1 - L.x0, L.z1 - L.z0) / 30));
      let gmin = Infinity;
      for (let z = L.z0; z <= L.z1; z += stp) for (let x = L.x0; x <= L.x1; x += stp) if (RB.lakeInside(L.poly, x, z)) gmin = Math.min(gmin, ground(x, z));
      if (isFinite(gmin)) lo = Math.min(lo, Math.floor(gmin));
    }
    const lvl = el('input', {type: 'range', min: Math.round(lo), max: Math.round(hi), step: 1, value: l.level, style: 'width:160px',
                             title: 'the water level (absolute Y): with "re-flood" on, the outline follows the basin at this level'});
    const out = el('span', {class: 'mapunit'}, fmtM((l.level - sea()) / vv) + ' asl');
    let snap = null;
    lvl.addEventListener('input', () => {
      if (!snap) snap = C.snapshot();
      const nv = +lvl.value;
      if (st.reflood && !reflood(l, nv)) l.level = nv; else if (!st.reflood) l.level = nv;
      out.textContent = fmtM((l.level - sea()) / vv) + ' asl';
      C.markDirty(true); C.paint();
    });
    lvl.addEventListener('change', () => { if (snap) C.pushUndo(snap); snap = null; sync(); });
    const rf = el('input', {type: 'checkbox', title: 're-flood: the outline follows the level (the basin round the lake’s deepest point); off = keep the polygon, move only the level'});
    rf.checked = st.reflood;
    rf.addEventListener('change', () => { st.reflood = rf.checked; });
    const band = el('input', {type: 'number', min: 4, max: 128, step: 1, value: l.band == null ? 24 : l.band, style: 'width:50px',
                              title: 'the shore band (voxels, 4..128): how far outside the polygon the engine raises low ground to level + 1 (the sill) and lays the shore'});
    band.addEventListener('change', () => edit(() => { const n = Math.max(4, Math.min(128, Math.round(+band.value || 24))); if (n === 24) delete l.band; else l.band = n; }));
    box.append(el('div', {class: 'mapbar'}, 'level ', lvl, out, el('label', {}, rf, ' re-flood')),
               el('div', {class: 'mapgrid2'}, field('bed', matIn('bed', 'shore_mud', 'the lake bed material (default shore_mud)'), ''),
                 field('shore', matIn('shore', 'sand', 'the shore material in the band (default sand)'), ''), field('band', band, '')));
    if (p.error) { box.append(el('div', {class: 'mapnote bad'}, 'the engine would REFUSE this lake: ' + p.error)); return; }
    const L = {...p, ...RB.lakePoly(p)};
    let area = 0;
    for (let i = 0, k = L.poly.length - 1; i < L.poly.length; k = i++) area += L.poly[k][0] * L.poly[i][1] - L.poly[i][0] * L.poly[k][1];
    const sills = sillOf(l);
    const raise = sills.reduce((m, c) => Math.max(m, c[2]), 0);
    box.append(el('div', {class: 'mapnote'}, `${(Math.abs(area) / 2 / vv / vv / 1e6).toFixed(3)} km² · outline ${L.poly.length} edges (as the engine bakes it)` +
      (st.seeds.has(l.id) ? '' : ' · drawn by hand or loaded: re-flood starts from its lowest point')));
    box.append(el('div', {class: sills.length ? 'mapwarn' : 'mapnote'}, sills.length
      ? `SILL: ${sills.length} shore-band sample(s) outside the polygon are below level + 1 (red on the map); the engine raises them by up to ${fmtM(raise / vv)} to hold the water — move the outline or lower the level if that wall is not wanted`
      : 'no sill needed: the ground round the polygon stands above the level'));
    const cont = el('button', {title: 'add vertices (Enter / double-click ends)'}, st.drawing ? 'Finish' : 'Add vertices');
    cont.addEventListener('click', () => { if (st.drawing) finish(); else { st.drawing = true; C.setTool('lake'); sync(); } });
    box.append(el('div', {class: 'mapbar'}, cont));
  }

  /* ---- THE PROFILE: the live bake, the engine's bake of the saved river beside it ---------- */
  function enginePart(r) {
    const ck = C.check && C.check();
    if (!ck || ck.unavailable || !ck.ok) return {why: ck ? (ck.unavailable ? 'the engine could not be asked' : 'the engine refuses the saved map') : 'asking the engine…'};
    const e = (ck.rivers || []).find(x => x.id === r.id);
    const sj = C.savedJson && C.savedJson();
    const sr = sj && (sj.rivers || []).find(x => x.id === r.id);
    if (!e || !sr) return {why: 'not saved yet: Save and the engine bakes it (dashed)'};
    return {e, stale: JSON.stringify(sr) !== JSON.stringify(r)};
  }
  function profilePanel(r, b) {
    const el = C.el;
    const cv = el('canvas', {width: 320, height: 130, class: 'mapprofile mapprofile-river',
                             title: 'the river’s longitudinal profile: the ground under the centreline (green) and the water surface (blue: flat reaches, steps, waterfalls in white), metres above sea; the ENGINE’s bake of the saved river dashed'});
    const note = el('div', {class: 'mapnote'});
    const E = enginePart(r);
    const draw = probeS => drawProfile(cv, r, b, E, note, probeS);
    draw();
    cv.addEventListener('mousemove', ev => {
      const rc = cv.getBoundingClientRect(), S = b.samples.s[b.samples.s.length - 1];
      const s = S * Math.max(0, Math.min(1, (ev.clientX - rc.left - 4) / (cv.width - 8)));
      let i = 0;
      while (i + 1 < b.samples.s.length && b.samples.s[i + 1] < s) i++;
      st.probe = [b.samples.x[i], b.samples.z[i]];
      draw(s); C.paint();
    });
    cv.addEventListener('mouseleave', () => { st.probe = null; draw(); C.paint(); });
    return el('div', {class: 'mapprofilebox'}, el('div', {class: 'mapsub'}, 'Profile (live: the engine’s rules on the height plane)'), cv, note);
  }
  function drawProfile(cv, r, b, E, note, probeS) {
    const g = cv.getContext('2d'), W = cv.width, Hh = cv.height, vv = v(), sl = sea();
    const s = b.samples, N = s.s.length, S = Math.max(1, s.s[N - 1]);
    g.fillStyle = '#0e1219'; g.fillRect(0, 0, W, Hh);
    g.font = '10px monospace';
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < N; i++) { lo = Math.min(lo, s.g[i], s.S[i] - s.d[i]); hi = Math.max(hi, s.g[i], s.S[i]); }
    if (E.e) for (let i = 0; i < E.e.profile.s.length; i++) { lo = Math.min(lo, E.e.profile.ground[i], E.e.profile.y[i]); hi = Math.max(hi, E.e.profile.ground[i], E.e.profile.y[i]); }
    if (hi - lo < 20) { const m = (hi + lo) / 2; hi = m + 10; lo = m - 10; }
    const X = q => 4 + (q / S) * (W - 8), Y = h => Math.round(Hh - 14 - (h - lo) / (hi - lo) * (Hh - 28));
    // ground silhouette, the water body between the surface and the bed
    for (let i = 0; i + 1 < N; i++) {
      const x0 = Math.round(X(s.s[i])), x1 = Math.max(x0 + 1, Math.round(X(s.s[i + 1])));
      for (let x = x0; x < x1; x++) {
        g.fillStyle = '#3a4a3a'; g.fillRect(x, Y(s.g[i]), 1, Hh - 12 - Y(s.g[i]));
        const bed = Math.min(s.g[i], s.S[i] - s.d[i]);
        g.fillStyle = 'rgba(60,140,240,0.55)'; g.fillRect(x, Y(s.S[i]), 1, Math.max(1, Y(bed) - Y(s.S[i])));
      }
    }
    // the surface: flat reaches, the steps
    g.strokeStyle = '#7cc0ff'; g.lineWidth = 2;
    g.beginPath();
    for (let i = 0; i < N; i++) { const x = X(s.s[i]), y = Y(s.S[i]); if (i) g.lineTo(x, y); else g.moveTo(x, y); }
    g.stroke();
    for (const f of b.falls) {
      let i = 0;
      while (i + 1 < N && !(s.x[i] === f[0] && s.z[i] === f[1])) i++;
      g.fillStyle = '#ffffff'; g.fillRect(Math.round(X(s.s[i])) - 1, Y(s.S[i] + f[2]), 3, Math.max(2, Y(s.S[i]) - Y(s.S[i] + f[2])));
    }
    // the engine's bake of the saved river, dashed
    if (E.e) {
      const P = E.e.profile;
      g.setLineDash([3, 3]); g.lineWidth = 1;
      g.strokeStyle = '#ffb454'; g.beginPath(); P.s.forEach((q, i) => (i ? g.lineTo(X(q), Y(P.y[i])) : g.moveTo(X(q), Y(P.y[i])))); g.stroke();
      g.strokeStyle = '#a0c080'; g.beginPath(); P.s.forEach((q, i) => (i ? g.lineTo(X(q), Y(P.ground[i])) : g.moveTo(X(q), Y(P.ground[i])))); g.stroke();
      g.setLineDash([]);
    }
    // the knots ticked along the bottom
    g.fillStyle = '#c7d2e3';
    for (const p of r.points) {
      let bi = 0, bd = Infinity;
      for (let i = 0; i < N; i++) { const d = (s.x[i] - p[0]) ** 2 + (s.z[i] - p[1]) ** 2; if (d < bd) { bd = d; bi = i; } }
      g.fillRect(Math.round(X(s.s[bi])), Hh - 11, 1, 4);
    }
    g.fillText(fmtM((hi - sl) / vv), 3, 10);
    g.fillText(fmtM((lo - sl) / vv), 3, Hh - 2);
    g.textAlign = 'right'; g.fillText(fmtM(S / vv), W - 3, Hh - 2); g.textAlign = 'left';
    let line = `live: ${b.reaches} reaches, ${b.falls.length} waterfall(s), surface ${fmtM((s.S[0] - sl) / vv)} → ${fmtM((s.S[N - 1] - sl) / vv)} asl. ` +
      (E.e ? `Engine (dashed, the saved river on the real ground with its noise): ${E.e.reaches} reaches, ${E.e.waterfalls.length} waterfall(s)` + (E.stale ? ' — EDITED since the save; Save to re-bake' : '') : E.why);
    if (probeS != null) {
      let i = 0;
      while (i + 1 < N && s.s[i + 1] < probeS) i++;
      g.strokeStyle = 'rgba(255,90,90,0.8)'; g.lineWidth = 1;
      g.beginPath(); g.moveTo(Math.round(X(probeS)) + 0.5, 0); g.lineTo(Math.round(X(probeS)) + 0.5, Hh - 12); g.stroke();
      line = `at ${fmtM(probeS / vv)}: water ${fmtM((s.S[i] - sl) / vv)} asl, ground ${fmtM((s.g[i] - sl) / vv)}, ${fmtM(2 * s.hw[i] / vv)} wide, ${fmtM(s.d[i] / vv)} deep`;
    }
    note.textContent = line;
    note.classList.toggle('bad', !!(E.e && E.stale));
  }

  /* ---- registration ---------------------------------------------------------------------- */
  function problems() {
    if (!C.map()) return null;
    const out = [], seen = new Set();
    for (const r of J().rivers || []) {
      const p = RB.parseRiver(r);
      if (p.error) out.push(p.error);
      if (seen.has('r' + r.id)) out.push(`two rivers are called "${r.id}"`);
      seen.add('r' + r.id);
      if (r.joins && !(J().rivers || []).some(o => o.id === r.joins)) out.push(`river "${r.id}" joins "${r.joins}", which is not a river on this map`);
    }
    for (const l of J().lakes || []) {
      const p = RB.parseLake(l);
      if (p.error) out.push(p.error);
      if (seen.has('l' + l.id)) out.push(`two lakes are called "${l.id}"`);
      seen.add('l' + l.id);
    }
    const B = bake();
    if (B) for (const e of B.errors) if (/cycle/.test(e)) out.push(e);
    return out.length ? out : null;
  }
  C.registerTool({
    key: 'river', label: 'River', group: 'vector', order: 15,
    title: 'RIVER: click from the SOURCE down to the MOUTH (double-click / Enter ends; the mouth snaps onto a river, a lake or the coast); drag a knot, its ◆ sets the width, Alt+drag the depth; the engine carves the channel and fills it as flat reaches stepping down. Derive / Meander in the Water panel',
    opts: riverOpts, down: riverDown, move: riverMove, up: riverUp, paint,
    statusAt() {
      const d = st.drag;
      if (!d || !d.r) return null;
      const p = d.r.points[d.k];
      if (!p) return null;
      if (d.kind === 'width') return `${d.r.id} knot ${d.k}: width ${fmtM(p[2] / v())}`;
      if (d.kind === 'depth') return `${d.r.id} knot ${d.k}: depth ${fmtM(p[3] / v())}`;
      return null;
    },
    onKey(ev) { if ((ev.key === 'Enter' || ev.key === 'Escape') && st.drawing) { finish(); return true; } return false; },
    dblclick() { if (!st.drawing) return false; finish(); return true; },
    guide: ['River — a river from its source to its mouth',
      'A real river the engine builds: it carves a channel along a spline, fills it with water at rest in flat REACHES that step down (a waterfall where a step drops more than 2 m), holds the banks, and ends in the sea, a lake or the river it joins. The surface never runs uphill: the engine takes the lowest ground so far, source to mouth.',
      'Click the source, then along the course to the mouth (double-click / Enter ends). The mouth SNAPS: onto another river (it becomes a tributary: joins), into a lake, or onto the coast. Drag a knot to move it, drag the ◆ beside a knot for the width, Alt+drag a knot up / down for the depth, click the selected river to insert a knot, shift+click a knot to delete it. The Water panel lists them; its inspector sets bed, stepVox (the reach step), bank, U / V profile, the knots’ width and depth, and draws the PROFILE: ground and water along the river. Derive finds the rivers the terrain drains into (threshold in km² of catchment): click them to pick, then Accept. Meander bends a river (wider loops where it is flat).',
      'The live preview uses the HEIGHT PLANE as the ground; the engine’s bake adds the seeded terrain noise and the sculpt layer, so reaches can differ a little (the profile draws the engine’s bake of the saved river dashed). Rivers never change the planes: the engine carves at load. A road crossing a river is a FORD (the road wins inside its width).'],
  });
  C.registerTool({
    key: 'lake', label: 'Lake', group: 'vector', order: 16,
    title: 'LAKE: click inside a hollow and it fills to its spill level (Priority-Flood); the level slider in the Water panel re-floods the outline; polygon mode draws the outline by hand',
    opts: lakeOpts, down: lakeDown, move: lakeMove, up: lakeUp,
    onKey(ev) { if ((ev.key === 'Enter' || ev.key === 'Escape') && st.drawing) { finish(); return true; } return false; },
    dblclick() { if (!st.drawing) return false; const l = selLake(); if (l && l.points.length > 3) { const a = l.points[l.points.length - 1], b = l.points[l.points.length - 2]; if (Math.hypot(px(a[0] - b[0]), px(a[1] - b[1])) < 4) l.points.pop(); } finish(); return true; },
    guide: ['Lake — still water filling a basin to its level',
      'A lake the engine fills: every column inside the outline whose ground is below the level becomes water up to it, and any low ground in the shore band just outside is raised to hold it (a sill). Small round ponds are still the Water site.',
      'Basin mode: click inside a hollow — the lake fills it to the SPILL level (the lowest point of its rim; a whole-map Priority-Flood) and its outline follows the water. In the Water panel, the level slider raises or lowers it and the outline re-floods live. Polygon mode: click the outline yourself (click the first vertex, double-click or Enter ends). Drag a vertex, click an edge to add one, shift+click deletes one. Red squares on the selected lake are where the engine would raise ground (a sill): move the outline or lower the level if that wall is not wanted.',
      'The outline is drawn smooth (the engine tessellates it the same way). The level is the plane’s heights; the engine adds terrain noise, so a lake at its exact spill level can leak a little at the rim — the sill holds it.'],
  });

  return {
    st, buildPanel, sync, paint, bake, derive, accept, meander, basinAt, reflood, problems, finish, selectById,
    reset() { st.sel = null; st.drawing = false; st.drag = null; st.derive = null; st.probe = null; st.seeds = new Map(); bakeC = {key: '', t: 0, out: null}; },
    afterRestore() { if (st.sel && !(st.sel.kind === 'river' ? selRiver() : selLake())) { st.sel = null; st.drawing = false; } st.drag = null; bakeC = {key: '', t: 0, out: null}; sync(); },
    invalidate() { bakeC = {key: '', t: 0, out: null}; },
  };
}
