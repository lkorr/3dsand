/* mapvector.js — the World map page's VECTOR LAYERS (docs/PLAN_map_v2.md E3):
 * ROADS, REGIONS and MARKERS, drawn and edited over the planes. map.js owns
 * the page (the canvas, the view, undo, save); this module owns the three
 * map.json arrays and is handed a context to reach the rest.
 *
 *   roads[]    graded ground the ENGINE builds (DESIGN.md "map v2 R1 ... ROADS
 *              ARE GRADED GROUND"): {id, name?, kind: road|track|path, points:
 *              [[x, z], ...] world voxels, width, shoulder (vox), surface (a
 *              SOLID material), maxGrade (%), depth (surface cells)}. The engine
 *              bakes the profile at load; the inspector's chart is THAT bake as
 *              --mapcheck reports it for the SAVED files (`roads[].profile`),
 *              never a JS re-implementation -- an edit since the save says so.
 *   regions[]  named polygons, data only: {id, name, colour "#rrggbb", points}.
 *              The engine validates them and answers RegionAt(x, z).
 *   markers[]  map annotations: {id, name, icon, at: [x, z]}; ICONS below is
 *              the art (pixel art, like all of the UI). LOCALE markers (L1's
 *              locales[]) are the locale editor's (E4), not these.
 *
 * Editing, the same gestures on all three:
 *   Road / Region tool: click on open map (or Ctrl+click anywhere, e.g. on a
 *     junction vertex to branch a road off it) starts a new one and each click adds
 *     a vertex (double-click, Enter or Esc ends; clicking the last vertex ends
 *     a road, clicking the first one closes a region); drag a vertex to move
 *     it; click a segment of the SELECTED one to insert a vertex there; click
 *     another one to select it; shift+click a vertex deletes it (and the whole
 *     road / region when too few are left for the engine to accept).
 *   JUNCTIONS: a vertex dropped within a few pixels of another road's vertex
 *     SNAPS onto it; dropped on another road's segment it snaps onto the
 *     segment and that road gets a vertex there too (a T junction). Dragging a
 *     junction vertex moves every road's vertex on it (Alt on the press drags
 *     it out alone; Alt on the release does not snap). Regions snap to other
 *     regions' vertices and edges the same way (shared borders).
 *   Marker tool: click places a marker with the picked icon; drag one to move
 *     it; shift+click deletes it.
 *
 * BENDS (docs/PLAN_worldbuilder.md 2.3, E2): a road with `smooth: true` is the
 * engine's spline through its points (spline.js smoothRoadPoints = the
 * engine's SmoothRoadPoints: centripetal Catmull-Rom at 16 voxels, rounded,
 * Douglas-Peucker 0.75); the page draws, hit-tests, snaps onto and profiles
 * THAT centreline. New roads are smooth; an existing polyline road stays
 * `smooth: false` until its inspector's "bend" box is ticked. Clicking the
 * selected road's curve inserts a point there and drags it (drag the curve).
 *
 * Every edit is one map.json undo step (map.js snapshot / restore). Saving is
 * refused while a road has fewer than two distinct points or a region fewer
 * than three -- the engine would refuse the whole map. */

import * as SPL from './spline.js';

/* ---- pure helpers (Node-testable: scripts/test_mapvector.mjs) ---------------- */

/** Road defaults by kind, the engine's (worldmap.cpp, DESIGN.md R1). */
export const ROAD_KINDS = {
  road:  {width: 40, shoulder: 16, maxGrade: 10},
  track: {width: 24, shoulder: 12, maxGrade: 15},
  path:  {width: 12, shoulder: 6, maxGrade: 25},
};
export const ROAD_DEPTH_DEFAULT = 3;
/** The loader's clamps (outside them is a load warning). */
export const ROAD_LIMITS = {width: [2, 256], shoulder: [0, 64], maxGrade: [1, 100], depth: [1, 8]};
/** Surfaces a road is offered by default, when the materials list is not at hand. */
export const ROAD_SURFACES = ['packed_gravel', 'packed_earth', 'cobble', 'flagstone', 'stone', 'sandstone'];

/** THE MARKER ICONS: 12 x 12 pixel art, one char per pixel ('.' clear). Must
 *  match src/sim/worldmap.h kMarkerIcons (the loader warns on any other). */
export const ICON_PALETTE = {
  k: '#1c1712', w: '#cdb98e', r: '#a8432e', y: '#e2c25a', b: '#7a5232', g: '#8d939a',
  d: '#2a2c34', l: '#e8e4d8', u: '#5a9ad8', o: '#ff8a3a', G: '#5a9a3a',
};
export const ICONS = {
  hut: [
    '............', '.....kk.....', '....kyyk....', '...kyyyyk...', '..kyyyyyyk..', '.kyyyyyyyyk.',
    'kkkkkkkkkkkk', '.kwwwwwwwwk.', '.kwwwbbwwwk.', '.kwwwbbwwwk.', '.kwwwbbwwwk.', '.kkkkkkkkkk.'],
  village: [
    '............', '............', '...k........', '..krk...k...', '.krrrk.krk..', 'krrrrrkrrrk.',
    '.kwwwk.kwk..', '.kwbwk.kwk..', '.kwbwk.kbk..', 'kkkkkkkkkkkk', '............', '............'],
  castle: [
    '............', 'k.k......k.k', 'kgk......kgk', 'kgk......kgk', 'kgkk.kk.kkgk', 'kgkkkkkkkkgk',
    'kggggggggggk', 'kgdggggggdgk', 'kggggkkggggk', 'kgggkbbkgggk', 'kgggkbbkgggk', 'kkkkkkkkkkkk'],
  tower: [
    '..kk.kk.kk..', '..kggggggk..', '..kkkkkkkk..', '...kggggk...', '...kgddgk...', '...kgddgk...',
    '...kggggk...', '...kggggk...', '...kggggk...', '...kgbbgk...', '...kgbbgk...', '..kkkkkkkk..'],
  cave: [
    '............', '............', '....kkkk....', '...kggggk...', '..kggggggk..', '.kgggkkgggk.',
    '.kggkddkggk.', 'kgggkddkgggk', 'kggkddddkggk', 'kggkddddkggk', 'kggkddddkggk', 'kkkkkkkkkkkk'],
  ruin: [
    '............', 'kkkkkkk.....', '.kgggk......', '.kgggk......', '.kgggk.k..k.', '.kgggk.kgkgk',
    '.kgggk.kgggk', '.kgggk.kgggk', '.kgggk.kgggk', 'kkkkkkkkkkkk', 'kggk.kgggggk', 'kkkk.kkkkkkk'],
  shrine: [
    '............', '.....kk.....', '....kyyk....', '...kkkkkk...', '..kllllllk..', '.kkkkkkkkkk.',
    '..klk..klk..', '..klkyyklk..', '..klk..klk..', '..klk..klk..', '.kkkkkkkkkk.', 'kllllllllllk'],
  camp: [
    '............', '............', '....k.......', '...kwk......', '...kwk......', '..kwwwk.....',
    '..kwdwk.....', '.kwwdwwk....', '.kwdddwk..o.', 'kwwdddwwkoyo', 'kkkkkkkkkbbb', '............'],
  mine: [
    '............', '............', '.kkkkkkkkkk.', '.kbbbbbbbbk.', '.kkkkkkkkkk.', '..kbddddbk..',
    '..kbddddbk..', '..kbddddbk..', '..kbddddbk..', '..kbddddbk..', 'kkkkkkkkkkkk', 'kggggggggggk'],
  port: [
    '.....ll.....', '....l..l....', '.....ll.....', '...llllll...', '.....ll.....', '.....ll.....',
    '.....ll.....', 'l....ll....l', 'll...ll...ll', '.ll..ll..ll.', '..llllllll..', '....llll....'],
};
export const ICON_NAMES = Object.keys(ICONS);
export const ICON_SIZE = 12;

/** Region fill colours a new region cycles through. */
export const REGION_COLOURS = ['#c9a227', '#4f9d69', '#5b7fc4', '#b5546a', '#8a6fc0', '#3fa3a8', '#c07a3a', '#7d8c3a'];

