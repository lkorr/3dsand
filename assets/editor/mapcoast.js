/* mapcoast.js — THE COAST TOOLS of the World map page (docs/PLAN_worldbuilder.md
 * E2 item 5): one brush, four modes, each acting only on the ground NEAR THE
 * SEA so the inland is never touched.
 *
 *   wobble  fractal displacement of the shoreline: every post within the
 *           height band round sea level takes the height of a point displaced
 *           by two-component fBm (amplitude, feature size), clamped to the
 *           band -- the coast frays into coves, spits and headlands while the
 *           land above the band and the sea floor below it stay put;
 *   beach   a BEACH BAND: land within `width` of the waterline is laid to a
 *           gentle ramp from the waterline up to `top` above the sea (fading
 *           back to the ground at the band's inner edge), the sea within a
 *           shelf off it to a shallow floor;
 *   cliff   a SEA CLIFF of a given height: land within `plateau` of the
 *           waterline is raised to at least sea + height (a plateau edge,
 *           easing back inland), the sea posts beside it are dropped to at
 *           least `drop` below the sea, so the coast becomes a wall;
 *   smooth  rounds the shoreline: the posts within the band are pulled toward
 *           their neighbourhood mean (a ~`smooth` m blur).
 *
 * Every mode computes a TARGET from the posts as they were BEFORE the stroke
 * (the stroke's undo tiles), and a post moves to orig + (target - orig) x the
 * largest weight any dab of the stroke gave it -- so a stroke is stable (a
 * second pass over the same spot does not compound) and one undo step. The
 * page's brush masks (maptiles.maskWeights: land / sea / height / slope)
 * multiply the weight. The pure part (coastTargets, coastDab, coastArea) is
 * Node-tested by scripts/test_coast.mjs; createCoastLayer is the page tool. */

import * as MT from './maptiles.js';
import * as TG from './terraingen.js';
import {distanceField} from './climate.js';

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
export const COAST_MODES = ['wobble', 'beach', 'cliff', 'smooth'];
/** The tool's settings, in METRES (the page converts with the map's voxels per metre). */
export const COAST_DEFAULTS = {mode: 'wobble', radiusM: 400, strength: 80, bandM: 20, ampM: 80, sizeM: 500, seed: 1,
                               widthM: 45, topM: 3, shelfM: 2, cliffM: 30, plateauM: 60, dropM: 8, smoothM: 40};
/** Metres -> the voxel options coastTargets reads. */
export function voxOpts(s, vpm, seaY) {
  const v = x => x * vpm;
  return {mode: s.mode, sea: seaY, band: v(s.bandM), amp: v(s.ampM), size: v(s.sizeM), seed: s.seed | 0, width: v(s.widthM), top: v(s.topM),
          shelf: v(s.shelfM), cliff: v(s.cliffM), plateau: v(s.plateauM), drop: v(s.dropM), smooth: v(s.smoothM), k: clamp(s.strength / 100, 0, 1)};
}

/** The posts of [i0, i0+w) x [j0, j0+h) as they were BEFORE the stroke whose
 *  undo copies are `before` (key -> tile | null). */
