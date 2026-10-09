/* mapoverlay.js — the World map page's ANALYSIS OVERLAYS and the MEASURE tool
 * (docs/PLAN_worldbuilder.md K1 item 5):
 *
 *   slope bands   the topo tiles coloured by the GAME's slope bands instead of
 *                 height (map.js heightImage reads disp.overlay / disp.slopeBands):
 *                 walkable (soil and powders hold, ~35 degrees, the talus the
 *                 erosion uses), steep (still climbable: the player steps
 *                 0.58 m per 0.1 m voxel column, so stairs up to ~80 degrees),
 *                 cliff (at or past the map's terrain.rockSlope, 2.5:1 = 68
 *                 degrees shipped: bare rock, no soil, no snow, no plants);
 *   flow          drainage area on a ~51 m proxy of the whole plane (D8 on the
 *                 Priority-Flood epsilon-filled surface, hydrology.js): where
 *                 rivers would run, brighter = more water;
 *   lakes         where Priority-Flood would hold standing water (the spill
 *                 depth of every closed basin);
 *   grid          a kilometre grid (1 / 2 / 5 / 10 km by zoom), labelled;
 *   minimap       the whole extent, hillshaded, with the view's box; click it
 *                 to jump there.
 *
 * The Measure tool: click points along a route, drag one to move it, shift+
 * click clears; the strip under the toolbar shows the horizontal and 3D
 * distance, the climb and descent, the steepest and mean GRADE, and the
 * elevation profile (hover it to find the spot on the map). Measuring writes
 * nothing. */

import * as MT from './maptiles.js';
import * as TG from './terraingen.js';