/** Distance from (x, z) to segment a-b and the projection parameter t (0..1). */
export function segDist(a, b, x, z) {
  const ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
  const t = L2 > 0 ? Math.max(0, Math.min(1, ((x - a[0]) * ex + (z - a[1]) * ez) / L2)) : 0;
  return {d: Math.hypot(x - a[0] - ex * t, z - a[1] - ez * t), t, x: a[0] + ex * t, z: a[1] + ez * t};
}
/** Even-odd point-in-polygon at the column CENTRE, the engine's RegionAt rule
 *  (worldmap.cpp WorldMapData::RegionAt) in the same doubled integer form. */
export function inPolygon(pts, x, z) {
  const px = 2 * x + 1, pz = 2 * z + 1;
  let inside = false;
  for (let i = 0, k = pts.length - 1; i < pts.length; k = i++) {
    const ax = 2 * pts[k][0], az = 2 * pts[k][1], bx = 2 * pts[i][0], bz = 2 * pts[i][1];
    if ((az > pz) === (bz > pz)) continue;
    const lhs = (pz - az) * (bx - ax), rhs = (px - ax) * (bz - az);
    if (bz > az ? lhs > rhs : lhs < rhs) inside = !inside;
  }
  return inside;
}
/** The last region (table order) holding column (x, z), or null: RegionAt. */
export function regionAt(regions, x, z) {
  for (let i = (regions || []).length - 1; i >= 0; i--) if (inPolygon(regions[i].points || [], x, z)) return regions[i];
  return null;
}
/** Signed shoelace area, voxels². */
export function polygonArea(pts) {
  let a = 0;
  for (let i = 0, k = pts.length - 1; i < pts.length; k = i++) a += pts[k][0] * pts[i][1] - pts[i][0] * pts[k][1];
  return a / 2;
}
/** A label point: the area centroid, or the vertex mean for a degenerate polygon. */
export function polygonLabelAt(pts) {
  const A = polygonArea(pts);
  if (Math.abs(A) < 1) {
    let x = 0, z = 0;
    for (const p of pts) { x += p[0]; z += p[1]; }
    return [x / Math.max(1, pts.length), z / Math.max(1, pts.length)];
  }
  let cx = 0, cz = 0;
  for (let i = 0, k = pts.length - 1; i < pts.length; k = i++) {
    const f = pts[k][0] * pts[i][1] - pts[i][0] * pts[k][1];
    cx += (pts[k][0] + pts[i][0]) * f; cz += (pts[k][1] + pts[i][1]) * f;
  }
  return [cx / (6 * A), cz / (6 * A)];
}
/** Distinct consecutive points, as the loader counts them (a closed region's
 *  repeated first point dropped too). */