export function origWindow(P, i0, j0, w, h, before) {
  const out = MT.readWindow(P, i0, j0, w, h);
  if (!before || !before.size) return out;
  for (let j = 0; j < h; j++) {
    const jj = clamp(j0 + j, 0, P.posts[1] - 1);
    for (let i = 0; i < w; i++) {
      const ii = clamp(i0 + i, 0, P.posts[0] - 1), k = MT.tileKey(ii >> 8, jj >> 8);
      if (!before.has(k)) continue;
      const t = before.get(k);
      out[j * w + i] = t ? t[(jj & 255) * MT.TILE + (ii & 255)] : P.def;
    }
  }
  return out;
}
/** How far (voxels) each mode reads past its window. */
function padOf(P, o) {
  const posts = x => Math.ceil(x / P.sp) + 2;
  switch (o.mode) {
    case 'wobble': return posts(o.amp);
    case 'beach': return posts(Math.max(o.width, o.width * 0.6));
    case 'cliff': return posts(Math.max(o.plateau, 2 * P.sp));
    case 'smooth': return posts(3 * o.smooth);
    default: throw new Error('unknown coast mode ' + o.mode);
  }
}
const sstep = (a, b, x) => { if (b <= a) return x >= b ? 1 : 0; const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
/** The TARGET height of every post of the window (Float32Array w x h; NaN =
 *  this mode leaves the post alone), from the pre-stroke posts. o = voxOpts. */
export function coastTargets(P, i0, j0, w, h, o, before) {
  const pad = padOf(P, o), W = w + 2 * pad, Hh = h + 2 * pad, sp = P.sp, sea = o.sea;
  const O = origWindow(P, i0 - pad, j0 - pad, W, Hh, before);
  const out = new Float32Array(w * h).fill(NaN);
  const bandW = v => 1 - sstep(o.band * 0.6, o.band, Math.abs(v - sea));
  const at = (i, j) => O[(j + pad) * W + i + pad];
  if (o.mode === 'wobble') {
    const bil = (x, z) => {
      x = clamp(x, 0, W - 1); z = clamp(z, 0, Hh - 1);
      const a = Math.min(W - 2, Math.floor(x)), b = Math.min(Hh - 2, Math.floor(z)), u = x - a, v = z - b, n = b * W + a;
      const t = O[n] + (O[n + 1] - O[n]) * u, q = O[n + W] + (O[n + W + 1] - O[n + W]) * u;
      return t + (q - t) * v;
    };
    const size = Math.max(sp, o.size);
    for (let j = 0; j < h; j++)
      for (let i = 0; i < w; i++) {
        const v = at(i, j), bw = bandW(v);
        if (bw <= 0) continue;
        const x = P.min[0] + (i0 + i) * sp, z = P.min[1] + (j0 + j) * sp;
        const dx = o.amp * TG.fbm(x / size, z / size, o.seed + 101, 4), dz = o.amp * TG.fbm(x / size + 5.2, z / size + 1.3, o.seed + 211, 4);
        const d = clamp(bil(i + pad + dx / sp, j + pad + dz / sp), sea - o.band, sea + o.band);
        out[j * w + i] = v + (d - v) * bw;
      }
    return out;
  }
  if (o.mode === 'smooth') {
    const B = TG.blur(O, W, Hh, Math.max(1, Math.round(o.smooth / sp)), 2);
    for (let j = 0; j < h; j++)
      for (let i = 0; i < w; i++) {
        const v = at(i, j), bw = bandW(v);
        if (bw > 0) out[j * w + i] = v + (B[(j + pad) * W + i + pad] - v) * bw;
      }
    return out;
  }
  // beach / cliff: the distance (posts) to the waterline, each side
  const seaM = new Uint8Array(W * Hh), landM = new Uint8Array(W * Hh);
  for (let n = 0; n < O.length; n++) { if (O[n] < sea) seaM[n] = 1; else landM[n] = 1; }
  const dL = distanceField(seaM, W, Hh), dS = distanceField(landM, W, Hh);
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const m = (j + pad) * W + i + pad, v = O[m];
      let t = NaN;
      if (o.mode === 'beach') {
        if (v >= sea) {
          const d = (dL[m] - 0.5) * sp;                 // the waterline lies between posts
          if (!(d <= o.width) || v > sea + o.band) continue;
          const prof = sea + Math.max(2, o.top * clamp(d / Math.max(1, o.width), 0, 1));
          const back = sstep(0.65, 1, d / Math.max(1, o.width)), bw = 1 - sstep(o.band * 0.7, o.band, v - sea);
          t = v + ((prof + (v - prof) * back) - v) * bw;
        } else {
          const shelfW = o.width * 0.6, d = (dS[m] - 0.5) * sp;
          if (!(d <= shelfW) || v < sea - o.band) continue;
          const prof = sea - Math.max(2, o.shelf * clamp(d / Math.max(1, shelfW), 0, 1));
          t = prof + (v - prof) * sstep(0.6, 1, d / Math.max(1, shelfW));
        }
      } else {
        if (v >= sea) {
          const d = (dL[m] - 0.5) * sp;
          if (!(d <= o.plateau)) continue;
          const top = Math.max(v, sea + o.cliff);
          t = top + (v - top) * sstep(0.6, 1, d / Math.max(1, o.plateau));
        } else {
          const dropW = Math.max(2 * sp, o.plateau * 0.3), d = (dS[m] - 0.5) * sp;
          if (!(d <= dropW)) continue;
          const floor = Math.min(v, sea - o.drop);
          t = floor + (v - floor) * sstep(0.6, 1, d / dropW);
        }
      }
      out[j * w + i] = t;
    }
  return out;
}
/** Write `targets` into the window with per-post weights wt (0..1, the dab's
 *  falloff x strength x mask), STROKE-STABLE: a post takes
 *  orig + (target - orig) * max(weight so far). stroke = {wmax: Map} kept by
 *  the caller for the whole stroke; before = its undo tiles. */