export function createOverlayLayer(C) {
  const st = {
    flow: null,        // {gen, A, depth, w, hh, x0, z0, cell, imgFlow, imgLake}
    flowTimer: 0, flowJob: null,
    mini: null,        // {gen, canvas, box}
    measure: {pts: [], drag: -1, hoverS: null},
  };
  const els = {};
  const disp = C.disp;
  if (disp.overlay == null) disp.overlay = 'none';
  if (disp.grid == null) disp.grid = false;
  if (disp.minimap == null) disp.minimap = true;
  const v = () => C.vpm();

  /* ---- the slope bands: from the map's own numbers ---- */
  function slopeBands() {
    const t = C.map() && C.map().json.terrain;
    const rock = t && t.rockSlope > 0 ? Math.atan(t.rockSlope / 256) * 180 / Math.PI : 68.2;
    return [35, rock];
  }
  /* ---- flow + lakes on a proxy of the whole plane ---- */
  function wantFlow() { return disp.overlay === 'flow' || disp.overlay === 'lakes'; }
  // one pending timer and one job in flight at most: a repaint never restarts them
  function scheduleFlow() {
    if (!wantFlow() || !C.map() || st.flowTimer || st.flowJob) return;
    const P = C.map().H;
    if (st.flow && st.flow.gen === P.gen) return;
    st.flowTimer = setTimeout(() => { st.flowTimer = 0; runFlow(); }, st.flow ? 1200 : 50);
  }
  function runFlow() {
    const m = C.map();
    if (!m || !wantFlow()) return;
    const P = m.H, gen = P.gen;
    let f = 1;
    while ((P.posts[0] / f) * (P.posts[1] / f) > 260000) f *= 2;
    const g = C.coarseGrid(null, f);
    if (!g) return;
    const run = C.runJob({job: 'flow', grid: {...g.grid, h: Float32Array.from(g.grid.h)}, seaY: C.seaY()}, {quiet: true, key: 'flow', onCancel: () => { st.flowJob = null; }});
    st.flowJob = run;
    run.then(res => {
      st.flowJob = null;
      if (run.cancelled || C.map() !== m) return;
      st.flow = {gen, ...g.grid, A: res.A, depth: res.depth, imgFlow: null, imgLake: null};
      C.paint();
      C.status(`flow overlay: ${g.grid.w}×${g.grid.hh} cells of ${(g.grid.cell / v()).toFixed(1)} m` + (P.gen !== gen ? ' (the plane changed since: recomputing)' : ''));
      if (P.gen !== gen) scheduleFlow();
    }).catch(e => { st.flowJob = null; if (!run.cancelled) C.status('flow overlay: ' + e.message); });
  }
  function flowImage(kind) {
    const F = st.flow;
    if (!F) return null;
    const key = kind === 'flow' ? 'imgFlow' : 'imgLake';
    if (F[key]) return F[key];
    const cv = document.createElement('canvas');
    cv.width = F.w; cv.height = F.hh;
    const ctx = cv.getContext('2d'), im = ctx.createImageData(F.w, F.hh), d = im.data;
    if (kind === 'flow') {
      // log-scaled: a stream from ~20 cells (a few km²... a gully at 51 m) up to the biggest river
      let mx = 1;
      for (let n = 0; n < F.A.length; n++) if (F.A[n] > mx) mx = F.A[n];
      const lo = Math.log(20), hi = Math.log(mx);
      for (let n = 0; n < F.A.length; n++) {
        const t = (Math.log(Math.max(1, F.A[n])) - lo) / Math.max(1e-6, hi - lo);
        if (t <= 0) continue;
        const o = n * 4;
        d[o] = 40 + 60 * (1 - t); d[o + 1] = 150 + 60 * (1 - t); d[o + 2] = 255; d[o + 3] = Math.min(255, 70 + 185 * t);
      }
    } else {
      for (let n = 0; n < F.depth.length; n++) {
        const dep = F.depth[n];
        if (!(dep > 2)) continue;
        const o = n * 4, t = Math.min(1, dep / (40 * v()));
        d[o] = 30; d[o + 1] = 110 - 40 * t; d[o + 2] = 230; d[o + 3] = 120 + 110 * t;
      }
    }
    ctx.putImageData(im, 0, 0);
    F[key] = cv;
    return cv;
  }
  /* ---- the minimap ---- */
  const MINI = 168;
  function miniImage() {
    const m = C.map();
    if (!m) return null;
    const P = m.H;
    if (st.mini && st.mini.gen === P.gen) return st.mini;
    if (st.mini && performance.now() - st.mini.at < 1500) return st.mini;   // at most every 1.5 s while painting
    const e = m.json.extent, side = Math.max(e.size[0], e.size[1]);
    const w = Math.max(8, Math.round(MINI * e.size[0] / side)), hh = Math.max(8, Math.round(MINI * e.size[1] / side));
    const h = new Float32Array(w * hh), cell = side / MINI;
    for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) h[j * w + i] = MT.heightAt(P, e.min[0] + (i + 0.5) * cell, e.min[1] + (j + 0.5) * cell);
    const [lo, hi] = C.rampRange();
    const rgba = TG.shade(h, w, hh, cell, {seaY: C.seaY(), lo, hi, tint: 0.85, zf: 3});
    const cv = document.createElement('canvas');
    cv.width = w; cv.height = hh;
    cv.getContext('2d').putImageData(new ImageData(rgba, w, hh), 0, 0);
    st.mini = {gen: P.gen, canvas: cv, w, hh, cell, x0: e.min[0], z0: e.min[1], at: performance.now()};
    return st.mini;
  }
  /** Where the minimap sits: the bottom-right corner, at most a fifth of the
   *  canvas wide; null when the canvas is too small to spare the corner (it
   *  would eat the clicks of the tools). */
  function miniRect(ctx) {
    const W = ctx.canvas.clientWidth || ctx.canvas.width, Hh = ctx.canvas.clientHeight || ctx.canvas.height, M = st.mini;
    if (!M || W < 760 || Hh < 360) return null;
    const k = Math.min(1, W * 0.2 / M.w, Hh * 0.35 / M.hh), w = Math.round(M.w * k), h = Math.round(M.hh * k);
    return [W - w - 10, Hh - h - 10, w, h];
  }
  function early(ev) {
    // a press on the minimap jumps the view there (any tool)
    if (!disp.minimap || !st.mini || ev.button !== 0) return false;
    const c = ev.target, r = c.getBoundingClientRect();
    const x = ev.clientX - r.left - c.clientLeft, y = ev.clientY - r.top - c.clientTop;
    const rr = miniRect(c.getContext('2d'));
    if (!rr) return false;
    const [mx, my, mw, mh] = rr;
    if (x < mx || y < my || x > mx + mw || y > my + mh) return false;
    C.view.cx = st.mini.x0 + (x - mx) / mw * st.mini.w * st.mini.cell;
    C.view.cz = st.mini.z0 + (y - my) / mh * st.mini.hh * st.mini.cell;
    C.paint();
    return true;
  }
  /* ---- painting (every repaint) ---- */
  function paint(ctx) {
    const m = C.map();
    if (!m) return;
    const SX = C.SX, SZ = C.SZ, view = C.view;
    if (wantFlow()) {
      scheduleFlow();
      const img = flowImage(disp.overlay);
      if (img) {
        const F = st.flow;
        ctx.imageSmoothingEnabled = disp.overlay === 'lakes';
        ctx.drawImage(img, SX(F.x0 - F.cell / 2), SZ(F.z0 - F.cell / 2), F.w * F.cell * view.scale, F.hh * F.cell * view.scale);
        ctx.imageSmoothingEnabled = false;
      }
    }
    if (disp.grid) paintGrid(ctx);
    paintMeasure(ctx);
    if (disp.minimap) {
      const M = miniImage();
      const rr = M ? miniRect(ctx) : null;
      if (rr) {
        const [mx, my, mw, mh] = rr, ks = mw / M.w;
        ctx.fillStyle = '#0e1219'; ctx.fillRect(mx - 2, my - 2, mw + 4, mh + 4);
        ctx.drawImage(M.canvas, mx, my, mw, mh);
        ctx.strokeStyle = '#2a3040'; ctx.lineWidth = 1; ctx.strokeRect(mx - 1.5, my - 1.5, mw + 3, mh + 3);
        // the view's box
        const W = ctx.canvas.clientWidth || ctx.canvas.width, Hh = ctx.canvas.clientHeight || ctx.canvas.height;
        const x0 = view.cx - W / 2 / view.scale, z0 = view.cz - Hh / 2 / view.scale;
        const bx = mx + (x0 - M.x0) / M.cell * ks, bz = my + (z0 - M.z0) / M.cell * ks, bw = W / view.scale / M.cell * ks, bh = Hh / view.scale / M.cell * ks;
        const cl = (a, lo, hi) => Math.max(lo, Math.min(hi, a));
        const ax = cl(bx, mx, mx + mw), az = cl(bz, my, my + mh), ex = cl(bx + bw, mx, mx + mw), ez = cl(bz + bh, my, my + mh);
        ctx.strokeStyle = '#ffd866'; ctx.lineWidth = 1;
        ctx.strokeRect(Math.round(ax) + 0.5, Math.round(az) + 0.5, Math.max(2, ex - ax), Math.max(2, ez - az));
      }
    }
  }
  function paintGrid(ctx) {
    const view = C.view, SX = C.SX, SZ = C.SZ, W = ctx.canvas.clientWidth || ctx.canvas.width, Hh = ctx.canvas.clientHeight || ctx.canvas.height;
    const kmPx = 1000 * v() * view.scale;
    let stepKm = 1;
    for (const s of [0.1, 0.25, 0.5, 1, 2, 5, 10, 20]) { stepKm = s; if (s * kmPx >= 70) break; }
    const step = stepKm * 1000 * v();
    const x0 = view.cx - W / 2 / view.scale, x1 = view.cx + W / 2 / view.scale, z0 = view.cz - Hh / 2 / view.scale, z1 = view.cz + Hh / 2 / view.scale;
    ctx.strokeStyle = 'rgba(255,255,255,0.22)'; ctx.lineWidth = 1; ctx.fillStyle = 'rgba(230,236,245,0.85)'; ctx.font = '10px monospace';
    ctx.beginPath();
    for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) { const s = Math.round(SX(x)) + 0.5; ctx.moveTo(s, 0); ctx.lineTo(s, Hh); }
    for (let z = Math.ceil(z0 / step) * step; z <= z1; z += step) { const s = Math.round(SZ(z)) + 0.5; ctx.moveTo(0, s); ctx.lineTo(W, s); }
    ctx.stroke();
    const lab = q => (Math.abs(q) < 1e-6 ? '0' : (q / (1000 * v())).toFixed(stepKm < 1 ? 2 : 0)) + ' km';
    for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) ctx.fillText(lab(x), Math.round(SX(x)) + 3, 11);
    for (let z = Math.ceil(z0 / step) * step; z <= z1; z += step) ctx.fillText(lab(z), 3, Math.round(SZ(z)) - 3);
  }
  /* ---- MEASURE ---- */
  const M = st.measure;
  /** The route sampled every `step` voxels: [{x, z, s, h}] (h = the plane's height). */
  function sampleRoute(step) {
    const out = [];
    if (M.pts.length < 2 || !C.map()) return out;
    let s = 0;
    for (let k = 0; k + 1 < M.pts.length; k++) {
      const a = M.pts[k], b = M.pts[k + 1], L = Math.hypot(b[0] - a[0], b[1] - a[1]);
      const n = Math.max(1, Math.ceil(L / step));
      for (let q = k ? 1 : 0; q <= n; q++) {
        const t = q / n, x = a[0] + (b[0] - a[0]) * t, z = a[1] + (b[1] - a[1]) * t;
        out.push({x, z, s: s + L * t, h: C.heightAt(x, z)});
      }
      s += L;
    }
    return out;
  }
  /** The route's numbers: horizontal and 3D length, climb, descent, steepest
   *  and mean grade (% = rise / run * 100, over a ~one-post window). */
  function stats(R) {
    if (R.length < 2) return null;
    let len3 = 0, up = 0, down = 0, maxG = 0;
    for (let k = 1; k < R.length; k++) {
      const dh = R[k].h - R[k - 1].h, ds = R[k].s - R[k - 1].s;
      len3 += Math.hypot(ds, dh);
      if (dh > 0) up += dh; else down -= dh;
      if (ds > 0) maxG = Math.max(maxG, Math.abs(dh) / ds);
    }
    const L = R[R.length - 1].s;
    return {L, len3, up, down, maxG: maxG * 100, meanG: L > 0 ? (up + down) / L * 100 : 0, lo: Math.min(...R.map(r => r.h)), hi: Math.max(...R.map(r => r.h))};
  }
  function syncMeasure() {
    if (!els.mInfo) return;
    const R = sampleRoute(Math.max(C.map() ? C.map().H.sp : 64, 16)), S = stats(R), f = C.fmtM, sea = C.seaY();
    els.mInfo.textContent = !S ? (M.pts.length ? 'click the next point' : 'click points along a route (shift+click clears)')
      : `${f(S.L / v())} (3D ${f(S.len3 / v())}) · climb ${f(S.up / v())} · descent ${f(S.down / v())} · grade max ${S.maxG.toFixed(0)} %, mean ${S.meanG.toFixed(0)} % · ${f((S.lo - sea) / v())}..${f((S.hi - sea) / v())} asl`;
    drawProfile(R, S);
  }
  function drawProfile(R, S) {
    const cv = els.mChart;
    if (!cv) return;
    const ctx = cv.getContext('2d'), W = cv.width, Hh = cv.height;
    ctx.fillStyle = '#0e1219'; ctx.fillRect(0, 0, W, Hh);
    if (!S) return;
    const lo = S.lo - 5, hi = Math.max(S.hi, S.lo + 50) + 5, sea = C.seaY();
    const X = s => 1 + (W - 2) * s / S.L, Y = h => Hh - 3 - (Hh - 6) * (h - lo) / (hi - lo);
    // ground, coloured by the slope band of each step
    const [walk, rock] = slopeBands();
    for (let k = 1; k < R.length; k++) {
      const g = Math.atan(Math.abs(R[k].h - R[k - 1].h) / Math.max(1e-6, R[k].s - R[k - 1].s)) * 180 / Math.PI;
      ctx.fillStyle = g >= rock ? '#c8503c' : g >= walk ? '#c9a640' : '#4f8f48';
      const x0 = Math.floor(X(R[k - 1].s)), x1 = Math.max(x0 + 1, Math.ceil(X(R[k].s)));
      const top = Math.round(Y(Math.max(R[k].h, R[k - 1].h)));
      ctx.fillRect(x0, top, x1 - x0, Hh - top);
    }
    if (sea > lo && sea < hi) { ctx.fillStyle = 'rgba(80,140,220,0.5)'; ctx.fillRect(0, Math.round(Y(sea)), W, 1); }
    if (M.hoverS != null) { ctx.fillStyle = '#ffd866'; ctx.fillRect(Math.round(X(M.hoverS)), 0, 1, Hh); }
    ctx.fillStyle = '#c7d2e3'; ctx.font = '10px monospace';
    ctx.fillText(C.fmtM((S.hi - sea) / v()), 3, 10); ctx.fillText(C.fmtM((S.lo - sea) / v()), 3, Hh - 3);
    ctx.textAlign = 'right'; ctx.fillText(C.fmtM(S.L / v()), W - 3, 10); ctx.textAlign = 'left';
  }
  function paintMeasure(ctx) {
    if (!M.pts.length) return;
    const SX = C.SX, SZ = C.SZ;
    ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 2; ctx.setLineDash([]);
    ctx.beginPath();
    M.pts.forEach((p, k) => (k ? ctx.lineTo(SX(p[0]), SZ(p[1])) : ctx.moveTo(SX(p[0]), SZ(p[1]))));
    ctx.stroke();
    ctx.strokeStyle = '#ff5ad0'; ctx.lineWidth = 1; ctx.stroke();
    ctx.font = '10px monospace';
    let s = 0;
    M.pts.forEach((p, k) => {
      if (k) s += Math.hypot(p[0] - M.pts[k - 1][0], p[1] - M.pts[k - 1][1]);
      const x = Math.round(SX(p[0])), z = Math.round(SZ(p[1]));
      ctx.fillStyle = '#ff5ad0'; ctx.fillRect(x - 3, z - 3, 7, 7);
      ctx.fillStyle = '#0e1219'; ctx.fillRect(x - 1, z - 1, 3, 3);
      if (k) { ctx.fillStyle = '#ffd0f0'; ctx.fillText(C.fmtM(s / v()), x + 6, z + 12); }
    });
    if (M.hoverS != null) {
      const R = sampleRoute(64), q = R.find(r => r.s >= M.hoverS) || R[R.length - 1];
      if (q) { ctx.strokeStyle = '#ffd866'; ctx.beginPath(); ctx.arc(SX(q.x), SZ(q.z), 6, 0, Math.PI * 2); ctx.stroke(); }
    }
  }
  function measureOpts() {
    const el = C.el;
    els.mInfo = el('span', {class: 'mapnote', style: 'color:#ffd0f0'});
    els.mChart = el('canvas', {width: 420, height: 80, class: 'mapprofile', style: 'width:420px;height:80px', title: 'the elevation profile of the route: green walkable, yellow steep, red cliff (the game’s slope bands); hover to find the spot on the map'});
    els.mChart.addEventListener('mousemove', ev => {
      const R = sampleRoute(64), S = stats(R);
      if (!S) return;
      const r = els.mChart.getBoundingClientRect();
      M.hoverS = Math.max(0, Math.min(S.L, (ev.clientX - r.left) / r.width * S.L));
      drawProfile(sampleRoute(Math.max(C.map().H.sp, 16)), stats(sampleRoute(Math.max(C.map().H.sp, 16)))); C.schedulePaint();
    });
    els.mChart.addEventListener('mouseleave', () => { M.hoverS = null; syncMeasure(); C.schedulePaint(); });
    const clr = el('button', {title: 'clear the route (shift+click on the map does too)'}, 'clear');
    clr.addEventListener('click', () => { M.pts = []; syncMeasure(); C.paint(); });
    const strip = el('span', {class: 'mapopts', style: 'flex-direction:column;align-items:flex-start'}, el('span', {}, els.mInfo, ' ', clr), els.mChart);
    syncMeasure();
    return strip;
  }
  C.registerTool({
    key: 'measure', label: 'Measure', group: 'view', order: 10, locale: true,
    title: 'MEASURE a route: click points (drag one to move it, shift+click clears): distance, climb, grade and the elevation profile under the toolbar',
    opts: measureOpts,
    down(ev, fx, fz) {
      if (ev.shiftKey) { M.pts = []; syncMeasure(); C.paint(); return true; }
      const k = M.pts.findIndex(p => Math.hypot((p[0] - fx) * C.view.scale, (p[1] - fz) * C.view.scale) <= 7);
      if (k >= 0) M.drag = k;
      else { M.pts.push([Math.round(fx), Math.round(fz)]); M.drag = M.pts.length - 1; }
      syncMeasure(); C.paint();
      return true;
    },
    move(ev, fx, fz) { if (M.drag < 0) return; M.pts[M.drag] = [Math.round(fx), Math.round(fz)]; syncMeasure(); },
    up() { M.drag = -1; },
    onKey(ev) { if (ev.key === 'Escape' && M.pts.length) { M.pts = []; syncMeasure(); C.paint(); return true; } return false; },
    cursor(ctx, wx, wz) {
      if (!M.pts.length || M.drag >= 0) return;
      const p = M.pts[M.pts.length - 1], d = Math.hypot(wx - p[0], wz - p[1]), dh = C.heightAt(wx, wz) - C.heightAt(p[0], p[1]);
      ctx.strokeStyle = 'rgba(255,90,208,0.6)'; ctx.setLineDash([3, 3]); ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(C.SX(p[0]), C.SZ(p[1])); ctx.lineTo(C.SX(wx), C.SZ(wz)); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = '#ffd0f0'; ctx.font = '11px monospace';
      ctx.fillText(`+${C.fmtM(d / v())}  ${dh >= 0 ? '↑' : '↓'}${C.fmtM(Math.abs(dh) / v())}  ${d > 0 ? (Math.abs(dh) / d * 100).toFixed(0) : 0} %`, C.SX(wx) + 10, C.SZ(wz) - 8);
    },
    early, paint,
    guide: ['Measure — distance, climb and grade',
      'Measures a route over the height plane: its horizontal and 3D length, the total climb and descent, the steepest and the mean grade, and its elevation profile coloured by the game’s slope bands (green walkable, yellow steep, red cliff).',
      'Click points along the route; drag a point to move it; shift+click (or Esc, or clear) starts over. Hover the profile to find the spot on the map. The live segment to the cursor shows its length, rise and grade.',
      'Reads the AUTHORED height plane (6.4 m posts), not the engine’s noise or sculpt on top; the grade is measured post to post. Writes nothing.'],
  });
  /** The display bar's controls: the overlay, the grid, the minimap. */
  function displayControls() {
    const el = C.el;
    const ov = el('select', {title: 'an analysis overlay: SLOPE bands (walkable / steep / cliff, from the game’s limits), FLOW (where rivers would run: drainage area, ~51 m), LAKES (where water would stand: Priority-Flood depth)'},
      el('option', {value: 'none'}, 'no overlay'), el('option', {value: 'slope'}, 'slope bands'), el('option', {value: 'flow'}, 'flow'), el('option', {value: 'lakes'}, 'lakes'));
    ov.value = disp.overlay;
    ov.addEventListener('change', () => { disp.overlay = ov.value; disp.slopeBands = slopeBands(); C.imgCacheClear(); scheduleFlow(); C.paint(); });
    const chk = (key, label, title) => {
      const c = el('input', {type: 'checkbox', title});
      c.checked = disp[key];
      c.addEventListener('change', () => { disp[key] = c.checked; C.paint(); });
      return el('label', {title}, c, label);
    };
    return [ov, chk('grid', 'km grid', 'a kilometre grid over the map, labelled'), chk('minimap', 'minimap', 'the whole map in the corner, the view’s box on it; click it to jump there')];
  }
  disp.slopeBands = slopeBands();
  return {
    st, displayControls, slopeBands, sampleRoute, stats, syncMeasure,
    reset() { st.flow = null; st.mini = null; clearTimeout(st.flowTimer); st.flowTimer = 0; if (st.flowJob) st.flowJob.cancel(); st.flowJob = null; M.pts = []; M.drag = -1; disp.slopeBands = slopeBands(); syncMeasure(); },
    planeChanged() { st.mini = st.mini ? {...st.mini, at: 0} : null; },
  };
}