export function distinctPoints(pts, closed) {
  const out = [];
  for (const p of pts || []) if (!out.length || out[out.length - 1][0] !== p[0] || out[out.length - 1][1] !== p[1]) out.push(p);
  if (closed && out.length > 1 && out[0][0] === out[out.length - 1][0] && out[0][1] === out[out.length - 1][1]) out.pop();
  return out.length;
}
/** Arc length at each vertex of a polyline (voxels). */
export function vertexArcs(pts) {
  const s = [0];
  for (let i = 1; i < pts.length; i++) s.push(s[i - 1] + Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
  return s;
}
/** The world point at arc length s along a polyline. */
export function pointAtArc(pts, s) {
  if (!pts.length) return null;
  let acc = 0;
  for (let i = 1; i < pts.length; i++) {
    const L = Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
    if (acc + L >= s || i === pts.length - 1) {
      const t = L > 0 ? Math.max(0, Math.min(1, (s - acc) / L)) : 0;
      return [pts[i - 1][0] + (pts[i][0] - pts[i - 1][0]) * t, pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * t];
    }
    acc += L;
  }
  return pts[0].slice();
}
const clCache = new Map();
/** A road's CENTRELINE as the engine builds it: `smooth: true` -> the spline
 *  (spline.smoothRoadPoints, the engine's SmoothRoadPoints), else its points. */
export function roadCentreline(r) {
  const P = (r && r.points) || [];
  if (!r || r.smooth !== true || distinctPoints(P, false) < 2) return P;
  const key = JSON.stringify(P);
  let c = clCache.get(key);
  if (!c) { c = SPL.smoothRoadPoints(P); if (clCache.size > 200) clCache.clear(); clCache.set(key, c); }
  return c;
}
/** Arc length along the road's centreline of each of its own points. */
export function knotArcs(r) {
  const C = roadCentreline(r), A = vertexArcs(C);
  return ((r && r.points) || []).map(p => {
    let best = Infinity, s = 0;
    for (let i = 0; i + 1 < C.length; i++) {
      const q = segDist(C[i], C[i + 1], p[0], p[1]);
      if (q.d < best) { best = q.d; s = A[i] + (A[i + 1] - A[i]) * q.t; }
    }
    return s;
  });
}
/** Where on a road's centreline (x, z) is nearest, as an INSERT position into
 *  its points: {k (insert after point k), x, z, d, t (0..1 between k and k+1,
 *  0 or 1 = on a point)}. A polyline road is its segments; a smooth one its curve. */
export function roadCurveHit(r, x, z) {
  const P = (r && r.points) || [];
  if (P.length < 2) return null;
  if (r.smooth !== true) {
    let best = null;
    for (let k = 0; k + 1 < P.length; k++) { const q = segDist(P[k], P[k + 1], x, z); if (!best || q.d < best.d) best = {k, ...q}; }
    return best;
  }
  const t = SPL.tessellate(P.map(p => [p[0], p[1]]), {step: 16, smooth: true});
  const c = SPL.closest(t, x, z);
  if (!c) return null;
  const {map} = SPL.dedupe(P);
  const k = map[Math.min(c.seg, map.length - 1)];
  const f = c.t - c.seg;
  return {k, x: c.x, z: c.z, d: c.dist, t: f};
}
/** What is wrong with the three arrays for the engine's loader, or null:
 *  the refusals only (an id missing or twice, too few points, a marker
 *  without `at`). The page refuses to save over one of these. */
export function vectorProblems(json) {
  const out = [];
  const ids = (arr, what) => {
    const seen = new Set();
    for (const o of arr || []) {
      if (!o || typeof o.id !== 'string' || !o.id) { out.push(`a ${what} has no id`); continue; }
      if (seen.has(o.id)) out.push(`two ${what}s are called "${o.id}"`);
      seen.add(o.id);
    }
  };
  ids(json.roads, 'road'); ids(json.regions, 'region'); ids(json.markers, 'marker');
  for (const r of json.roads || []) if (distinctPoints(r.points, false) < 2) out.push(`road "${r.id}" needs two distinct points`);
  for (const r of json.regions || []) if (distinctPoints(r.points, true) < 3) out.push(`region "${r.id}" needs three distinct points`);
  for (const m of json.markers || []) if (!Array.isArray(m.at) || m.at.length !== 2) out.push(`marker "${m.id}" has no at`);
  return out.length ? out : null;
}
/** A fresh id `<base>_<n>` not used in arr. */
export function freshId(arr, base) {
  let n = (arr || []).length, id = base + '_' + n;
  while ((arr || []).some(o => o.id === id)) id = base + '_' + (++n);
  return id;
}

/* ---- the layer ---------------------------------------------------------------------- */
const iconCache = new Map();
/** The 12 x 12 canvas of an icon (an unknown name: a plain pin). */
export function iconCanvas(name) {
  if (iconCache.has(name)) return iconCache.get(name);
  const rows = ICONS[name] || ['............', '....kkkk....', '...kyyyyk...', '...kyyyyk...', '...kyyyyk...', '....kyyk....',
                               '.....kk.....', '.....kk.....', '.....kk.....', '.....kk.....', '............', '............'];
  const c = document.createElement('canvas');
  c.width = ICON_SIZE; c.height = ICON_SIZE;
  const g = c.getContext('2d');
  rows.forEach((row, j) => [...row].forEach((ch, i) => {
    if (ch === '.') return;
    g.fillStyle = ICON_PALETTE[ch] || '#ff00ff';
    g.fillRect(i, j, 1, 1);
  }));
  iconCache.set(name, c);
  return c;
}

/**
 * The vector layer of one map page. `C` is the page:
 *   map()            the loaded {json, ...} or null
 *   view, SX(x), SZ(z), worldAtF(ev), fitBox(x0, z0, x1, z1) (centre on a box,
 *                    zooming only when it would be tiny or not fit)
 *   pushUndo(e), snapshot(), markDirty(), paint(), status(t), toast(t, bad), el(...)
 *   vpm(), seaY(), check(), savedJson() (the map.json as last loaded / saved)
 *   tool(), setTool(t), hover() ([wx, wz] under the cursor or null), materials()
 */
export function createVectorLayer(C) {
  const st = {
    show: {roads: true, regions: true, markers: true},
    sel: null,            // {layer: 'road' | 'region' | 'marker', id, k: selected vertex or -1}
    drawing: false,       // clicks append to the selected road / region
    drag: null,           // {obj, layer, k, links: [[obj, k]], snap, moved, alt}
    newRoad: {kind: 'path', surface: 'packed_gravel'},
    newIcon: 'hut',
    probe: null,          // [x, z]: the profile chart's hover, drawn on the map
    lastSnap: null,       // what the last placement snapped to (tests, status)
  };
  const els = {};
  const grab = 7, segGrab = 5, snapPx = 8;   // screen pixels
  const J = () => C.map().json;
  const arr = layer => {
    const j = J(), k = layer + 's';
    if (!Array.isArray(j[k])) j[k] = [];
    return j[k];
  };
  const find = (layer, id) => (C.map() ? (J()[layer + 's'] || []).find(o => o.id === id) || null : null);
  const selObj = () => (st.sel ? find(st.sel.layer, st.sel.id) : null);
  const px = d => d * C.view.scale;
  const fmtM = m => (Math.abs(m) >= 1000 ? (m / 1000).toFixed(2) + ' km' : (Math.abs(m) >= 100 ? m.toFixed(0) : m.toFixed(1)) + ' m');
  const mOf = v => v / C.vpm();
  function edit(fn) { C.pushUndo(C.snapshot()); fn(); C.markDirty(true); sync(); C.paint(); }
  function select(layer, id, k = -1) { st.sel = id == null ? null : {layer, id, k}; sync(); }

  /* ---- surfaces: the solid MINERAL materials (a road on a grade must be solid) ---- */
  function surfaces() {
    const mats = (C.materials && C.materials()) || null;
    const ids = mats && mats.length
      ? mats.filter(m => m.class === 'solid' && (m.tags || []).includes('mineral')).map(m => m.id)
      : ROAD_SURFACES.slice();
    return ids.length ? ids : ROAD_SURFACES.slice();
  }
  function surfaceColour(name) {
    const mats = (C.materials && C.materials()) || [];
    const m = mats.find(x => x.id === name);
    const c = m && Array.isArray(m.colors) && m.colors[0];
    if (typeof c === 'string') return c[0] === '#' ? c : '#' + c;
    if (Array.isArray(c) && c.length >= 3) return `rgb(${c[0]},${c[1]},${c[2]})`;
    return name === 'packed_earth' ? '#8a6a48' : name === 'cobble' ? '#8d8d90' : '#b8a888';
  }

  /* ---- hit tests (screen-pixel reaches) ---------------------------------------- */
  function vertexHit(layer, wx, wz, first) {
    const list = J()[layer + 's'] || [];
    const order = first ? [first, ...list.filter(o => o !== first)] : list;
    for (const o of order) {
      const pts = o.points || [];
      for (let k = 0; k < pts.length; k++) if (px(Math.hypot(pts[k][0] - wx, pts[k][1] - wz)) <= grab) return {obj: o, k};
    }
    return null;
  }
  function segmentHit(obj, wx, wz, closed) {
    const pts = obj.points || [];
    if (!closed && obj.smooth === true) {
      const h = roadCurveHit(obj, wx, wz);
      return h && px(h.d) <= segGrab ? h : null;
    }
    const n = closed ? pts.length : pts.length - 1;
    for (let k = 0; k < n; k++) {
      const s = segDist(pts[k], pts[(k + 1) % pts.length], wx, wz);
      if (px(s.d) <= segGrab) return {k, ...s};
    }
    return null;
  }
  /** Where a vertex dropped at (wx, wz) lands: another road's (region's)
   *  vertex, else a point on another one's segment (with the junction it makes
   *  there), else where it was dropped. `self` is never snapped to. */
  function snapAt(layer, wx, wz, self) {
    let best = null, bd = snapPx;
    for (const o of J()[layer + 's'] || []) {
      if (o === self) continue;
      for (const p of o.points || []) {
        const d = px(Math.hypot(p[0] - wx, p[1] - wz));
        if (d <= bd) { bd = d; best = {x: p[0], z: p[1], vertexOf: o.id}; }
      }
    }
    if (best) return best;
    bd = segGrab + 1;
    for (const o of J()[layer + 's'] || []) {
      if (o === self) continue;
      const pts = o.points || [], closed = layer === 'region', n = closed ? pts.length : pts.length - 1;
      if (!closed && o.smooth === true) {
        const h = roadCurveHit(o, wx, wz);
        const d = h ? px(h.d) : Infinity;
        if (h && d <= bd && h.t > 0.02 && h.t < 0.98) { bd = d; best = {x: Math.round(h.x), z: Math.round(h.z), segOf: o, seg: h.k}; }
        continue;
      }
      for (let k = 0; k < n; k++) {
        const s = segDist(pts[k], pts[(k + 1) % pts.length], wx, wz);
        const d = px(s.d);
        if (d <= bd && s.t > 0 && s.t < 1) { bd = d; best = {x: Math.round(s.x), z: Math.round(s.z), segOf: o, seg: k}; }
      }
    }
    return best || {x: Math.round(wx), z: Math.round(wz)};
  }
  /** Apply a snap: a segment snap gives the OTHER line a vertex there too. */
  function landSnap(s) {
    if (s.segOf) {
      const pts = s.segOf.points, at = s.seg + 1;
      if (!pts.some(p => p[0] === s.x && p[1] === s.z)) pts.splice(at, 0, [s.x, s.z]);
    }
    st.lastSnap = s.vertexOf ? {vertexOf: s.vertexOf} : s.segOf ? {segOf: s.segOf.id} : null;
    return [s.x, s.z];
  }
  /** Every OTHER line's vertex at exactly (x, z): a junction drags together. */
  function linksAt(layer, x, z, self) {
    const out = [];
    for (const o of J()[layer + 's'] || []) {
      if (o === self) continue;
      (o.points || []).forEach((p, k) => { if (p[0] === x && p[1] === z) out.push([o, k]); });
    }
    return out;
  }
  /** How many roads have a vertex at (x, z). */
  function junctionCount(x, z) {
    let n = 0;
    for (const r of J().roads || []) if ((r.points || []).some(p => p[0] === x && p[1] === z)) n++;
    return n;
  }

  /* ---- the line tools (road + region share them) ---------------------------------- */
  function newObject(layer, x, z) {
    if (layer === 'road') {
      const d = ROAD_KINDS[st.newRoad.kind] || ROAD_KINDS.path;
      return {id: freshId(J().roads, st.newRoad.kind), kind: st.newRoad.kind, surface: st.newRoad.surface,
              width: d.width, shoulder: d.shoulder, maxGrade: d.maxGrade, depth: ROAD_DEPTH_DEFAULT, smooth: true, points: [[x, z]]};
    }
    const regs = J().regions || [];
    return {id: freshId(regs, 'region'), name: 'Region ' + (regs.length + 1),
            colour: REGION_COLOURS[regs.length % REGION_COLOURS.length], points: [[x, z]]};
  }
  /** End the drawing: a road / region the engine would refuse is taken out. */
  function finish() {
    if (!st.drawing) return;
    st.drawing = false;
    const o = selObj();
    if (o) {
      const closed = st.sel.layer === 'region', need = closed ? 3 : 2;
      if (distinctPoints(o.points, closed) < need) {
        const a = arr(st.sel.layer);
        a.splice(a.indexOf(o), 1);
        st.sel = null;
        C.status(`a ${closed ? 'region needs three' : 'road needs two'} distinct points; the unfinished one was dropped`);
      }
    }
    sync(); C.paint();
  }
  function lineDown(layer, ev) {
    const [fx, fz] = C.worldAtF(ev), wx = Math.round(fx), wz = Math.round(fz);
    const closed = layer === 'region', cur = selObj() && st.sel.layer === layer ? selObj() : null;
    if (!cur) st.drawing = false;
    // 1. DRAWING: the click is the next vertex, snapped onto another line's
    //    vertex or segment (that is how a junction is drawn) -- except on its
    //    own last vertex (a road) / first vertex (a region), which ends it.
    if (st.drawing) {
      const own = vertexHit(layer, fx, fz, cur);
      if (own && own.obj === cur && cur.points.length > 1 &&
          ((!closed && own.k === cur.points.length - 1) || (closed && own.k === 0 && cur.points.length >= 3))) { finish(); return; }
      const s = ev.altKey ? {x: wx, z: wz} : snapAt(layer, fx, fz, cur);
      edit(() => { cur.points.push(landSnap(s)); st.sel.k = cur.points.length - 1; });
      return;
    }
    // Ctrl+click: a NEW one here even on top of another (a branch off a
    // junction vertex, a region inside a region).
    if (ev.ctrlKey || ev.metaKey) return startNew(layer, ev, fx, fz, wx, wz);
    // 2. a vertex (the selected line's first): drag it (a junction's partners
    //    come along unless Alt); shift+click deletes it
    const vh = vertexHit(layer, fx, fz, cur);
    if (vh) {
      const {obj, k} = vh;
      if (ev.shiftKey) {
        edit(() => {
          obj.points.splice(k, 1);
          if (distinctPoints(obj.points, closed) < (closed ? 3 : 2)) {
            const a = arr(layer); a.splice(a.indexOf(obj), 1);
            if (st.sel && st.sel.id === obj.id) st.sel = null;
            C.status(`${layer} "${obj.id}" had too few points left and was deleted`);
          } else st.sel = {layer, id: obj.id, k: -1};
        });
        return;
      }
      st.sel = {layer, id: obj.id, k};
      const p = obj.points[k];
      st.drag = {obj, layer, k, links: ev.altKey ? [] : linksAt(layer, p[0], p[1], obj), snap: C.snapshot(), moved: false};
      sync();
      return;
    }
    // 3. on a segment of the selected one: insert a vertex there and drag it
    if (cur) {
      const sh = segmentHit(cur, fx, fz, closed);
      if (sh) {
        const snap = C.snapshot();
        cur.points.splice(sh.k + 1, 0, [Math.round(sh.x), Math.round(sh.z)]);
        st.sel.k = sh.k + 1;
        st.drag = {obj: cur, layer, k: sh.k + 1, links: [], snap, moved: true};
        C.markDirty(true); sync();
        return;
      }
    }
    // 4. another one: select it (a region also by clicking inside it)
    for (const o of J()[layer + 's'] || []) {
      if (segmentHit(o, fx, fz, closed)) { st.drawing = false; select(layer, o.id); return; }
    }
    if (closed) {
      const r = regionAt(J().regions, wx, wz);
      if (r && r !== cur) { st.drawing = false; select(layer, r.id); return; }
      if (r && r === cur) { C.status('click an edge of the selected region to add a vertex, a vertex to drag it; Ctrl+click inside it starts a new region'); return; }
    }
    // 5. open map: a new one, drawing
    startNew(layer, ev, fx, fz, wx, wz);
  }
  function startNew(layer, ev, fx, fz, wx, wz) {
    const closed = layer === 'region';
    const s = ev.altKey ? {x: wx, z: wz} : snapAt(layer, fx, fz, null);
    edit(() => {
      const p = landSnap(s);
      const o = newObject(layer, p[0], p[1]);
      arr(layer).push(o);
      st.sel = {layer, id: o.id, k: 0};
      st.drawing = true;
    });
    C.status(`new ${layer}: click to add vertices; ${closed ? 'click the first vertex, ' : 'click the last vertex, '}double-click, Enter or Esc ends it`);
  }
  function markerDown(ev) {
    const [fx, fz] = C.worldAtF(ev), wx = Math.round(fx), wz = Math.round(fz);
    const ms = J().markers || [];
    let hit = null;
    for (let i = ms.length - 1; i >= 0 && !hit; i--) {
      const m = ms[i];
      if (!Array.isArray(m.at)) continue;
      // the drawn badge: 28 px wide, standing on its pin (30 px up to 4 below)
      const dx = C.SX(fx) - C.SX(m.at[0]), dz = C.SZ(fz) - C.SZ(m.at[1]);
      if (Math.abs(dx) <= 14 && dz >= -31 && dz <= 4) hit = m;
    }
    if (hit && ev.shiftKey) {
      edit(() => { ms.splice(ms.indexOf(hit), 1); if (st.sel && st.sel.id === hit.id) st.sel = null; });
      return;
    }
    if (hit) {
      st.sel = {layer: 'marker', id: hit.id, k: -1};
      st.drag = {obj: hit, layer: 'marker', k: -1, links: [], snap: C.snapshot(), moved: false, dx: hit.at[0] - wx, dz: hit.at[1] - wz};
      sync();
      return;
    }
    edit(() => {
      const m = {id: freshId(ms, st.newIcon), name: '', icon: st.newIcon, at: [wx, wz]};
      arr('marker').push(m);
      st.sel = {layer: 'marker', id: m.id, k: -1};
    });
  }

  /* ---- events from map.js --------------------------------------------------------- */
  function pointerDown(ev) {
    if (!C.map()) return false;
    const t = C.tool();
    if (t === 'road') lineDown('road', ev);
    else if (t === 'region') lineDown('region', ev);
    else if (t === 'marker') markerDown(ev);
    else return false;
    C.paint();
    return true;
  }
  function pointerMove(ev) {
    const d = st.drag;
    if (!d || !(ev.buttons & 1)) return false;
    const [fx, fz] = C.worldAtF(ev);
    if (d.layer === 'marker') d.obj.at = [Math.round(fx) + d.dx, Math.round(fz) + d.dz];
    else {
      const p = d.obj.points[d.k];
      p[0] = Math.round(fx); p[1] = Math.round(fz);
      for (const [o, k] of d.links) { o.points[k][0] = p[0]; o.points[k][1] = p[1]; }
      d.alt = ev.altKey;
    }
    d.moved = true;
    C.markDirty(true);
    if (els.insp && !d.redraw) { d.redraw = true; requestAnimationFrame(() => { d.redraw = false; syncInspector(); }); }
    C.paint();
    return true;
  }
  function pointerUp() {
    const d = st.drag;
    if (!d) return false;
    st.drag = null;
    if (d.moved && d.layer !== 'marker' && !d.alt && !d.links.length) {
      // snap the dropped vertex (a junction keeps its partners: it is one already)
      const p = d.obj.points[d.k], s = snapAt(d.layer, p[0], p[1], d.obj);
      const q = landSnap(s);
      p[0] = q[0]; p[1] = q[1];
    }
    if (d.moved) { if (d.snap) C.pushUndo(d.snap); C.markDirty(true); }
    sync(); C.paint();
    return true;
  }
  function dblclick() {
    if (!st.drawing || !selObj()) return false;
    const pts = selObj().points;
    if (pts.length >= 2) {
      const a = pts[pts.length - 1], b = pts[pts.length - 2];
      if (px(Math.hypot(a[0] - b[0], a[1] - b[1])) < 4) pts.pop();
    }
    finish();
    return true;
  }
  function key(ev) {
    if (st.drawing && (ev.key === 'Enter' || ev.key === 'Escape')) { finish(); return true; }
    if ((ev.key === 'Delete' || ev.key === 'Backspace') && st.sel && !st.drawing) {
      const o = selObj();
      if (!o) return false;
      if (st.sel.k >= 0 && st.sel.layer !== 'marker') {
        const closed = st.sel.layer === 'region', k = st.sel.k;
        if (distinctPoints(o.points, closed) <= (closed ? 3 : 2)) { C.toast(`${st.sel.layer} "${o.id}" needs its points: delete the whole ${st.sel.layer} from its row instead`, true); return true; }
        edit(() => { o.points.splice(k, 1); st.sel.k = -1; });
      } else removeSelected();
      return true;
    }
    return false;
  }
  function removeSelected() {
    const o = selObj();
    if (!o) return;
    const layer = st.sel.layer;
    edit(() => { const a = arr(layer); a.splice(a.indexOf(o), 1); st.sel = null; st.drawing = false; });
  }
  function afterRestore() {
    if (st.sel && !selObj()) st.sel = null;
    if (!st.sel) st.drawing = false;
    st.drag = null;
    sync();
  }
  function reset() { st.sel = null; st.drawing = false; st.drag = null; st.probe = null; sync(); }

  /* ---- painting ---------------------------------------------------------------------- */
  function strokePath(g, pts, closed) {
    g.beginPath();
    pts.forEach((p, k) => (k ? g.lineTo(C.SX(p[0]), C.SZ(p[1])) : g.moveTo(C.SX(p[0]), C.SZ(p[1]))));
    if (closed) g.closePath();
  }
  function vertexSquare(g, p, fill, ring) {
    const x = Math.round(C.SX(p[0])), z = Math.round(C.SZ(p[1]));
    g.fillStyle = ring; g.fillRect(x - 2, z - 2, 5, 5);
    g.fillStyle = fill; g.fillRect(x - 1, z - 1, 3, 3);
  }
  function paint(g) {
    if (!C.map()) return;
    const j = J(), hover = C.hover && C.hover();
    g.save();
    g.font = '11px monospace';
    if (st.show.regions) for (const r of j.regions || []) {
      const pts = r.points || [];
      if (!pts.length) continue;
      const sel = st.sel && st.sel.layer === 'region' && st.sel.id === r.id, col = /^#[0-9a-f]{6}$/i.test(r.colour || '') ? r.colour : '#888888';
      strokePath(g, pts, true);
      g.globalAlpha = sel ? 0.26 : 0.16; g.fillStyle = col; g.fill();
      g.globalAlpha = 1; g.strokeStyle = sel ? '#ffffff' : col; g.lineWidth = sel ? 2 : 1;
      g.setLineDash(sel ? [] : [6, 3]); g.stroke(); g.setLineDash([]);
      if (sel || C.tool() === 'region') pts.forEach((p, k) => vertexSquare(g, p, sel && st.sel.k === k ? '#ffd866' : '#0e1219', sel ? '#ffffff' : col));
      const [lx, lz] = polygonLabelAt(pts);
      const label = r.name || r.id;
      g.fillStyle = '#0e1219'; g.globalAlpha = 0.7;
      const w = g.measureText(label).width;
      g.fillRect(Math.round(C.SX(lx) - w / 2) - 3, Math.round(C.SZ(lz)) - 10, Math.ceil(w) + 6, 14);
      g.globalAlpha = 1; g.fillStyle = col;
      g.fillText(label, Math.round(C.SX(lx) - w / 2), Math.round(C.SZ(lz)));
    }
    if (st.show.roads) {
      const roads = j.roads || [];
      // shoulders, then carriageways, then centrelines: the junctions read clean
      for (const pass of [0, 1, 2]) for (const r of roads) {
        const pts = r.points || [];
        if (pts.length < 1) continue;
        const sel = st.sel && st.sel.layer === 'road' && st.sel.id === r.id, col = surfaceColour(r.surface);
        const d = ROAD_KINDS[r.kind] || ROAD_KINDS.road, w = r.width == null ? d.width : r.width, sh = r.shoulder == null ? d.shoulder : r.shoulder;
        g.lineCap = 'round'; g.lineJoin = 'round';
        strokePath(g, roadCentreline(r), false);
        if (pts.length === 1) g.lineTo(C.SX(pts[0][0]) + 0.01, C.SZ(pts[0][1]));
        if (pass === 0) { g.globalAlpha = 0.18; g.strokeStyle = col; g.lineWidth = Math.max(3, px(w + 2 * sh)); g.stroke(); }
        else if (pass === 1) { g.globalAlpha = sel ? 0.95 : 0.8; g.strokeStyle = col; g.lineWidth = Math.max(2, px(w)); g.stroke(); }
        else {
          g.globalAlpha = 1; g.lineCap = 'butt';
          g.strokeStyle = sel ? '#ffffff' : (r.kind === 'path' ? '#5a4630' : '#2a2a2a'); g.lineWidth = sel ? 2 : 1;
          if (r.kind === 'path') g.setLineDash([4, 3]);
          g.stroke(); g.setLineDash([]);
        }
      }
      g.globalAlpha = 1;
      for (const r of roads) {
        const pts = r.points || [];
        const sel = st.sel && st.sel.layer === 'road' && st.sel.id === r.id;
        if (sel || C.tool() === 'road')
          pts.forEach((p, k) => {
            const jn = junctionCount(p[0], p[1]) > 1;
            vertexSquare(g, p, sel && st.sel.k === k ? '#ffd866' : jn ? '#ffd866' : '#0e1219', sel ? '#ffffff' : jn ? '#1c1712' : '#c8b48a');
          });
        if (pts.length >= 2 && (sel || C.view.scale >= 0.08)) {
          const cl = roadCentreline(r), m = pointAtArc(cl, vertexArcs(cl)[cl.length - 1] / 2);
          g.fillStyle = sel ? '#ffffff' : '#e8dcc0';
          g.fillText(r.name || r.id, Math.round(C.SX(m[0])) + 6, Math.round(C.SZ(m[1])) - 6);
        }
      }
    }
    // the rubber band while drawing
    if (st.drawing && hover) {
      const o = selObj();
      if (o && o.points.length) {
        const a = o.points[o.points.length - 1];
        g.strokeStyle = 'rgba(255,255,255,0.7)'; g.lineWidth = 1; g.setLineDash([3, 3]);
        g.beginPath(); g.moveTo(C.SX(a[0]), C.SZ(a[1])); g.lineTo(C.SX(hover[0]), C.SZ(hover[1]));
        if (st.sel.layer === 'region' && o.points.length >= 2) g.lineTo(C.SX(o.points[0][0]), C.SZ(o.points[0][1]));
        g.stroke(); g.setLineDash([]);
      }
    }
    // snap preview: the target under the cursor while drawing / dragging a vertex
    if (hover && (st.drawing || (st.drag && st.drag.layer !== 'marker')) && C.tool() !== 'marker') {
      const self = st.drag ? st.drag.obj : selObj();
      const s = snapAt(st.sel ? st.sel.layer : 'road', hover[0], hover[1], self);
      if (s.vertexOf || s.segOf) {
        const x = Math.round(C.SX(s.x)), z = Math.round(C.SZ(s.z));
        g.strokeStyle = '#ffd866'; g.lineWidth = 1;
        g.strokeRect(x - 5.5, z - 5.5, 11, 11);
      }
    }
    if (st.show.markers) for (const m of j.markers || []) {
      if (!Array.isArray(m.at)) continue;
      const sel = st.sel && st.sel.layer === 'marker' && st.sel.id === m.id;
      const x = Math.round(C.SX(m.at[0])), z = Math.round(C.SZ(m.at[1]));
      // a dark badge, the 12 px icon at 2x, a pin tail to the column
      g.fillStyle = sel ? '#ffd866' : '#c8b48a'; g.fillRect(x - 14, z - 30, 28, 28);
      g.fillStyle = '#0e1219'; g.fillRect(x - 13, z - 29, 26, 26);
      g.fillStyle = sel ? '#ffd866' : '#c8b48a'; g.fillRect(x - 1, z - 2, 3, 3);
      g.fillRect(x, z - 3, 1, 3);
      g.drawImage(iconCanvas(m.icon), x - 12, z - 28, 24, 24);
      g.fillStyle = sel ? '#ffd866' : '#e8dcc0';
      g.fillText(m.name || m.id, x + 17, z - 12);
    }
    if (st.probe) {
      const x = Math.round(C.SX(st.probe[0])), z = Math.round(C.SZ(st.probe[1]));
      g.strokeStyle = '#ff5a5a'; g.lineWidth = 2; g.strokeRect(x - 4, z - 4, 9, 9);
    }
    g.restore();
  }

  /* ---- the panel ---------------------------------------------------------------------- */
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'mapvector', open: true});
    det.append(el('summary', {}, 'Vector layers — roads, regions, markers (map.json)'));
    const showChk = (k, label) => {
      const c = el('input', {type: 'checkbox', title: 'draw the ' + label + ' layer'});
      c.checked = st.show[k];
      c.addEventListener('change', () => { st.show[k] = c.checked; C.paint(); });
      return el('label', {}, c, ' ' + label);
    };
    det.append(el('div', {class: 'mapbar'}, 'show ', showChk('roads', 'roads'), showChk('regions', 'regions'), showChk('markers', 'markers')));
    els.roads = el('div', {class: 'mapsites-list'});
    els.regions = el('div', {class: 'mapsites-list'});
    els.markers = el('div', {class: 'mapsites-list'});
    els.insp = el('div', {class: 'maptools-panel'});
    det.append(el('div', {class: 'mapsub'}, 'Roads (graded ground the engine builds)'), els.roads,
               el('div', {class: 'mapsub'}, 'Regions (named areas: data only)'), els.regions,
               el('div', {class: 'mapsub'}, 'Markers (annotations; locales are the locale editor’s)'), els.markers, els.insp);
    return det;
  }
  /** Tool-bar option groups for map.js's els.opts. */
  function toolOpts() {
    const el = C.el;
    const kind = el('select', {title: 'a NEW road’s kind: its width, shoulder and grade defaults'}, ...Object.keys(ROAD_KINDS).map(k => el('option', {value: k}, k)));
    kind.value = st.newRoad.kind;
    kind.addEventListener('change', () => { st.newRoad.kind = kind.value; });
    els.newSurface = el('select', {title: 'a NEW road’s surface (a solid material: a road on a grade must not be a powder)'});
    const fillSurf = () => {
      els.newSurface.replaceChildren(...surfaces().map(s => el('option', {value: s}, s)));
      if (!surfaces().includes(st.newRoad.surface)) st.newRoad.surface = surfaces()[0];
      els.newSurface.value = st.newRoad.surface;
    };
    fillSurf();
    els.fillSurf = fillSurf;
    els.newSurface.addEventListener('change', () => { st.newRoad.surface = els.newSurface.value; });
    const icons = el('span', {class: 'mapicons'});
    els.newIcons = icons;
    for (const n of ICON_NAMES) {
      const b = el('button', {class: 'mapicon' + (n === st.newIcon ? ' on' : ''), title: n, 'data-icon': n});
      const cv = el('canvas', {width: 24, height: 24});
      const g = cv.getContext('2d'); g.imageSmoothingEnabled = false; g.drawImage(iconCanvas(n), 0, 0, 24, 24);
      b.append(cv);
      b.addEventListener('click', () => {
        st.newIcon = n;
        for (const x of icons.children) x.classList.toggle('on', x.dataset.icon === n);
        C.setTool('marker');
      });
      icons.append(b);
    }
    return {
      road: el('span', {class: 'mapopts'}, 'new ', kind, ' ', els.newSurface,
               el('span', {class: 'mapnote'}, ' · click to draw, double-click ends · Ctrl+click on a road branches off it · drag a vertex · Alt = no snap')),
      region: el('span', {class: 'mapopts'}, el('span', {class: 'mapnote'}, 'click to draw a polygon, click the first vertex to close · drag a vertex · Ctrl+click inside a region starts another')),
      marker: el('span', {class: 'mapopts'}, 'icon ', icons),
    };
  }
  function rowFor(layer, o, text) {
    const el = C.el;
    const on = st.sel && st.sel.layer === layer && st.sel.id === o.id;
    const row = el('div', {class: 'mapsite' + (on ? ' on' : '')});
    const tag = el('span', {class: 'mapkind mapkind-' + layer}, layer === 'road' ? (o.kind || 'road') : layer);
    if (layer === 'marker') {
      const cv = el('canvas', {width: 12, height: 12, class: 'mapiconsm'});
      cv.getContext('2d').drawImage(iconCanvas(o.icon), 0, 0);
      tag.replaceChildren(cv, ' ' + (o.icon || '?'));
    }
    const sel = el('button', {title: 'select and centre on the map'}, on ? 'selected' : 'select');
    sel.addEventListener('click', () => { selectById(layer, o.id, true); });
    const del = el('button', {title: 'delete this ' + layer}, '×');
    del.addEventListener('click', () => { st.sel = {layer, id: o.id, k: -1}; removeSelected(); });
    row.append(tag, el('span', {class: 'mapwhere'}, text), sel, del);
    return row;
  }
  /** Select by id; `centre` moves the view onto it. */
  function selectById(layer, id, centre) {
    const o = find(layer, id);
    if (!o) return false;
    st.drawing = false;
    st.sel = {layer, id, k: -1};
    if (centre) {
      if (layer === 'marker') C.fitBox(o.at[0] - 200, o.at[1] - 200, o.at[0] + 200, o.at[1] + 200);
      else if ((o.points || []).length) {
        let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
        for (const p of o.points) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
        C.fitBox(x0, z0, x1, z1);
      }
    }
    sync(); C.paint();
    return true;
  }
  function sync() {
    if (!els.roads || !C.map()) return;
    const el = C.el, v = C.vpm(), j = J();
    if (els.fillSurf) els.fillSurf();
    const listInto = (box, layer, items, text, empty) => {
      box.replaceChildren(...items.map(o => rowFor(layer, o, text(o))));
      if (!items.length) box.append(el('div', {class: 'mapnote'}, empty));
    };
    listInto(els.roads, 'road', j.roads || [], r => { const cl = roadCentreline(r); return `${r.name || r.id} · ${fmtM(vertexArcs(cl)[cl.length - 1] / v || 0)} · ${r.surface || '?'}${r.smooth === true ? ' · bends' : ''}`; },
             'no roads — pick the Road tool and click on the map');
    listInto(els.regions, 'region', j.regions || [], r => `${r.name || r.id} · ${(r.points || []).length} pts · ${(Math.abs(polygonArea(r.points || [])) / v / v / 1e6).toFixed(3)} km²`,
             'no regions — pick the Region tool and click out a polygon');
    listInto(els.markers, 'marker', j.markers || [], m => `${m.name || m.id} · (${(m.at || []).join(',')})`,
             'no markers — pick the Marker tool, an icon, and click');
    syncInspector();
  }

  /* ---- the inspector ----------------------------------------------------------------- */
  function syncInspector() {
    const box = els.insp;
    if (!box || !C.map()) return;
    const el = C.el, v = C.vpm();
    box.replaceChildren();
    const o = selObj();
    if (!o) { box.append(el('div', {class: 'mapnote'}, 'Select a road, region or marker to edit it.')); return; }
    const layer = st.sel.layer;
    const text = (label, key, title, onSet) => {
      const i = el('input', {type: 'text', value: o[key] || '', class: 'mapid', title});
      i.addEventListener('change', () => {
        const val = i.value.trim();
        if (onSet && !onSet(val)) { i.value = o[key] || ''; return; }
        edit(() => { if (val) o[key] = val; else delete o[key]; });
      });
      return el('label', {title}, label, ' ', i);
    };
    const idField = text('id', 'id', 'unique within the ' + layer + 's; what warnings and RegionAt name', (val) => {
      if (!val) { C.toast('an id cannot be empty', true); return false; }
      if (val !== o.id && (J()[layer + 's'] || []).some(x => x.id === val)) { C.toast(`another ${layer} is already "${val}"`, true); return false; }
      st.sel.id = val;
      return true;
    });
    if (layer === 'marker') {
      const icons = el('span', {class: 'mapicons'});
      for (const n of ICON_NAMES) {
        const b = el('button', {class: 'mapicon' + (n === o.icon ? ' on' : ''), title: n});
        const cv = el('canvas', {width: 24, height: 24});
        const g = cv.getContext('2d'); g.imageSmoothingEnabled = false; g.drawImage(iconCanvas(n), 0, 0, 24, 24);
        b.append(cv);
        b.addEventListener('click', () => edit(() => { o.icon = n; }));
        icons.append(b);
      }
      const xyz = (i, label) => {
        const inp = el('input', {type: 'number', step: 1, value: o.at[i], style: 'width:80px'});
        inp.addEventListener('change', () => edit(() => { o.at[i] = Math.round(+inp.value || 0); }));
        return el('label', {}, label, ' ', inp);
      };
      box.append(el('b', {}, 'marker “' + (o.name || o.id) + '”'), idField, text('name', 'name', 'the label on the map'),
                 el('div', {}, 'icon ', icons), xyz(0, 'x (vox)'), xyz(1, 'z (vox)'),
                 el('span', {class: 'mapnote'}, 'A map annotation: the engine loads and checks it but builds nothing. Drag it with the Marker tool; shift+click deletes.'));
      return;
    }
    if (layer === 'region') {
      const col = el('input', {type: 'color', value: /^#[0-9a-f]{6}$/i.test(o.colour || '') ? o.colour : '#888888'});
      col.addEventListener('change', () => edit(() => { o.colour = col.value; }));
      const A = Math.abs(polygonArea(o.points || []));
      box.append(el('b', {}, 'region “' + (o.name || o.id) + '”'), idField, text('name', 'name', 'what RegionAt answers'),
                 el('label', {}, 'colour ', col),
                 el('span', {class: 'mapnote'}, `${o.points.length} vertices · ${(A / v / v / 1e6).toFixed(3)} km² (${Math.round(A / v / v).toLocaleString()} m²). ` +
                    'Data only: the engine validates it and answers RegionAt(x, z) with the LAST region holding the column, so a region drawn later sits on top. ' +
                    'Drag a vertex, click an edge to add one, shift+click a vertex or Delete to remove it.'));
      box.append(vertexTable(o, true));
      return;
    }
    // ---- a road
    const kd = ROAD_KINDS[o.kind] || ROAD_KINDS.road;
    const kind = el('select', {title: 'road / track / path: the defaults below follow the kind where they were the old kind’s'},
                    ...Object.keys(ROAD_KINDS).map(k => el('option', {value: k}, k)));
    kind.value = o.kind || 'road';
    kind.addEventListener('change', () => edit(() => {
      const from = ROAD_KINDS[o.kind || 'road'], to = ROAD_KINDS[kind.value];
      for (const k of ['width', 'shoulder', 'maxGrade']) if (o[k] == null || o[k] === from[k]) o[k] = to[k];
      o.kind = kind.value;
    }));
    const surf = el('select', {title: 'the surface material: a SOLID (the engine refuses a powder: a graded road is a slope and a powder on one never sleeps)'});
    const opts = surfaces();
    if (o.surface && !opts.includes(o.surface)) opts.push(o.surface);
    surf.replaceChildren(...opts.map(s => el('option', {value: s}, s)));
    surf.value = o.surface || 'packed_earth';
    surf.addEventListener('change', () => edit(() => { o.surface = surf.value; }));
    const num = (label, key, def, unit, title) => {
      const [lo, hi] = ROAD_LIMITS[key];
      const inp = el('input', {type: 'number', min: lo, max: hi, step: 1, value: o[key] == null ? def : o[key], style: 'width:60px', title});
      const out = el('span', {class: 'mapunit'}, unit(o[key] == null ? def : o[key]));
      inp.addEventListener('change', () => edit(() => { o[key] = Math.max(lo, Math.min(hi, Math.round(+inp.value || 0))); }));
      return el('label', {title}, label, ' ', inp, out);
    };
    const ocl = roadCentreline(o), len = vertexArcs(ocl)[ocl.length - 1] || 0;
    const bend = el('input', {type: 'checkbox', title: 'BEND: the road is a smooth spline through its points (the engine tessellates it the same way: map.json smooth: true); off = straight segments between them'});
    bend.checked = o.smooth === true;
    bend.addEventListener('change', () => edit(() => { if (bend.checked) o.smooth = true; else o.smooth = false; }));
    box.append(el('b', {}, (o.kind || 'road') + ' “' + (o.name || o.id) + '”'), idField, text('name', 'name', 'a name for the road (optional)'),
      el('label', {}, 'kind ', kind), el('label', {}, 'surface ', surf),
      num('width', 'width', kd.width, w => (w / v).toFixed(1) + ' m', 'carriageway width, voxels (2..256): the surface is laid over this'),
      num('shoulder', 'shoulder', kd.shoulder, s => (s / v).toFixed(1) + ' m', 'the ramp back to the bare ground either side, voxels (0..64); the engine widens it where a cut or fill would step more than a voxel'),
      num('max grade', 'maxGrade', kd.maxGrade, g => '%', 'steepest the baked profile may climb, percent: cuts through bumps, fills dips'),
      num('depth', 'depth', ROAD_DEPTH_DEFAULT, d => 'cells', 'surface cells from the ground’s top down (1..8)'),
      el('label', {}, bend, ' bend (smooth)'),
      el('span', {class: 'mapnote'}, `${o.points.length} vertices · ${fmtM(len / v)} long` + (o.smooth === true ? ` (the curve: ${ocl.length} engine vertices)` : '')));
    const bar = el('div', {class: 'mapbar'});
    const cont = el('button', {title: 'add vertices after the last one (Enter / double-click ends)'}, st.drawing ? 'Finish' : 'Continue from end');
    cont.addEventListener('click', () => { if (st.drawing) finish(); else { st.drawing = true; C.setTool('road'); sync(); } });
    const rev = el('button', {title: 'reverse the vertex order (the profile chart runs from the other end)'}, 'Reverse');
    rev.addEventListener('click', () => edit(() => { o.points.reverse(); }));
    bar.append(cont, rev);
    box.append(bar, vertexTable(o, false), profilePanel(o));
  }
  function vertexTable(o, closed) {
    const el = C.el, v = C.vpm();
    const tbl = el('div', {class: 'maplinepts'});
    o.points.forEach((p, k) => {
      const row = el('div', {class: 'mapsite' + (st.sel && st.sel.k === k ? ' on' : '')});
      const xy = i => {
        const inp = el('input', {type: 'number', step: 1, value: p[i], style: 'width:66px'});
        inp.addEventListener('change', () => edit(() => {
          const n = Math.round(+inp.value || 0), links = closed ? [] : linksAt('road', p[0], p[1], o);
          p[i] = n;
          for (const [q, kk] of links) q.points[kk][i] = n;   // a junction moves together
        }));
        return inp;
      };
      const jn = !closed && junctionCount(p[0], p[1]) > 1;
      const go = el('button', {title: 'select this vertex and centre on it'}, k + (jn ? ' ⋈' : ''));
      go.addEventListener('click', () => { st.sel.k = k; C.view.cx = p[0]; C.view.cz = p[1]; sync(); C.paint(); });
      const del = el('button', {title: 'delete this vertex'}, '×');
      del.addEventListener('click', () => {
        if (distinctPoints(o.points, closed) <= (closed ? 3 : 2)) { C.toast('too few vertices left: delete the whole ' + (closed ? 'region' : 'road') + ' instead', true); return; }
        edit(() => { o.points.splice(k, 1); st.sel.k = -1; });
      });
      row.append(go, xy(0), xy(1), el('span', {class: 'mapunit'}, `${fmtM(p[0] / v)}, ${fmtM(p[1] / v)}`), del);
      tbl.append(row);
    });
    return tbl;
  }

  /* ---- THE PROFILE: the engine's bake of the SAVED road ---------------------------- */
  /** The saved road this one was (by id), or null. */
  function savedRoad(id) {
    const sj = C.savedJson && C.savedJson();
    return sj ? (sj.roads || []).find(r => r.id === id) || null : null;
  }
  const roadKey = r => JSON.stringify([r.points, r.kind, r.width, r.shoulder, r.maxGrade, r.surface, r.depth, r.smooth === true]);
  /** {engine road entry, stale?, why} for the inspector's chart. */
  function bakeOf(o) {
    const ck = C.check && C.check();
    if (!ck) return {why: 'asking the engine (--mapcheck) for the bake…'};
    if (ck.unavailable) return {why: 'the engine could not be asked for the bake: ' + (ck.error || '?')};
    if (!ck.ok) return {why: 'the engine REFUSES the saved map, so it baked nothing: ' + String(ck.error || '?').slice(0, 200)};
    const sr = savedRoad(o.id), e = (ck.roads || []).find(r => r.id === o.id) || null;
    if (!sr || !e) return {why: 'not saved yet: Save the map and the engine bakes this road’s profile'};
    return {e, stale: roadKey(sr) !== roadKey(o)};
  }
  function profilePanel(o) {
    const el = C.el;
    const wrap = el('div', {class: 'mapprofilebox'});
    const b = bakeOf(o);
    const cv = el('canvas', {width: 320, height: 120, class: 'mapprofile mapprofile-road',
                             title: 'the ENGINE’s bake of the saved road (--mapcheck): the road surface over the bare ground it was graded from, metres above sea; vertices ticked'});
    const note = el('div', {class: 'mapnote'});
    wrap.append(el('div', {class: 'mapsub'}, 'Profile (the engine’s bake)'), cv, note);
    drawRoadProfile(cv, o, b, note);
    cv.addEventListener('mousemove', ev => {
      if (!b.e) return;
      const P = profileOf(b.e), r = cv.getBoundingClientRect();
      const s = P.s[P.s.length - 1] * Math.max(0, Math.min(1, (ev.clientX - r.left - 4) / (cv.width - 8)));
      st.probe = pointAtArc(roadCentreline(b.stale ? (savedRoad(o.id) || o) : o), s);
      drawRoadProfile(cv, o, b, note, s);
      C.paint();
    });
    cv.addEventListener('mouseleave', () => { st.probe = null; drawRoadProfile(cv, o, b, note); C.paint(); });
    return wrap;
  }
  /** The engine's entry as {s, y, g} arrays: the dense profile, or (an exe that
   *  predates it) the per-vertex numbers at the polyline's arc lengths. */
  function profileOf(e) {
    if (e.profile && Array.isArray(e.profile.s) && e.profile.s.length >= 2) return {s: e.profile.s, y: e.profile.y, g: e.profile.ground, dense: true};
    const sr = savedRoad(e.id);
    const arcs = sr ? vertexArcs(roadCentreline(sr)) : (e.vertexY || []).map((_, k) => k);
    return {s: arcs, y: e.vertexY || [], g: e.vertexGround || [], dense: false};
  }
  function drawRoadProfile(cv, o, b, note, probeS) {
    const g = cv.getContext('2d'), W = cv.width, Hh = cv.height, v = C.vpm(), sea = C.seaY();
    g.fillStyle = '#0e1219'; g.fillRect(0, 0, W, Hh);
    g.font = '10px monospace';
    if (!b.e) { liveGround(g, o, W, Hh); g.fillStyle = '#6f7f97'; g.fillText('no bake to show: the plane under the current centreline', 6, 14); note.textContent = b.why; return; }
    const P = profileOf(b.e), N = P.s.length, S = Math.max(1, P.s[N - 1]);
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < N; i++) { lo = Math.min(lo, P.y[i], P.g[i]); hi = Math.max(hi, P.y[i], P.g[i]); }
    if (hi - lo < 20) { const m = (hi + lo) / 2; hi = m + 10; lo = m - 10; }
    const X = s => 4 + (s / S) * (W - 8), Y = h => Math.round(Hh - 14 - (h - lo) / (hi - lo) * (Hh - 28));
    // the bare ground: a filled silhouette, cut (ground over road) and fill (under) tinted
    for (let i = 0; i + 1 < N; i++) {
      const x0 = Math.round(X(P.s[i])), x1 = Math.max(x0 + 1, Math.round(X(P.s[i + 1])));
      for (let x = x0; x < x1; x++) {
        const t = (x - x0) / Math.max(1, x1 - x0), gh = P.g[i] + (P.g[i + 1] - P.g[i]) * t, yh = P.y[i] + (P.y[i + 1] - P.y[i]) * t;
        g.fillStyle = '#3a4a3a'; g.fillRect(x, Y(gh), 1, Hh - 12 - Y(gh));
        if (Math.abs(gh - yh) >= 1) { g.fillStyle = gh > yh ? 'rgba(255,120,90,0.45)' : 'rgba(110,170,255,0.45)'; g.fillRect(x, Math.min(Y(gh), Y(yh)), 1, Math.abs(Y(gh) - Y(yh))); }
      }
    }
    // the road surface; a span over maxGrade (it should never be) in red
    const mg = o.maxGrade == null ? (ROAD_KINDS[o.kind] || ROAD_KINDS.road).maxGrade : b.e.maxGrade;
    let over = 0;
    for (let i = 0; i + 1 < N; i++) {
      const ds = Math.max(1, P.s[i + 1] - P.s[i]), gr = Math.abs(P.y[i + 1] - P.y[i]) / ds * 100;
      const bad = P.dense && gr > mg + 100 / ds;   // one voxel of rounding at each end
      if (bad) over++;
      g.strokeStyle = bad ? '#ff4a4a' : surfaceColour(o.surface); g.lineWidth = 2;
      g.beginPath(); g.moveTo(X(P.s[i]), Y(P.y[i])); g.lineTo(X(P.s[i + 1]), Y(P.y[i + 1])); g.stroke();
    }
    // vertex ticks along the bottom
    const sr = savedRoad(o.id);
    g.fillStyle = '#c7d2e3';
    for (const a of sr ? knotArcs(sr) : []) g.fillRect(Math.round(X(a)), Hh - 11, 1, 4);
    g.fillText(fmtM((hi - sea) / v), 3, 10);
    g.fillText(fmtM((lo - sea) / v), 3, Hh - 2);
    g.textAlign = 'right'; g.fillText(fmtM(S / v), W - 3, Hh - 2); g.textAlign = 'left';
    let cut = 0, fill = 0;
    for (let i = 0; i < N; i++) { cut = Math.max(cut, P.g[i] - P.y[i]); fill = Math.max(fill, P.y[i] - P.g[i]); }
    let line = `engine: ${b.e.pieces} pieces, worst grade ${(+b.e.worstGrade || 0).toFixed(1)} % (max ${b.e.maxGrade} %), deepest cut ${fmtM(cut / v)}, highest fill ${fmtM(fill / v)}` +
               (P.dense ? '' : ' — vertices only (this sandvox.exe predates the dense profile)') +
               (over ? ` — ${over} span(s) over the grade` : '');
    if (probeS != null) {
      let i = 0;
      while (i + 1 < N - 1 && P.s[i + 1] < probeS) i++;
      const t = Math.max(0, Math.min(1, (probeS - P.s[i]) / Math.max(1, P.s[i + 1] - P.s[i])));
      const yh = P.y[i] + (P.y[i + 1] - P.y[i]) * t, gh = P.g[i] + (P.g[i + 1] - P.g[i]) * t;
      g.strokeStyle = 'rgba(255,90,90,0.8)'; g.lineWidth = 1;
      g.beginPath(); g.moveTo(Math.round(X(probeS)) + 0.5, 0); g.lineTo(Math.round(X(probeS)) + 0.5, Hh - 12); g.stroke();
      line = `at ${fmtM(probeS / v)}: road ${fmtM((yh - sea) / v)} asl, bare ground ${fmtM((gh - sea) / v)} (${yh >= gh ? 'fill ' : 'cut '}${fmtM(Math.abs(yh - gh) / v)})`;
    }
    note.textContent = (b.stale ? 'EDITED since the save — this is the SAVED road’s bake; Save to re-bake. ' : '') + line;
    note.classList.toggle('bad', !!b.stale);
  }

  /** No engine bake yet: the HEIGHT PLANE under the current centreline (the
   *  path a save would grade), so the chart still shows the bent route. */
  function liveGround(g, o, W, Hh) {
    if (!C.heightAt) return;
    const cl = roadCentreline(o), A = vertexArcs(cl), L = A[A.length - 1] || 0;
    if (cl.length < 2 || L <= 0) return;
    const n = Math.min(300, Math.max(2, Math.ceil(L / 8))), hs = [];
    for (let i = 0; i <= n; i++) { const p = pointAtArc(cl, L * i / n); hs.push(C.heightAt(p[0], p[1])); }
    let lo = Math.min(...hs), hi = Math.max(...hs);
    if (hi - lo < 20) { const m = (hi + lo) / 2; hi = m + 10; lo = m - 10; }
    g.fillStyle = '#2a362a';
    for (let i = 0; i < n; i++) {
      const x0 = Math.round(4 + i / n * (W - 8)), x1 = Math.max(x0 + 1, Math.round(4 + (i + 1) / n * (W - 8)));
      const y = Math.round(Hh - 14 - (hs[i] - lo) / (hi - lo) * (Hh - 28));
      g.fillRect(x0, y, x1 - x0, Hh - 12 - y);
    }
    g.fillStyle = '#c7d2e3';
    for (const a of knotArcs(o)) g.fillRect(Math.round(4 + a / L * (W - 8)), Hh - 11, 1, 4);
  }

  return {
    st, buildPanel, toolOpts, paint, pointerDown, pointerMove, pointerUp, dblclick, key, sync, afterRestore, reset,
    selectById, finish, problems: () => (C.map() ? vectorProblems(J()) : null),
    dragging: () => !!st.drag,
  };
}