function coastWrite(P, i0, j0, w, h, targets, wt, before, stroke) {
  const wmax = (stroke && stroke.wmax) || new Map();
  if (stroke && !stroke.wmax) stroke.wmax = wmax;
  const orig = origWindow(P, i0, j0, w, h, before);
  return MT.writeWindow(P, i0, j0, w, h, (n) => {
    const t = targets[n], k = wt[n];
    if (!(k > 0) || Number.isNaN(t)) return null;
    const i = i0 + n % w, j = j0 + Math.floor(n / w);
    if (i < 0 || j < 0 || i >= P.posts[0] || j >= P.posts[1]) return null;
    const tk = MT.tileKey(i >> 8, j >> 8), idx = (j & 255) * MT.TILE + (i & 255);
    let wm = wmax.get(tk);
    if (!wm) { wm = new Float32Array(MT.TILE * MT.TILE); wmax.set(tk, wm); }
    if (k <= wm[idx]) return null;
    wm[idx] = k;
    return orig[n] + (t - orig[n]) * k;
  }, before);
}
/** One dab of the coast brush at world (wx, wz), radius r (voxels), options o
 *  (voxOpts) + o.mask (a maptiles brush mask or null). Returns writeWindow's
 *  {changed, skipped}. */
export function coastDab(P, wx, wz, r, o, before, stroke) {
  r = Math.max(r, P.sp);
  const i0 = Math.floor((wx - r - P.min[0]) / P.sp), j0 = Math.floor((wz - r - P.min[1]) / P.sp);
  const i1 = Math.ceil((wx + r - P.min[0]) / P.sp), j1 = Math.ceil((wz + r - P.min[1]) / P.sp);
  const w = i1 - i0 + 1, h = j1 - j0 + 1, wt = new Float32Array(w * h);
  let any = false;
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const d = Math.hypot(P.min[0] + (i0 + i) * P.sp - wx, P.min[1] + (j0 + j) * P.sp - wz);
      const f = MT.falloff(d, r) * o.k;
      if (f > 0) { wt[j * w + i] = f; any = true; }
    }
  if (!any) return {changed: 0, skipped: 0};
  if (o.mask) { const mw = MT.maskWeights(P, i0, j0, w, h, o.mask); if (mw) for (let n = 0; n < wt.length; n++) wt[n] *= mw[n]; }
  const T = coastTargets(P, i0, j0, w, h, o, before);
  return coastWrite(P, i0, j0, w, h, T, wt, before, stroke);
}
/** The mode over a whole post rectangle [i0, j0, w, h] (the Area), the weight
 *  feathered over the rectangle's edge. */
export function coastArea(P, rect, o, before, stroke = {}) {
  const [i0, j0, w, h] = rect, fe = Math.max(2, Math.min(w, h) >> 4), wt = new Float32Array(w * h);
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const e = Math.min(i, j, w - 1 - i, h - 1 - j), t = clamp((e + 1) / fe, 0, 1);
      wt[j * w + i] = t * t * (3 - 2 * t) * o.k;
    }
  if (o.mask) { const mw = MT.maskWeights(P, i0, j0, w, h, o.mask); if (mw) for (let n = 0; n < wt.length; n++) wt[n] *= mw[n]; }
  const T = coastTargets(P, i0, j0, w, h, o, before);
  return coastWrite(P, i0, j0, w, h, T, wt, before, stroke);
}

/* ---- the page tool ---------------------------------------------------------------------- */
export function createCoastLayer(C) {
  const S = {...COAST_DEFAULTS, mask: 'none'};
  const els = {};
  let stroke = null, last = null;
  const v = () => C.vpm();
  const P = () => C.map().H;
  const opts = () => {
    const o = voxOpts(S, v(), C.seaY());
    o.mask = S.mask === 'none' ? null : {kind: S.mask, seaY: C.seaY(), soft: v() * 2};
    return o;
  };
  function dab(wx, wz) {
    const r = coastDab(P(), wx, wz, S.radiusM * v(), opts(), stroke.before, stroke);
    if (r.changed) stroke.changed = true;
    if (r.skipped && !r.changed) C.status('those tiles are still loading from disk — the brush skipped them; stroke again in a moment');
  }
  function down(ev, wx, wz) {
    if (!C.map()) return true;
    if (C.pending()) C.commitPending();
    stroke = {before: new Map(), wmax: new Map(), changed: false};
    last = [wx, wz];
    dab(wx, wz);
    C.paint();
    return true;
  }
  function move(ev, fx, fz) {
    if (!stroke) return;
    const gap = Math.max(P().sp * 0.5, S.radiusM * v() * 0.25);
    let [lx, lz] = last, d = Math.hypot(fx - lx, fz - lz);
    while (d >= gap) {
      lx += (fx - lx) * gap / d; lz += (fz - lz) * gap / d;
      dab(lx, lz);
      d = Math.hypot(fx - lx, fz - lz);
    }
    last = [lx, lz];
  }
  function up() {
    const st = stroke;
    stroke = null;
    if (!st || !st.changed) return;
    C.pushUndo({tiles: {height: st.before}});
    C.planeChanged(); C.markDirty(true);
  }
  function applyArea() {
    const a = C.area();
    if (!C.map()) return;
    if (!a) { C.toast('draw an Area first (the Area tool), or paint with the brush', true); return; }
    const rect = C.areaRect();
    if (C.rectBlocked(rect)) { C.toast('tiles under the area are still loading; try again in a moment', true); return; }
    if (C.pending()) C.commitPending();
    const before = new Map(), t0 = performance.now();
    const r = coastArea(P(), rect, opts(), before, {});
    if (r.changed) { C.pushUndo({tiles: {height: before}}); C.planeChanged(); C.markDirty(true); }
    C.paint();
    C.status(`coast ${S.mode} over the Area: ${r.changed} posts moved in ${((performance.now() - t0) / 1000).toFixed(1)} s — Ctrl+Z takes it back`);
    return r;
  }
  function optsStrip() {
    const el = C.el;
    const num = (key, min, max, step, w, title) => {
      const i = el('input', {type: 'number', min, max, step, value: S[key], style: `width:${w}px`, title});
      i.addEventListener('change', () => { S[key] = clamp(+i.value || 0, min, max); i.value = S[key]; });
      els['n_' + key] = i;
      return i;
    };
    const mode = el('select', {title: 'WOBBLE frays the coastline (fractal displacement), BEACH lays a beach band, CLIFF builds a sea cliff, SMOOTH rounds the shoreline'},
                   ...COAST_MODES.map(m => el('option', {value: m}, m)));
    mode.value = S.mode;
    mode.addEventListener('change', () => { S.mode = mode.value; sync(); });
    els.mode = mode;
    const mask = el('select', {title: 'MASK: limit the brush to the land or the sea side'},
                    el('option', {value: 'none'}, 'no mask'), el('option', {value: 'land'}, 'land only'), el('option', {value: 'sea'}, 'sea only'));
    mask.value = S.mask;
    mask.addEventListener('change', () => { S.mask = mask.value; });
    els.wobble = el('span', {}, ' amp ', num('ampM', 1, 2000, 5, 52, 'how far the shoreline is displaced, m'), 'm size ', num('sizeM', 10, 20000, 10, 56, 'the largest cove / headland, m'), 'm seed ', num('seed', 0, 99999, 1, 52, 'the wobble’s seed'));
    els.beach = el('span', {}, ' width ', num('widthM', 5, 1000, 5, 52, 'the beach band’s width inland from the waterline, m'), 'm top ', num('topM', 0.2, 30, 0.5, 44, 'the beach’s height at its inner edge, m above sea'),
                    'm shelf ', num('shelfM', 0.2, 30, 0.5, 44, 'the depth of the shallow shelf off the beach, m'), 'm');
    els.cliff = el('span', {}, ' height ', num('cliffM', 2, 500, 1, 48, 'the cliff’s height above sea, m'), 'm plateau ', num('plateauM', 10, 2000, 5, 52, 'how far inland the cliff-top plateau reaches before easing back to the ground, m'),
                   'm drop ', num('dropM', 0, 200, 1, 44, 'how deep the sea is at the cliff’s foot, m'), 'm');
    els.smooth = el('span', {}, ' blur ', num('smoothM', 5, 1000, 5, 48, 'how far the shoreline is smoothed, m'), 'm');
    const go = el('button', {title: 'apply the mode over the whole Area (draw one with the Area tool); one undo step'}, 'apply to Area');
    go.addEventListener('click', applyArea);
    els.strip = el('span', {class: 'mapopts'}, 'coast ', mode, ' r ', num('radiusM', 5, 5000, 10, 56, 'brush radius, m ([ and ])'), 'm str ', num('strength', 1, 100, 5, 44, 'strength %'),
                   '% band ', num('bandM', 1, 500, 1, 44, 'only ground within this height of sea level is touched, m (inland stays put)'), 'm',
                   els.wobble, els.beach, els.cliff, els.smooth, ' ', mask, ' ', go);
    sync();
    return els.strip;
  }
  function sync() {
    if (!els.strip) return;
    for (const m of COAST_MODES) els[m].style.display = m === S.mode ? '' : 'none';
    if (els.mode) els.mode.value = S.mode;
    if (els.n_radiusM) els.n_radiusM.value = S.radiusM;
  }
  C.registerTool({
    key: 'coast', label: 'Coast', group: 'height', order: 40,
    title: 'COAST tools: WOBBLE frays the coastline, BEACH lays a beach band, CLIFF builds a sea cliff, SMOOTH rounds the shore; only ground within the band round sea level moves; [ ] radius; "apply to Area" does the whole Area',
    opts: optsStrip, down, move, up,
    cursor(ctx, wx, wz) {
      const r = Math.max(S.radiusM * v(), P().sp), sx = C.SX(wx), sz = C.SZ(wz);
      ctx.strokeStyle = 'rgba(120,220,255,0.9)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.arc(sx, sz, Math.max(2, r * C.view.scale), 0, Math.PI * 2); ctx.stroke();
      ctx.fillStyle = '#9fe6ff'; ctx.font = '11px monospace';
      ctx.fillText(`coast ${S.mode} · band ±${C.fmtM(S.bandM)}`, sx + 10, sz - 10);
    },
    resize(f) { S.radiusM = clamp(Math.round(S.radiusM * f), 5, 5000); sync(); },
    sync,
    guide: ['Coast — wobble, beach, cliff, smooth',
      'Shapes the COASTLINE without touching the inland: only ground within the height BAND round sea level moves (cliff: the land within its plateau of the waterline). WOBBLE frays the shore into coves, spits and headlands (a fractal displacement of the heights near the sea); BEACH lays a beach band — a gentle ramp from the waterline up to "top" over "width", easing back to the ground, and a shallow shelf off it; CLIFF raises the land by the water to a plateau at "height" and drops the sea at its foot, so the coast becomes a wall; SMOOTH rounds the shoreline.',
      'Pick the mode and its numbers in the toolbar, left-drag along the coast ([ ] radius). A stroke is stable: going over the same spot again does not compound it; each stroke is one undo step. "apply to Area" runs the mode over the whole Area (the Area tool) at once. The mask keeps it to the land or the sea side.',
      'Works on the 6.4 m height plane: a beach narrower than ~2 posts (13 m) or a cliff under a few metres hardly shows; use Sculpt for finer coast work. The game builds the sand / rock skin from the biome and the slope, not from the mode.'],
  });
  return {S, applyArea, sync, reset() { stroke = null; }};
}
