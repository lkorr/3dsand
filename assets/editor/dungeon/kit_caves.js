/* kit_caves.js — the CAVES kit (PLAN_dungeons §4): organic, few right
 * angles, limestone / basalt / ice by palette.
 *
 * Every piece: origin socket ('in', or 'surface' / 'top' for entrances and
 * shafts) at (0,0,0), body to +Z (or down -Y for a shaft). Cave passages are
 * spline tubes and ellipsoid clusters with heavy world-space wall noise, so
 * neighbouring pieces flow into each other (the seam rule, PLAN §2).
 *
 * Caves opt IN to the natural cave bands (PLAN §2.2: `naturalCaves: allow`).
 */
'use strict';
import { defineKit, clamp } from './dgutil.js';

export const KIT = {
  name: 'caves', label: 'Caves',
  roughMax: 14, floorMax: 5, octaves: 3, repose: 0.9,
  naturalCaves: 'allow',
  lightTypes: ['fungus', 'crystal', 'torch', 'none'],
  lights: {fungus: {prop: 'caves/glow_mushroom_patch', power: (p) => ({glow: p, giant: 1 + Math.round(2 * p), spread: +(0.8 + 0.8 * p).toFixed(2)})},
           candles: {prop: 'ruins/candle_cluster', power: (p) => ({count: 6 + Math.round(10 * p), spread: +(0.3 + 0.4 * p).toFixed(2), flame: p})},
           lantern: {prop: 'mines/lantern', power: (p) => ({glow: p})}},
  PALETTES: {
    limestone: {wall: 'limestone', floor: 'limestone', ceil: 'limestone', trim: 'chalk_rock', support: 'limestone',
                rubble: 'gravel', accent: 'crystal_spire', light: 'glowcap', lightCool: 'crystal', liquid: 'water',
                ore: 'crystal', moss: 'wet_moss', fungus: 'glowcap_cluster', veg: 'fern', hang: 'moss_hang',
                beach: 'sand', soil: 'dirt', loose: 'limestone'},
    basalt: {wall: 'basalt', floor: 'basalt', ceil: 'basalt', trim: 'brimstone', support: 'basalt',
             rubble: 'cinder', accent: 'crystal_spire', light: 'glowcap', lightCool: 'crystal', liquid: 'water',
             ore: 'brimstone', moss: 'old_moss', fungus: 'glowcap_cluster', veg: 'fern', hang: 'moss_hang',
             beach: 'cinder', soil: 'black_earth', loose: 'basalt'},
    ice: {wall: 'glacier_ice', floor: 'ice', ceil: 'glacier_ice', trim: 'rime_crystal', support: 'glacier_ice',
          rubble: 'snow', accent: 'rime_crystal', light: 'glowcap', lightCool: 'rime_crystal', liquid: 'water',
          ore: 'crystal', moss: 'rime_crystal', fungus: 'glowcap_cluster', veg: 'heath_shrub', hang: 'rime_crystal',
          beach: 'snow', soil: 'snow', loose: 'glacier_ice'},
    sea: {wall: 'sandstone', floor: 'sandstone', ceil: 'sandstone', trim: 'limestone', support: 'sandstone',
          rubble: 'sand', accent: 'crystal_spire', light: 'glowcap', lightCool: 'crystal', liquid: 'water',
          ore: 'crystal', moss: 'wet_moss', fungus: 'glowcap_cluster', veg: 'marram_grass', hang: 'kelp',
          beach: 'sand', soil: 'sand', loose: 'sandstone'}
  },
  DEFAULT_PALETTE: 'limestone',
  /** Dead-end pieces grow() may cap an open socket with. */
  CAPS: [{type: 'cavern', params: {exits: 0}}, {type: 'grotto_pool', params: {exits: 0}}],
  GROW: {tunnel: 6, cavern: 2, crawl: 1.2, grotto_pool: 0.8, chasm: 0.6, rubble_choke: 0.5, shaft: 0.6},
  TYPES: {}
};

const ROWS_FORMATIONS = [
  {sec: 'Formations', note: 'Speleothems are scatter rules: the GPU grows them per cell from the seed, so they cost no prims.'},
  {k: 'stalactites', n: 'stalactites', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Density on the ceiling.'},
  {k: 'stalagmites', n: 'stalagmites', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Density on the floor.'},
  {k: 'stalLength', n: 'formation length (m)', min: 0.2, max: 3.2, step: 0.1, def: 1.2, d: 'Longest stalactite / stalagmite.'}
];

function formations(b, P, prim) {
  const Lm = b.m(P.stalLength, 2);
  if (P.stalactites > 0) b.scatter.push({prim, kind: 'stalactite', density: +(P.stalactites * 0.5).toFixed(3), size: [2, Lm], surface: 'ceil', mat: 'support', seedKey: 'stalactite'});
  if (P.stalagmites > 0) b.scatter.push({prim, kind: 'stalagmite', density: +(P.stalagmites * 0.3).toFixed(3), size: [2, Math.max(2, Math.round(Lm * 0.7))], surface: 'floor', mat: 'support', seedKey: 'stalagmite'});
}

/** A meandering tube from (0,y0,0) along +Z. Returns {pts, radii, prim, floorY}.
 *  D8: the universal `wander` adds to the kit's own meander (both are zero
 *  with zero slope at the ends); `xsec` varies the CROSS-SECTION along the
 *  length (a second tube, offset up or down and to a side by a seeded amount
 *  per point, unions into the first: a round bore becomes a lens, a pear, a
 *  slot, changing as you walk); `canyon` cuts a vadose trench down from the
 *  floor of the bore (a keyhole section, its depth varying); the floor fill is
 *  open-space only, so it levels the bore without burying the walls. */
function tubePath(b, P, key, L, R, opt) {
  opt = opt || {};
  const n = Math.max(3, Math.round(L / b.m(2.5)) + 1);
  const amp = b.m(P.meander || 0), freq = (P.meanderFreq || 1) * Math.PI;
  const ph = b.r(key, 'phase') * Math.PI * 2;
  const rise = opt.rise || 0, shift = opt.shift || 0;
  const pts = [], radii = [];
  const squeezeAt = new Set();
  for (let k = 0; k < (P.squeezes || 0); k++) squeezeAt.add(1 + Math.floor(b.r(key, 'sq', k) * (n - 2)));
  for (let i = 0; i < n; i++) {
    const t = i / (n - 1);
    const env = Math.sin(Math.PI * t);
    const zz = Math.round(L * t);
    const x = Math.round(amp * env * Math.sin(ph + freq * t * 2) + shift * t) + b.wanderX(zz, L, key);
    let r = R;
    if (i > 0 && i < n - 1) {
      r = R * (1 + (P.radiusVar || 0) * (b.r(key, 'rv', i) * 2 - 1));
      if (squeezeAt.has(i)) r *= (P.squeeze || 0.6);
      // D8 width / height variation (radius here: a bore is round) -- the floor
      // stays at y = rise*t, so a taller section rises from it
      r *= b.varAt('widthVar', zz, L, key, 0.15) * b.varAt('heightVar', zz, L, key, 0.15);
    }
    r = Math.max(4, Math.round(r));
    const y = Math.round(rise * t) + (i === 0 || i === n - 1 ? R : r);   // floor near y = rise*t
    pts.push([x, y, zz]);
    radii.push(r);
  }
  const prim = b.room('spline_tube', {pts, radii});
  // Cross-section variation: a second bore, offset per point (ends pinned).
  if ((P.xsec || 0) > 0) {
    const pts2 = pts.map((p, i) => {
      if (i === 0 || i === n - 1) return p.slice();
      const up = (b.r(key, 'xs', i, 'y') * 2 - 1) * radii[i] * 0.7 * P.xsec, side = (b.r(key, 'xs', i, 'x') * 2 - 1) * radii[i] * 0.6 * P.xsec;
      return [Math.round(p[0] + side), Math.round(p[1] + Math.max(-radii[i] * 0.3, up)), p[2]];
    });
    const r2 = radii.map((r, i) => (i === 0 || i === n - 1) ? r : Math.max(4, Math.round(r * (0.55 + 0.35 * b.r(key, 'xs', i, 'r')))));
    b.room('spline_tube', {pts: pts2, radii: r2}, {tag: 'xsec'});
  }
  // Flatten the floor: a second tube shifted down fills the bottom of the round
  // (open space only: it levels the bore and never turns the walls to floor).
  const f = P.flatten || 0;
  const floorY = (i) => pts[i][1] - Math.round(radii[i] * (1 - 0.45 * f) * 0.9);
  if (f > 0) {
    b.fill('spline_tube', {pts: pts.map((p, i) => [p[0], p[1] - Math.round(radii[i] * (2 - 0.45 * f)), p[2]]), radii},
           {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
  }
  // Canyon: a narrow trench cut down from the floor, its depth seeded per
  // point, absent at the ends (the socket keeps its round opening).
  if ((P.canyon || 0) > 0) {
    const cpts = [], cr = [];
    for (let i = 0; i < n; i++) {
      const end = i === 0 || i === n - 1;
      const dep = end ? 0 : Math.round(radii[i] * (0.6 + 1.4 * P.canyon) * (0.4 + 0.6 * b.r(key, 'cy', i)));
      cpts.push([pts[i][0] + (end ? 0 : b.jit(Math.round(radii[i] * 0.3), key, 'cyx', i)), floorY(i) - dep, pts[i][2]]);
      cr.push(end ? 2 : Math.max(3, Math.round(radii[i] * (0.28 + 0.12 * b.r(key, 'cy', i, 'w')))));
    }
    // a slot: the trench is a chain of capsules from the floor down to its bottom
    for (let i = 1; i + 1 < n; i++) {
      const top = floorY(i) + 2;
      b.carve('capsule', {a: [cpts[i][0], top, cpts[i][2]], b: [cpts[i][0], cpts[i][1], cpts[i][2]], r: cr[i]}, {tag: 'canyon', noise: 0.8, blend: 3, layer: 3});
    }
    b.room('spline_tube', {pts: cpts, radii: cr}, {tag: 'canyon', noise: 0.8, blend: 3, layer: 3});
  }
  return {pts, radii, prim, floorY};
}

/** Side pockets and breakdown along a tube (D8): small chambers off the wall
 *  at seeded points, and blocks of fallen roof on its floor (open space only). */
function tubeDressing(b, P, key, tp) {
  const {pts, radii, floorY} = tp, n = pts.length;
  for (let k = 0; k < (P.pockets || 0); k++) {
    const i = 1 + Math.floor(b.r(key, 'pk', k) * Math.max(1, n - 2));
    const p = pts[i], r = radii[i], side = b.r(key, 'pk', k, 's') < 0.5 ? -1 : 1;
    const pr = Math.max(5, Math.round(r * (0.5 + 0.6 * b.r(key, 'pk', k, 'r'))));
    const cx = p[0] + side * (r + Math.round(pr * 0.5)), cy = floorY(i) + Math.round(pr * 0.7);
    b.carve('ellipsoid', {min: [cx - pr, floorY(i) - 2, p[2] - pr], max: [cx + pr, cy + pr, p[2] + pr]}, {tag: 'room', noise: 1, blend: Math.max(P.blend, 6)});
  }
  const nb = Math.round((P.breakdown || 0) * n * 0.8);
  for (let k = 0; k < nb; k++) {
    const i = 1 + Math.floor(b.r(key, 'bd', k) * Math.max(1, n - 2));
    const p = pts[i], r = radii[i];
    const s = Math.max(3, Math.round(r * (0.25 + 0.35 * b.r(key, 'bd', k, 's'))));
    const x = p[0] + Math.round((b.r(key, 'bd', k, 'x') * 2 - 1) * r * 0.5), z = p[2] + b.ri(-6, 6, key, 'bd', k, 'z');
    const y = floorY(i);
    b.fill('ellipsoid', {min: [x - s, y - 2, z - Math.round(s * 1.2)], max: [x + s, y + Math.round(s * 1.1), z + Math.round(s * 1.2)]}, {mat: 'loose', layer: 4, noise: 0.5, mask: 'air'});
  }
}

const ROWS_TUBE = [
  {sec: 'Passage'},
  {k: 'radiusVar', n: 'radius variation', min: 0, max: 0.8, step: 0.05, def: 0.25, d: 'Seeded swelling and pinching along the passage.'},
  {k: 'meander', n: 'meander (m)', min: 0, max: 10, step: 0.1, def: 1.5, d: 'Sideways wander (zero at both ends so the sockets line up).'},
  {k: 'meanderFreq', n: 'meander frequency', min: 0.2, max: 4, step: 0.1, def: 1, d: 'Bends per length.'},
  {k: 'squeezes', n: 'squeeze points', min: 0, max: 6, step: 1, int: true, def: 1, d: 'Places where the passage pinches.'},
  {k: 'squeeze', n: 'squeeze factor', min: 0.3, max: 1, step: 0.05, def: 0.65, d: 'Radius multiplier at a squeeze.'},
  {k: 'flatten', n: 'floor flatten', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Fill the bottom of the round with floor so it can be walked.'},
  {sec: 'Shape along the passage', note: 'What keeps a cave passage from reading as a pipe.'},
  {k: 'xsec', n: 'cross-section change', min: 0, max: 1, step: 0.05, def: 0, d: 'The bore\'s section changes along its length: a second, offset bore unions in (a lens, a pear, a tall slot, a low bedding plane), different at every point.'},
  {k: 'canyon', n: 'canyon trench', min: 0, max: 1, step: 0.05, def: 0, d: 'A narrow trench cut down from the floor, deeper here, shallower there: a keyhole section (water once ran in it).'},
  {k: 'pockets', n: 'side pockets', min: 0, max: 8, step: 1, int: true, def: 0, d: 'Small chambers off the walls at seeded points.'},
  {k: 'breakdown', n: 'breakdown', min: 0, max: 1, step: 0.05, def: 0, d: 'Fallen blocks of roof on the flattened floor.'}
];

const TUNNEL = {
  type: 'tunnel', label: 'Tunnel', version: 1, passage: true,
  size: {width: {min: 1, max: 12, def: 3, n: 'diameter (m)'}, length: {min: 3, max: 120, def: 16}, height: null, ceiling: null},
  u: {wallRough: 0.5, wallScale: 1.6, lightCount: 1},
  rows: ROWS_TUBE.concat([
    {k: 'rise', n: 'rise (m)', min: -20, max: 20, step: 0.1, def: 0, d: 'Height of the far socket over the near one (negative = descends).'},
    {k: 'endShift', n: 'end shift (m)', min: -20, max: 20, step: 0.1, def: 0, d: 'Sideways offset of the far socket.'}
  ], ROWS_FORMATIONS),
  PRESETS: {
    passage: {params: {}},
    winding_descent: {params: {length: 30, rise: -6, meander: 4, meanderFreq: 1.6, squeezes: 2, width: 2.6}},
    phreatic_tube: {params: {width: 4.5, radiusVar: 0.1, meander: 0.5, flatten: 0.2, squeezes: 0, wallRough: 0.25, stalactites: 0.05, stalagmites: 0}},
    rift: {params: {width: 2, radiusVar: 0.5, meander: 2.5, squeezes: 3, squeeze: 0.45, wallRough: 0.9, moisture: 0.6}},
    keyhole_canyon: {params: {width: 3.2, length: 24, meander: 2, wander: 2.5, wanderWave: 14, canyon: 0.7, xsec: 0.5, pockets: 2, breakdown: 0.4, radiusVar: 0.35, widthVar: 0.2, flatten: 0.7, lightPower: 0.5}},
    breakdown_crawlway: {params: {width: 3.6, length: 20, xsec: 0.8, breakdown: 0.9, pockets: 3, flatten: 0.8, radiusVar: 0.4, wander: 1.5, decay: 0.6}}
  },
  build(b, P) {
    const R = Math.round(b.m(P.width, 8) / 2), L = b.m(P.length, 20);
    const rise = b.m(P.rise), shift = b.m(P.endShift);
    const tp = tubePath(b, P, 'tun', L, R, {rise, shift});
    const {pts, radii, prim} = tp;
    tubeDressing(b, P, 'tun', tp);
    formations(b, P, prim);
    pts.forEach((p, i) => { if (i % 2 === 1) b.lightSpot([p[0], p[1] - radii[i] + 1, p[2]], 'floor', '+y'); });
    if ((P.lightDensity || 0) > 0) pts.forEach((p, i) => { if (i % 2 === 0 && i > 0 && i < pts.length - 1) b.lightSpot([p[0], tp.floorY(i) + 1, p[2]], 'floor', '+y'); });
    b.wayChain('w', pts.slice(1, -1).map((p, i) => [p[0], p[1] - radii[i + 1], p[2]]), ['socket:in'], ['socket:out']);
    b.socket('in', [0, 0, 0], '-z', 2 * R, 2 * R, 'round');
    b.socket('out', [shift, rise, L], '+z', 2 * R, 2 * R, 'round');
  }
};

const CRAWL = {
  type: 'crawl', label: 'Crawl', version: 1, passage: true,
  size: {width: {min: 0.8, max: 5, def: 1.6}, length: {min: 2, max: 40, def: 8}, height: {min: 0.6, max: 1.6, def: 1.1, d: 'A crawl is 1-1.5 m: the player has to crouch or go prone.'}, ceiling: null},
  u: {wallRough: 0.35, lightCount: 0, lightType: 'none', decay: 0.4},
  rows: [
    {sec: 'Crawl'},
    {k: 'meander', n: 'meander (m)', min: 0, max: 4, step: 0.1, def: 0.8, d: ''},
    {k: 'pinches', n: 'pinches', min: 0, max: 6, step: 1, int: true, def: 2, d: 'Segments squeezed lower.'},
    {k: 'pinch', n: 'pinch factor', min: 0.5, max: 1, step: 0.05, def: 0.75, d: 'Height multiplier at a pinch.'},
    {k: 'floorRubble', n: 'floor rubble', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Loose stuff underfoot.'},
    {k: 'rise', n: 'rise (m)', min: -6, max: 6, step: 0.1, def: 0, d: ''}
  ],
  PRESETS: {
    flat_out: {params: {}},
    belly_crawl: {params: {height: 0.7, width: 1.2, pinches: 3, pinch: 0.85, floorRubble: 0.8}},
    bedding_plane: {params: {width: 4, height: 0.9, meander: 0.3, pinches: 1, floorRubble: 0.2}},
    twisting_squeeze: {params: {width: 1.8, length: 10, wander: 1.4, wanderWave: 6, widthVar: 0.35, heightVar: 0.2, pinches: 3, rise: 1}}
  },
  build(b, P) {
    const W = b.m(P.width, 6), H = b.m(P.height, 6), L = b.m(P.length, 10), hw = Math.round(W / 2);
    const rise = b.m(P.rise), seg = b.m(1.5), n = Math.max(2, Math.ceil(L / seg));
    const amp = b.m(P.meander), ph = b.r('crawl', 'ph') * 6.283;
    const pinch = new Set();
    for (let k = 0; k < P.pinches; k++) pinch.add(1 + Math.floor(b.r('pinch', k) * Math.max(1, n - 2)));
    const pts = [];
    let first = -1;
    for (let i = 0; i < n; i++) {
      const z0 = Math.round((L * i) / n), z1 = Math.round((L * (i + 1)) / n);
      const t = (i + 0.5) / n, env = Math.sin(Math.PI * t);
      const end = i === 0 || i === n - 1;
      const zm = Math.round((z0 + z1) / 2);
      const x = end ? 0 : Math.round(amp * env * Math.sin(ph + t * 7)) + b.wanderX(zm, L, 'crawl');
      let h = pinch.has(i) && !end ? Math.max(5, Math.round(H * P.pinch)) : H;
      const sw = end ? hw : Math.max(3, Math.round(hw * b.varAt('widthVar', zm, L, 'crawl')));
      if (!end) h = Math.max(5, Math.round(h * b.varAt('heightVar', zm, L, 'crawl')));
      const y = Math.round(rise * t);
      const k = b.room('rbox', {min: [x - sw, y, z0 - 2], max: [x + sw, y + h, z1 + 2], r: Math.round(Math.min(2 * sw, h) / 3)}, {blend: Math.max(P.blend, 4)});
      if (first < 0) first = k;
      pts.push([x, y, zm]);
    }
    if (P.floorRubble > 0) b.scatter.push({prim: first, kind: 'rubble', density: +(P.floorRubble).toFixed(3), size: [1, 2], surface: 'floor', mat: 'rubble', seedKey: 'crawlfloor'});
    b.wayChain('w', pts, ['socket:in'], ['socket:out'], {crawl: true});
    b.socket('in', [0, 0, 0], '-z', W, H, 'rough');
    b.socket('out', [0, rise, L], '+z', W, H, 'rough');
  }
};

/** A cave room: an ellipsoid cluster in a W x L footprint, entered through a
 *  short tube from the origin. Returns frame info for the caller.
 *  D8: `slope` tilts the floor from the entrance to the back (the far exits
 *  rise or fall with it; a falling floor deepens the bowl), and yAt(z) is the
 *  floor under any point. */
function caveRoom(b, P, key) {
  const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 20), C = b.m(P.ceiling);
  const hw = Math.round(W / 2), mR = Math.round(b.m(P.mouth, 10) / 2), A = b.m(1.5);
  b.carve('capsule', {a: [0, mR, Math.min(mR, A)], b: [0, mR, A + mR], r: mR}, {tag: 'door', blend: Math.max(P.blend, 4)});
  const z0 = A, z1 = A + L, zc = Math.round((z0 + z1) / 2);
  const SR = b.slopeR();
  const D = b.m(1.5) + Math.max(0, -SR);
  const main = b.room('ellipsoid', {min: [-hw, -D, z0], max: [hw, H + C + Math.max(0, SR), z1]});
  const n = P.clusters || 0;
  const ells = [{min: [-hw, -D, z0], max: [hw, H + C + Math.max(0, SR), z1]}];
  for (let i = 0; i < n; i++) {
    const rx = Math.round(hw * (0.35 + 0.4 * b.r(key, 'cl', i, 'rx')));
    const rz = Math.round((L / 2) * (0.3 + 0.4 * b.r(key, 'cl', i, 'rz')));
    const ry = Math.round((H + C) * (0.35 + 0.35 * b.r(key, 'cl', i, 'ry')));
    const x = Math.round((b.r(key, 'cl', i, 'x') * 2 - 1) * (hw - rx * 0.5));
    const z = zc + Math.round((b.r(key, 'cl', i, 'z') * 2 - 1) * (L / 2 - rz * 0.5));
    const yb = -D + Math.max(0, b.slopeY(z - z0, L));
    b.room('ellipsoid', {min: [x - rx, yb, z - rz], max: [x + rx, yb + 2 * ry, z + rz]});
    ells.push({min: [x - rx, yb, z - rz], max: [x + rx, yb + 2 * ry, z + rz]});
  }
  /** The highest carved roof over column (x, z) among the room's ellipsoids
   *  (un-noised), or null where none covers it: what a hanging feature roots in. */
  const ceilAt = (x, z) => {
    let best = null;
    for (const e of ells) {
      const cx = (e.min[0] + e.max[0]) / 2, cz = (e.min[2] + e.max[2]) / 2, cy = (e.min[1] + e.max[1]) / 2;
      const ax = (e.max[0] - e.min[0]) / 2, az = (e.max[2] - e.min[2]) / 2, ay = (e.max[1] - e.min[1]) / 2;
      const q = 1 - ((x - cx) / ax) ** 2 - ((z - cz) / az) ** 2;
      if (q <= 0) continue;
      const y = cy + ay * Math.sqrt(q);
      if (best === null || y > best) best = y;
    }
    return best;
  };
  // Floor: fill the bowl up to a flat floor (flatten 1) or part way; a sloped
  // room's floor is a one-voxel-step ramp across it (open space only).
  const fy = -Math.round(b.m(1.5) * (1 - (P.flatten === undefined ? 0.8 : P.flatten)));
  const yAt = (z) => fy + b.slopeY(z - z0, L);
  if (!SR) b.fill('box', {min: [-hw - 4, -D - 4, z0], max: [hw + 4, fy, z1]}, {mat: 'floor', noise: 'floor', layer: 2, mask: 'air'});
  else if (P.slopeSteps > 0) {
    const ns = P.slopeSteps;
    for (let k = 0; k <= ns; k++) {
      const za = k === 0 ? z0 : Math.round(z0 + (L * (k - 0.5)) / ns), zb = k === ns ? z1 : Math.round(z0 + (L * (k + 0.5)) / ns);
      b.fill('box', {min: [-hw - 4, -D - 4, za], max: [hw + 4, fy + Math.round((SR * k) / ns), zb]}, {mat: 'floor', noise: 'floor', layer: 2, mask: 'air'});
    }
  } else b.flight('fill', SR > 0 ? '+z' : '-z', -hw - 4, hw + 4, z0, z1, fy + Math.min(0, SR), Math.abs(SR), 1, {below: D + 4}, {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
  // The main ellipsoid's footprint AT FLOOR HEIGHT is smaller than W x L:
  // floor features go through floorAt(u, v) (u, v in the unit disc) so they
  // land on the floor, not in the rock round it.
  const cy = (-D + H + C) / 2, hy = (H + C + D) / 2;
  const k = Math.sqrt(Math.max(0.05, 1 - ((fy + 0.5 - cy) / hy) ** 2));
  const kAt = (y) => Math.sqrt(Math.max(0.05, 1 - ((y + 0.5 - cy) / hy) ** 2));
  const floorAt = (u, v, margin) => {
    const r = Math.hypot(u, v), sc = r > 1 ? 1 / r : 1;
    const m = margin || 0;
    let kk = k;
    let z = zc + Math.round(v * sc * Math.max(1, (kk * L) / 2 - m));
    if (SR) {
      // a sloped floor that falls below the flat one meets a narrower bowl
      kk = Math.min(k, kAt(yAt(z)));
      z = zc + Math.round(v * sc * Math.max(1, (kk * L) / 2 - m));
    }
    return [Math.round(u * sc * Math.max(1, kk * hw - m)), yAt(z), z];
  };
  return {W, L, H, C, hw, mR, A, z0, z1, zc, D, fy, SR, yAt, main, k, floorAt, ceilAt};
}

/** FANGS: the big speleothems a scatter rule cannot make (a scatter spike is
 *  at most 3.2 m). Tapered tubes of the support material: stalactites rooted
 *  in the roof (the root is pushed past the carve's noise so it is never a
 *  floating island), stalagmites grown from the floor, and the odd pair that
 *  has met in the middle as a waisted column. Every fang is keyed by its index,
 *  so raising the count adds fangs and moves none. */
const ROWS_FANGS = [
  {sec: 'Fangs', note: 'Big tapered stalactites and stalagmites (prims, not scatter): the cave\'s teeth. Use them sparingly in big rooms; each one is a prim.'},
  {k: 'fangs', n: 'fangs', min: 0, max: 16, step: 1, int: true, def: 0, d: 'How many big speleothems.'},
  {k: 'fangLength', n: 'fang length (m)', min: 1, max: 20, step: 0.1, def: 5, d: 'Longest fang (each is 40-100% of it, and never more than the room allows).'},
  {k: 'fangGirth', n: 'fang girth (m)', min: 0.2, max: 3, step: 0.05, def: 0.8, d: 'Root radius of the biggest fang.'},
  {k: 'fangUp', n: 'from the floor', min: 0, max: 1, step: 0.05, def: 0.35, d: 'Share of the fangs that grow up from the floor (the rest hang).'},
  {k: 'fangJoin', n: 'joined columns', min: 0, max: 1, step: 0.05, def: 0.15, d: 'Share of the fangs that are a stalactite met by its stalagmite (a waisted column).'}
];
function fangs(b, P, f) {
  if (!P.fangs) return;
  const amp = (b.wallNoise() || {amp: 0}).amp;
  const Lmax = b.m(P.fangLength, 10), G = Math.max(3, b.m(P.fangGirth));
  for (let i = 0; i < P.fangs; i++) {
    const g = Math.max(3, Math.round(G * (0.5 + 0.5 * b.r('fang', i, 'g'))));
    const [x, fy, z] = f.floorAt(...disc(b, 'fang', i), g + 6);
    if (Math.abs(x) < f.mR + g + 6) continue;                      // keep the way through clear
    const roof = f.ceilAt(x, z);
    if (roof === null) continue;
    const span = Math.round(roof - fy);
    if (span < 12) continue;
    const len = Math.min(Math.round(Lmax * (0.4 + 0.6 * b.r('fang', i, 'len'))), Math.round(span * 0.8));
    const u = b.r('fang', i, 'kind');
    const lean = () => [b.jit(Math.round(g * 0.8), 'fang', i, 'lx'), b.jit(Math.round(g * 0.8), 'fang', i, 'lz')];
    const [lx, lz] = lean();
    const root = Math.round(roof + amp * 2 + 6);                    // buried past the roof's noise
    if (u < P.fangJoin) {
      // A column: wide root above, wide foot below, waisted where they met.
      const mid = Math.round(fy + span * (0.35 + 0.3 * b.r('fang', i, 'mid')));
      b.fill('spline_tube', {pts: [[x, root, z], [x + lx, mid, z + lz], [x, fy - 4, z]], radii: [g + 2, Math.max(2, Math.round(g * 0.4)), g + 3]}, {mat: 'support', layer: 2, noise: 0.25, tag: 'fang'});
    } else if (u < P.fangJoin + P.fangUp) {
      const top = fy + Math.min(len, Math.round(span * 0.6));
      b.fill('spline_tube', {pts: [[x, fy - 4, z], [x + Math.round(lx / 2), Math.round((fy + top) / 2), z + Math.round(lz / 2)], [x + lx, top, z + lz]], radii: [g + 2, Math.max(2, Math.round(g * 0.55)), 1]}, {mat: 'support', layer: 2, noise: 0.2, tag: 'fang'});
    } else {
      const tip = Math.round(roof - len);
      b.fill('spline_tube', {pts: [[x, root, z], [x + Math.round(lx / 2), Math.round((roof + tip) / 2), z + Math.round(lz / 2)], [x + lx, tip, z + lz]], radii: [g, Math.max(2, Math.round(g * 0.5)), 1]}, {mat: 'support', layer: 2, noise: 0.2, tag: 'fang'});
    }
  }
}
/** A seeded point in the unit disc (keyed). */
function disc(b, ...key) {
  const a = b.r(...key, 'a') * Math.PI * 2, r = Math.sqrt(b.r(...key, 'r'));
  return [Math.cos(a) * r, Math.sin(a) * r];
}

const ROOM_ROWS = [
  {sec: 'Shape'},
  {k: 'mouth', n: 'opening (m)', min: 1, max: 10, step: 0.1, def: 3, d: 'Diameter of the entry tube (the socket size).'},
  {k: 'clusters', n: 'lobes', min: 0, max: 8, step: 1, int: true, def: 3, d: 'Extra ellipsoids clustered into the room: more = lumpier, irregular.'},
  {k: 'flatten', n: 'floor flatten', min: 0, max: 1, step: 0.05, def: 0.8, d: 'Fill the bowl to a flat floor.'},
  {k: 'exits', n: 'exits', min: 0, max: 3, step: 1, int: true, def: 1, d: 'Sockets besides the origin: far end, then right, then left.'}
];

function roomExits(b, P, f, ways) {
  const out = [];
  const R = f.mR;
  const yo = f.yAt(f.z1) - f.fy, ys = f.yAt(f.zc) - f.fy;     // a sloped floor lifts the far exits
  const spots = [
    {name: 'out', y: yo, a: [0, R + yo, f.z1 - R], b: [0, R + yo, f.z1 + b.m(1.5)], pos: [0, yo, f.z1 + b.m(1.5) + R], dir: '+z'},
    {name: 'right', y: ys, a: [f.hw - R, R + ys, f.zc], b: [f.hw + b.m(1.5), R + ys, f.zc], pos: [f.hw + b.m(1.5) + R, ys, f.zc], dir: '+x'},
    {name: 'left', y: ys, a: [-f.hw + R, R + ys, f.zc], b: [-f.hw - b.m(1.5), R + ys, f.zc], pos: [-f.hw - b.m(1.5) - R, ys, f.zc], dir: '-x'}
  ];
  for (let i = 0; i < Math.min(3, P.exits); i++) {
    const s = spots[i];
    b.carve('capsule', {a: s.a, b: s.b, r: R}, {tag: 'door', blend: Math.max(P.blend, 4)});
    b.socket(s.name, s.pos, s.dir, 2 * R, 2 * R, 'rough');
    const wp = [Math.round((s.a[0] + s.b[0]) / 2), f.fy + s.y, Math.round((s.a[2] + s.b[2]) / 2)];
    b.way('x_' + s.name, wp, ['c', 'socket:' + s.name]);
    out.push('x_' + s.name);
  }
  b.way('door', [0, f.fy, Math.round(f.A / 2) + 2], ['socket:in', 'c']);
  b.way('c', [0, f.yAt(f.zc), f.zc], ['door'].concat(out, ways || []));
  b.socket('in', [0, 0, 0], '-z', 2 * R, 2 * R, 'rough');
}

/** D8 room extras shared by cavern and grotto: bays, the lobe, the key light
 *  and the density grid of light mounts. */
function caveRoomExtras(b, P, f, skip) {
  const doors = [];
  if (P.exits >= 2) doors.push({side: 1, z: f.zc, w: 2 * f.mR + 8});
  if (P.exits >= 3) doors.push({side: -1, z: f.zc, w: 2 * f.mR + 8});
  const fr = {x0: -f.hw, x1: f.hw, z0: f.z0, z1: f.z1, y: f.fy, H: f.H, roof: f.H + f.C, kind: 'cave', floorAt: f.yAt, doors};
  b.roomVariety(fr);
  b.keyLight(Object.assign({}, fr, {roof: f.ceilAt(0, f.zc) || f.H + f.C}));
  b.gridSpots({x0: -f.hw * f.k, x1: f.hw * f.k, z0: f.zc - (f.k * f.L) / 2, z1: f.zc + (f.k * f.L) / 2, y: f.fy, floorAt: f.yAt}, (x, z) => Math.abs(x) < f.mR + 4 || (skip ? skip(x, z) : false));
}

const CAVERN = {
  type: 'cavern', label: 'Cavern', version: 1, room: true, slope: true,
  size: {width: {min: 4, max: 60, def: 16}, length: {min: 4, max: 80, def: 20}, height: {min: 2, max: 40, def: 6, n: 'wall height (m)'},
         ceiling: {min: 0, max: 30, def: 4, n: 'dome height (m)', d: 'Extra rise of the domed roof.'}},
  u: {wallRough: 0.6, wallScale: 2.5, lightCount: 3, moisture: 0.35},
  rows: ROOM_ROWS.concat(ROWS_FORMATIONS, [
    {sec: 'Features'},
    {k: 'columns', n: 'columns', min: 0, max: 10, step: 1, int: true, def: 1, d: 'Floor-to-roof columns (a stalactite met its stalagmite).'},
    {k: 'flowstone', n: 'flowstone', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Draped lumps down the walls (trim material).'},
    {k: 'crystals', n: 'crystals', min: 0, max: 1, step: 0.05, def: 0.15, d: 'Crystal scatter on the walls + crystal cluster props.'},
    {k: 'mushrooms', n: 'mushroom patches', min: 0, max: 10, step: 1, int: true, def: 2, d: 'Glowing mushroom patches (props; glowcaps are lights).'},
    {k: 'pool', n: 'pool', min: 0, max: 1, step: 0.05, def: 0.25, d: 'Size of a still pool in a floor dip (0 = none).'},
    {k: 'boulders', n: 'breakdown blocks', min: 0, max: 10, step: 1, int: true, def: 2, d: 'Fallen roof blocks on the floor.'}
  ], ROWS_FANGS),
  PRESETS: {
    chamber: {params: {}},
    great_dome: {params: {width: 36, length: 40, height: 10, ceiling: 16, clusters: 5, columns: 4, stalactites: 0.5, flowstone: 0.6, lightCount: 6, exits: 3}},
    crystal_geode: {params: {width: 12, length: 14, height: 5, ceiling: 6, clusters: 1, crystals: 1, lightType: 'crystal', lightCount: 6, flowstone: 0, pool: 0}},
    fungal_grotto: {params: {mushrooms: 8, moisture: 0.8, pool: 0.4, lightType: 'fungus', lightCount: 5, stalagmites: 0.05}},
    breakdown_hall: {params: {width: 24, length: 30, boulders: 10, decay: 0.9, flatten: 0.4, columns: 0, ceiling: 7}},
    fanged_hall: {params: {width: 34, length: 40, height: 9, ceiling: 14, clusters: 4, columns: 1, stalactites: 0.6, stalLength: 2.4, flowstone: 0.5, fangs: 10, fangLength: 9, fangGirth: 1.4, lightCount: 5, exits: 3}},
    lit_shaft_chamber: {params: {width: 26, length: 30, height: 8, ceiling: 10, clusters: 5, slope: 2.5, bays: 3, bayDepth: 4, lobe: 0.5, keyLight: 'shaft', keySize: 0.5, keyHeight: 25, keyAt: 0.55,
                                 boulders: 6, fangs: 5, fangLength: 6, lightType: 'fungus', lightPower: 0.7, lightDensity: 0.4, exits: 2}},
    tilted_breakdown: {params: {width: 22, length: 26, height: 6, ceiling: 6, clusters: 6, slope: -3, slopeSteps: 3, bays: 2, lobe: 0.35, boulders: 9, decay: 0.8,
                                keyLight: 'crystals', keySize: 0.7, keyAt: 0.8, keySide: -0.4, lightType: 'crystal', lightPower: 0.6}}
  },
  build(b, P) {
    const f = caveRoom(b, P, 'cav');
    formations(b, P, f.main);
    fangs(b, P, f);
    for (let i = 0; i < P.columns; i++) {
      const r = b.ri(b.m(0.4), b.m(1.2), 'col', i, 'r');
      const [x, y, z] = f.floorAt(...disc(b, 'col', i), r * 2);
      if (Math.abs(x) < f.mR + r + 4) continue;              // keep the axis clear for the way through
      b.fill('capsule', {a: [x, y - 2, z], b: [x, f.H + f.C + Math.max(0, f.SR), z], r}, {mat: 'support', layer: 2, noise: 0.35});
    }
    const nFlow = Math.round(P.flowstone * 8);
    for (let i = 0; i < nFlow; i++) {
      const side = b.r('flow', i, 'side') < 0.5 ? -1 : 1, r = b.ri(b.m(0.8), b.m(2.2), 'flow', i, 'r');
      const z = b.ri(f.z0 + r, f.z1 - r, 'flow', i, 'z');
      const x = Math.round(side * f.hw * 0.85), y = f.yAt(z);
      b.fill('ellipsoid', {min: [x - r, y, z - r], max: [x + r, y + Math.round(r * 2.5), z + r]}, {mat: 'trim', layer: 2, noise: 0.6});
    }
    if (P.crystals > 0) b.scatter.push({prim: f.main, kind: 'crystal', density: +(P.crystals * 0.6).toFixed(3), size: [2, 5 + Math.round(P.crystals * 8)], surface: 'wall', mat: 'accent', seedKey: 'crystal'});
    const rough = (b.floorNoise() || {amp: 0}).amp >= 1;
    for (let i = 0; i < Math.round(P.crystals * 4); i++) {
      const at = f.floorAt(...disc(b, 'cc', i), 8);
      b.prop('caves/crystal_cluster', {size: 0.6 + P.crystals}, at, b.ri(0, 3, 'cc', i, 'yaw') * 90);
      if (rough) b.footing(at, 3);
    }
    for (let i = 0; i < P.mushrooms; i++) {
      const at = f.floorAt(...disc(b, 'mush', i), 8);
      b.prop('caves/glow_mushroom_patch', {}, at, b.ri(0, 3, 'mush', i, 'yaw') * 90, {light: true});
      if (rough) b.footing(at, 2);
    }
    for (let i = 0; i < P.boulders; i++) {
      const r = b.ri(4, 12, 'bld', i, 'r');
      const [x, y, z] = f.floorAt(...disc(b, 'bld', i), r);
      b.fill('ellipsoid', {min: [x - r, y - 2, z - Math.round(r * 1.2)], max: [x + r, y + Math.round(r * 1.3), z + Math.round(r * 1.2)]}, {mat: 'loose', layer: 4, noise: 0.4, mask: 'air'});
    }
    if (P.pool > 0) {
      const pr = Math.round(Math.min(f.hw, f.L / 2) * 0.6 * P.pool) + 4, pd = Math.max(4, Math.round(pr * 0.35));
      const [px, py, pz] = f.floorAt(...disc(b, 'pool'), pr);
      b.carve('ellipsoid', {min: [px - pr, py - pd, pz - pr], max: [px + pr, py + pd, pz + pr]}, {layer: 3, noise: 0.25, tag: 'room', liquid: 'liquid', level: py - 2});
    }
    for (let i = 0; i < 8; i++) {
      const a = (i / 8) * Math.PI * 2;
      b.lightSpot(f.floorAt(Math.sin(a) * 0.8, Math.cos(a) * 0.8, 4), 'floor', '+y');
    }
    roomExits(b, P, f);
    caveRoomExtras(b, P, f);
  }
};

const GROTTO = {
  type: 'grotto_pool', label: 'Grotto pool', version: 1, room: true,
  size: {width: {min: 4, max: 40, def: 12}, length: {min: 4, max: 40, def: 14}, height: {min: 2, max: 20, def: 4}, ceiling: {min: 0, max: 15, def: 3}},
  u: {moisture: 0.9, lightCount: 4, lightType: 'fungus', wallRough: 0.5},
  rows: ROOM_ROWS.concat(ROWS_FORMATIONS, [
    {sec: 'Pool'},
    {k: 'poolSize', n: 'pool size', min: 0.2, max: 1, step: 0.05, def: 0.65, d: 'Fraction of the floor under water.'},
    {k: 'poolDepth', n: 'pool depth (m)', min: 0.3, max: 10, step: 0.1, def: 1.8, d: ''},
    {k: 'waterLevel', n: 'water level', min: 0.1, max: 1, step: 0.05, def: 0.9, d: 'Fraction of the basin filled (born flat, capped below the floor).'},
    {k: 'beach', n: 'beach', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Sand on the shore (the beach slot).'},
    {k: 'island', n: 'island', kind: 'bool', def: false, d: 'A rock island in the middle.'},
    {k: 'glow', n: 'glow fungus', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Glowing fungus scatter on the walls.'}
  ], ROWS_FANGS),
  PRESETS: {
    still_pool: {params: {}},
    underground_lake: {params: {width: 30, length: 36, height: 6, ceiling: 8, poolSize: 0.9, poolDepth: 5, glow: 0.3, exits: 2, beach: 0.1}},
    glow_grotto: {params: {glow: 1, poolSize: 0.4, island: true, mushrooms: 0, lightCount: 6, stalactites: 0.6}},
    sea_pool: {params: {poolSize: 0.8, poolDepth: 3, beach: 1, glow: 0.1, lightType: 'none', lightCount: 0}},
    sunlit_pool: {params: {width: 18, length: 20, height: 5, ceiling: 7, clusters: 4, bays: 2, lobe: 0.45, keyLight: 'shaft', keyHeight: 20, keySize: 0.6, keyAt: 0.6, glow: 0.6, lightPower: 0.6}}
  },
  build(b, P) {
    const f = caveRoom(b, P, 'grot');
    formations(b, P, f.main);
    fangs(b, P, f);
    const pr = Math.round(Math.min(f.hw, f.L / 2) * P.poolSize), pd = b.m(P.poolDepth, 3);
    const pz = f.zc + Math.round(f.L * 0.1);
    const lvl = Math.min(f.fy - 1, f.fy - pd + Math.max(1, Math.round(P.waterLevel * pd)));
    b.carve('ellipsoid', {min: [-pr, f.fy - pd, pz - pr], max: [pr, f.fy + Math.round(pd / 2), pz + pr]}, {layer: 3, noise: 0.3, tag: 'room', liquid: 'liquid', level: lvl - 1});
    if (P.beach > 0) b.mound(0, f.fy, pz - pr, Math.round(f.hw * 0.7), b.m(1.5 + 2 * P.beach), b.m(0.3), {mat: 'beach'});
    if (P.island) b.fill('ellipsoid', {min: [-b.m(1.5), f.fy - pd, pz - b.m(1.5)], max: [b.m(1.5), f.fy + b.m(0.6), pz + b.m(1.5)]}, {mat: 'wall', layer: 6, noise: 0.5});
    if (P.glow > 0) b.scatter.push({prim: f.main, kind: 'moss', density: +(P.glow * 0.9).toFixed(3), size: [1, 4], surface: 'wall', mat: 'fungus', seedKey: 'glow'});
    for (let i = 0; i < 8; i++) {
      const a = (i / 8) * Math.PI * 2;
      b.lightSpot(f.floorAt(Math.sin(a) * 0.85, Math.cos(a) * 0.85, 3), 'floor', '+y');
    }
    const shore = [0, f.fy, Math.max(f.z0 + 4, pz - pr - 6)];
    b.way('shore', shore, ['c'], {});
    roomExits(b, P, f, ['shore']);
    caveRoomExtras(b, P, f, (x, z) => Math.hypot(x, z - pz) < pr + 6);
  }
};
const MOUTH = {
  type: 'mouth', label: 'Cave mouth', version: 1, surface: true,
  size: {width: {min: 1.5, max: 30, def: 5}, length: {min: 2, max: 40, def: 10, n: 'depth into the hill (m)'}, height: {min: 1.5, max: 20, def: 4}, ceiling: null},
  u: {wallRough: 0.6, lightCount: 0, lightType: 'none', moisture: 0.4},
  rows: [
    {sec: 'Mouth', note: 'The anchor (origin socket) is the mouth at grade; the cave runs +Z into the hill (or down, for a sinkhole).'},
    {k: 'variant', n: 'variant', kind: 'enum', options: ['hillside', 'sinkhole', 'seacave'], def: 'hillside',
     d: 'hillside = an overhung mouth in a slope; sinkhole = a collapse funnel from the surface down to a side passage; seacave = a wide low arch at the waterline (flooded: true).'},
    {k: 'flare', n: 'flare', min: 1, max: 3, step: 0.05, def: 1.6, d: 'How much bigger the outer mouth is than the inner passage.'},
    {k: 'drop', n: 'drop (m)', min: 0, max: 30, step: 0.1, def: 1.5, d: 'Height lost from the mouth to the inner socket.'},
    {k: 'innerWidth', n: 'inner diameter (m)', min: 1, max: 10, step: 0.1, def: 3, d: 'Size of the inner socket.'},
    {k: 'lip', n: 'vegetation lip', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Ferns on the sill and hanging moss from the brow.'},
    {k: 'apron', n: 'apron (m)', min: 0, max: 15, step: 0.1, def: 4, d: 'Ground outside the mouth that is levelled and blended into the terrain.'},
    {k: 'talus', n: 'talus', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Scree heaped at the foot of the brow.'}
  ],
  PRESETS: {
    hillside: {params: {}},
    sinkhole: {params: {variant: 'sinkhole', width: 10, drop: 12, flare: 2.2, apron: 2, lip: 0.9, talus: 0.6}},
    sea_cave: {params: {variant: 'seacave', width: 14, height: 5, flare: 1.4, drop: 0.5, innerWidth: 4, lip: 0.2, apron: 6}},
    low_overhang: {params: {width: 8, height: 2.2, flare: 2.4, length: 6, innerWidth: 2, talus: 0.8}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 10), L = b.m(P.length, 10), drop = b.m(P.drop);
    const Ri = Math.round(b.m(P.innerWidth, 8) / 2), A = b.m(P.apron);
    const hw = Math.round(W / 2);
    b.flooded = P.variant === 'seacave';
    if (P.variant === 'sinkhole') {
      // A funnel: wide bowl at grade narrowing into a shaft, then a side passage.
      const Rt = Math.round(hw * P.flare), D = Math.max(drop, b.m(4));
      const bowl = b.room('ellipsoid', {min: [-Rt, -Math.round(D * 0.5), -Rt], max: [Rt, Math.round(D * 0.5), Rt]}, {noise: 1.2});
      b.room('spline_tube', {pts: [[0, 0, 0], [b.jit(6, 'sk1'), -Math.round(D * 0.5), b.jit(6, 'sk2')], [0, -D + Ri, Math.round(Ri / 2)]], radii: [hw, Math.round((hw + Ri) / 2), Ri]});
      b.room('capsule', {a: [0, -D + Ri, Ri], b: [0, -D + Ri, Ri + L], r: Ri});
      b.fill('box', {min: [-Ri - 4, -D - 6, -Ri - 4], max: [Ri + 4, -D, Ri + L]}, {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
      if (P.talus > 0) b.mound(0, -D, 0, Ri + 6, Ri + 6, Math.round(P.talus * Ri), {mat: 'rubble'});
      if (P.lip > 0) b.scatter.push({prim: bowl, kind: 'moss', density: +(P.lip * 0.9).toFixed(3), size: [1, 4], surface: 'wall', mat: 'veg', seedKey: 'lip'});
      b.ladder('caves/rope_ladder', D / 10, [0, -D, -Ri + 2], 0);
      b.wayChain('w', [[0, 0, 0], [0, -D, Math.round(Ri / 2)], [0, -D, Ri + Math.round(L / 2)]], ['socket:surface'], ['socket:in'], {climb: true});
      b.socket('surface', [0, 0, 0], '+y', 2 * Rt, 2 * Rt, 'rough', ['surface']);
      b.socket('in', [0, -D, Ri + L + Ri], '+z', 2 * Ri, 2 * Ri, 'round');
      b.lightSpot([0, -D, Ri + Math.round(L / 2)], 'floor', '+y');
      return;
    }
    // Hillside / sea cave: a flared tube from the mouth, plus an overhung brow.
    const Ro = Math.round(Math.max(hw, Ri) * P.flare);
    const seaLow = P.variant === 'seacave';
    const outerH = seaLow ? Math.round(H * 0.8) : H;
    // The mouth arch: a flattened ellipsoid half above grade (the brow is where the hill meets it).
    const arch = b.room('ellipsoid', {min: [-Ro, -b.m(1), -Ro], max: [Ro, outerH + Math.round(outerH * 0.3), Ro + Math.round(L * 0.3)]}, {noise: 1.1});
    const n = 5, pts = [], radii = [];
    for (let i = 0; i < n; i++) {
      const t = i / (n - 1);
      const r = Math.round(Ro + (Ri - Ro) * t);
      // The tube is round: cap it by the mouth height (the arch ellipsoid gives the width).
      const rr = Math.max(Ri, Math.min(seaLow ? Math.round(r * 0.7) : r, Math.round(outerH / 2)));
      pts.push([b.jit(4, 'mw', i) * (i > 0 && i < n - 1 ? 1 : 0), Math.round(-drop * t) + rr, Math.round(L * t)]);
      radii.push(rr);
    }
    pts[n - 1][1] = -drop + Ri; radii[n - 1] = Ri;
    const tube = b.room('spline_tube', {pts, radii});
    b.fill('spline_tube', {pts: pts.map((p, i) => [p[0], p[1] - Math.round(radii[i] * 1.7), p[2]]), radii}, {mat: seaLow ? 'beach' : 'floor', layer: 2, noise: 'floor', mask: 'air'});
    // Apron: clear the ground above grade in front of the mouth and fill the hollows under it.
    if (A > 0) {
      b.carve('box', {min: [-Ro - 4, 0, -A], max: [Ro + 4, outerH, 1]}, {layer: 1, noise: 0.5, blend: 10, tag: 'apron'});
      b.fill('box', {min: [-Ro - 4, -b.m(1.5), -A], max: [Ro + 4, 0, Ro]}, {mat: seaLow ? 'beach' : 'soil', layer: 2, noise: 'floor', mask: 'air'});
    }
    if (P.talus > 0) b.mound(b.jit(Ro, 'talusx'), 0, Math.round(Ro * 0.5), Ro, Math.round(Ro * 0.6), Math.round(Ro * 0.5 * P.talus), {mat: 'rubble'});
    if (P.lip > 0) {
      b.scatter.push({prim: arch, kind: 'moss', density: +(P.lip * 0.9).toFixed(3), size: [1, 4], surface: 'floor', mat: 'veg', seedKey: 'lip'});
      b.scatter.push({prim: arch, kind: 'moss', density: +(P.lip * 0.6).toFixed(3), size: [2, 8], surface: 'ceil', mat: 'hang', seedKey: 'brow'});
    }
    if (seaLow) b.scatter.push({prim: tube, kind: 'puddle', density: 0.4, size: [3, 8], surface: 'floor', mat: 'liquid', seedKey: 'tidepool'});
    b.lightSpot([Math.round(Ro * 0.6), 0, Math.round(Ro * 0.4)], 'floor', '+y');
    b.lightSpot([0, -drop, L - Ri], 'floor', '+y');
    b.wayChain('w', [[0, 0, -Math.max(2, Math.round(A / 2))], [0, 0, Math.round(Ro * 0.5)], [0, Math.round(-drop / 2), Math.round(L / 2)], [0, -drop, L - Ri]], ['socket:surface'], ['socket:in']);
    b.socket('surface', [0, 0, 0], '-z', 2 * Ro, outerH, 'rough', ['surface']);
    b.socket('in', [0, -drop, L], '+z', 2 * Ri, 2 * Ri, 'round');
  }
};

const SHAFT = {
  type: 'shaft', label: 'Shaft / pit', version: 1,
  size: {width: {min: 1, max: 20, def: 3, n: 'diameter (m)'}, length: {min: 2, max: 150, def: 12, n: 'depth (m)'}, height: {min: 1.5, max: 10, def: 3, n: 'foot chamber height (m)'}, ceiling: null},
  u: {wallRough: 0.6, lightCount: 1},
  rows: [
    {sec: 'Shaft'},
    {k: 'wander', n: 'wander (m)', min: 0, max: 8, step: 0.1, def: 1, d: 'How far the shaft drifts sideways on the way down.'},
    {k: 'ledges', n: 'ledges', min: 0, max: 8, step: 1, int: true, def: 2, d: 'Rock shelves sticking out of the wall.'},
    {k: 'ledgeWidth', n: 'ledge width (m)', min: 0.3, max: 3, step: 0.1, def: 0.8, d: ''},
    {k: 'rope', n: 'rope ladder', kind: 'bool', def: true, d: 'A rope ladder down from the lip (climbable in the nav graph; without one the drop is one-way).'},
    {k: 'surfaceTop', n: 'opens at the surface', kind: 'bool', def: false, d: 'A pothole: the top is at grade (an entrance piece).'},
    {k: 'footPool', n: 'plunge pool (m)', min: 0, max: 4, step: 0.1, def: 0, d: 'Water at the bottom.'},
    {sec: 'Entry', note: 'top = the shaft is entered from above (a vertical socket: a pothole, or a floor opening). side = a PITCH: a short passage at floor level ends at a lip and the floor drops away -- the origin socket is horizontal, so the shaft hangs off any passage or room exit.'},
    {k: 'entry', n: 'entry', kind: 'enum', options: ['top', 'side'], def: 'top', d: 'Where the shaft is entered (see the note).'},
    {k: 'lipLength', n: 'lip passage (m)', min: 0.5, max: 12, step: 0.1, def: 3, d: 'side: length of the approach passage before the drop.'},
    {k: 'lipWidth', n: 'lip passage width (m)', min: 1.2, max: 8, step: 0.1, def: 3, d: 'side: width (and height) of the approach passage: the origin socket\'s size.'},
    {k: 'lipBrow', n: 'brow over the drop (m)', min: 0, max: 8, step: 0.1, def: 1.5, d: 'side: how far the roof of the shaft head rises above the lip (a dome over the drop).'}
  ],
  PRESETS: {
    pitch: {params: {}},
    deep_pothole: {params: {length: 40, width: 5, wander: 3, ledges: 5, surfaceTop: true, lightCount: 0}},
    narrow_drop: {params: {width: 1.6, length: 8, ledges: 0, rope: false}},
    plunge: {params: {length: 20, footPool: 2.5, moisture: 0.9, ledges: 3, width: 4}},
    lip_pitch: {params: {entry: 'side', length: 18, width: 4, wander: 1.5, ledges: 3, lipLength: 4, lipWidth: 2.8, lipBrow: 2.5}}
  },
  build(b, P) {
    const R = Math.round(b.m(P.width, 6) / 2), D = b.m(P.length, 10), Hf = b.m(P.height, 10), wand = b.m(P.wander);
    const side = P.entry === 'side';
    b.surface = P.surfaceTop && !side;
    // side entry: the whole shaft moves +Z past an approach passage (oz); a top
    // entry is the original piece, bit for bit (oz = 0, no extra prims).
    const Lp = side ? b.m(P.lipLength, 4) : 0, Wp = side ? b.m(P.lipWidth, 12) : 0;
    const oz = side ? Lp + R : 0;
    const Z = (p) => [p[0], p[1], p[2] + oz];
    const n = Math.max(3, Math.round(D / b.m(3)) + 1);
    const pts = [], radii = [];
    for (let i = 0; i < n; i++) {
      const t = i / (n - 1);
      const env = Math.sin(Math.PI * t);
      pts.push([Math.round(wand * env * (b.r('sw', i, 'x') * 2 - 1)), Math.round(-D * t), Math.round(wand * env * (b.r('sw', i, 'z') * 2 - 1)) + oz]);
      radii.push(i === 0 ? R : Math.round(R * (0.85 + 0.3 * b.r('sw', i, 'r'))));
    }
    if (side) {
      // The head: the first point lifts by the brow so the roof domes over the
      // drop (a round tube's top at y = 0 would leave a lip of rock at head height).
      const brow = b.m(P.lipBrow);
      pts.unshift([0, Math.max(brow, Math.round(Wp / 2)), oz]);
      radii.unshift(R);
    }
    const shaft = b.room('spline_tube', {pts, radii});
    if (side) {
      // The approach: a rough passage, floor at y = 0, from the socket to the lip.
      const hw = Math.round(Wp / 2);
      b.room('rbox', {min: [-hw, 0, -2], max: [hw, Wp, Lp + Math.round(R / 2)], r: Math.round(Wp / 3)}, {blend: Math.max(P.blend, 6)});
      b.floor(-hw - 2, -2, hw + 2, Lp, 0);
    }
    // The foot chamber and its side exit.
    const fc = b.room('ellipsoid', {min: Z([-R - 6, -D - b.m(0.5), -R - 6]), max: Z([R + 6, -D + Hf, R + 6 + b.m(2)])});
    b.fill('box', {min: Z([-R - 10, -D - b.m(1), -R - 10]), max: Z([R + 10, -D, R + 10 + b.m(4)])}, {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
    const Rx = Math.max(Math.round(b.m(1.4)), Math.min(R, b.m(1.5)));
    b.carve('capsule', {a: Z([0, -D + Rx, R]), b: Z([0, -D + Rx, R + 6 + b.m(3)]), r: Rx}, {tag: 'door', blend: Math.max(P.blend, 4)});
    if (P.footPool > 0) {
      const pd = b.m(P.footPool);
      b.carve('ellipsoid', {min: Z([-R, -D - pd, -R]), max: Z([R, -D + 2, R])}, {layer: 3, noise: 0.3, tag: 'room', liquid: 'liquid', level: -D - 2});
    }
    const p0 = side ? 1 : 0;           // pts index of the lip (side: pts[0] is the brow)
    for (let k = 0; k < P.ledges; k++) {
      const i = p0 + 1 + Math.floor(b.r('ledge', k) * (n - 2));
      const p = pts[i], r = radii[i], lw = b.m(P.ledgeWidth, 3);
      const dir = b.ri(0, 3, 'ledge', k, 'dir');
      const [dx, dz] = [[1, 0], [-1, 0], [0, 1], [0, -1]][dir];
      const cx = p[0] + dx * (r - Math.round(lw / 2)), cz = p[2] + dz * (r - Math.round(lw / 2));
      b.fill('ellipsoid', {min: [cx - lw - (dz ? b.m(1) : 0), p[1] - 4, cz - lw - (dx ? b.m(1) : 0)], max: [cx + lw + (dz ? b.m(1) : 0), p[1], cz + lw + (dx ? b.m(1) : 0)]}, {mat: 'wall', layer: 2, noise: 0.4});
    }
    formations(b, Object.assign({}, P, {stalactites: 0, stalagmites: 0.3, stalLength: 0.8}), fc);
    // side: the rope hangs from the lip, a third of the way out over the drop
    // (clear of the near wall's wander, which is up to `wander` on each axis).
    if (P.rope) b.ladder('caves/rope_ladder', P.length, side ? [0, -D, oz - R + Math.max(2, Math.round(R * 0.35))] : [pts[0][0], -D, pts[0][2] - R + 2], 0);
    if (P.surfaceTop && !side) b.carve('ellipsoid', {min: [-R * 2, -R, -R * 2], max: [R * 2, R * 2, R * 2]}, {layer: 1, noise: 1.2, blend: 8, tag: 'apron'});
    b.lightSpot(Z([0, -D, R + 4]), 'floor', '+y');
    b.lightSpot(Z([R - 2, Math.round(-D / 2), 0]), 'wall', '-x');
    if (side) {
      b.lightSpot([Math.round(Wp / 2) - 2, 0, Math.round(Lp / 2)], 'floor', '+y');
      b.way('lip', [0, 0, Math.max(2, Lp - 4)], ['socket:in', 'top']);
      b.way('top', [0, 0, oz - R + 2], ['lip', 'foot'], {climb: true, ladder: P.rope});
    } else b.way('top', [0, 0, 0], ['socket:top', 'foot'], {climb: true, ladder: P.rope});
    b.way('foot', Z([0, -D, 0]), ['top', 'exit'], {climb: true, ladder: P.rope});
    b.way('exit', Z([0, -D, R + 6 + b.m(2)]), ['foot', 'socket:foot']);
    if (side) b.socket('in', [0, 0, 0], '-z', Wp, Wp, 'rough');
    else b.socket('top', [0, 0, 0], '+y', 2 * R, 2 * R, 'shaft', P.surfaceTop ? ['surface'] : []);
    b.socket('foot', Z([0, -D, R + 6 + b.m(3) + Rx]), '+z', 2 * Rx, 2 * Rx, 'round');
    void shaft;
  }
};

const CHASM = {
  type: 'chasm', label: 'Chasm', version: 1,
  size: {width: {min: 1.5, max: 12, def: 3, n: 'passage width (m)'}, length: {min: 4, max: 60, def: 14}, height: {min: 2, max: 20, def: 4}, ceiling: {min: 0, max: 20, def: 3}},
  u: {wallRough: 0.6, lightCount: 1},
  rows: [
    {sec: 'Crack', note: 'A long crack crosses the passage at its middle; the way across is a bridge, or nothing.'},
    {k: 'crackWidth', n: 'crack width (m)', min: 1, max: 20, step: 0.1, def: 4, d: 'Gap to cross.'},
    {k: 'crackLength', n: 'crack length (m)', min: 4, max: 80, step: 0.1, def: 24, d: 'Along the crack (across the passage).'},
    {k: 'crackDepth', n: 'crack depth (m)', min: 2, max: 100, step: 0.1, def: 15, d: 'Below the passage floor.'},
    {k: 'crackAngle', n: 'crack skew', min: -1, max: 1, step: 0.05, def: 0.2, d: 'How much the crack wanders off square (lobes along it).'},
    {k: 'bridge', n: 'bridge', kind: 'enum', options: ['natural', 'rope', 'none'], def: 'natural', d: 'natural = a rock arch; rope = planks on ropes (burns); none = no way across (the validator reports it).'},
    {k: 'bridgeWidth', n: 'bridge width (m)', min: 0.6, max: 5, step: 0.1, def: 1.6, d: ''},
    {k: 'fogDrop', n: 'water at the bottom', kind: 'bool', def: false, d: 'A stream pooled at the bottom of the crack.'}
  ],
  PRESETS: {
    rock_bridge: {params: {}},
    rope_span: {params: {bridge: 'rope', crackWidth: 8, crackDepth: 30, bridgeWidth: 1, lightCount: 2}},
    abyss: {params: {bridge: 'none', crackWidth: 3, crackLength: 50, crackDepth: 80}},
    wide_rift: {params: {width: 6, crackWidth: 12, crackLength: 40, bridge: 'natural', bridgeWidth: 3, ceiling: 12, fogDrop: true}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), L = b.m(P.length, 30), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), CW = b.m(P.crackWidth, 6), CL = b.m(P.crackLength, 30), CD = b.m(P.crackDepth, 10);
    const zc = Math.round(L / 2), cw = Math.round(CW / 2);
    b.room('rbox', {min: [-hw, 0, 0], max: [hw, H, L], r: Math.round(Math.min(W, H) / 3)});
    if (C > 2) b.room('ellipsoid', {min: [-hw, H - C, 0], max: [hw, H + C, L]});
    b.floor(-hw, 0, hw, L, 0);
    // The crack: a few lobes along X, skewed in Z.
    const lobes = Math.max(2, Math.round(CL / b.m(6)));
    let crack = -1;
    for (let i = 0; i < lobes; i++) {
      const t = (i + 0.5) / lobes - 0.5;
      const x = Math.round(t * CL), z = zc + Math.round(t * CL * 0.3 * P.crackAngle) + b.jit(4, 'lobe', i);
      const lw = Math.round(cw * (0.8 + 0.4 * b.r('lobe', i, 'w')));
      const wet = P.fogDrop ? {liquid: 'liquid', level: -CD + b.m(1) - 1} : {};
      const k = b.carve('rbox', {min: [x - Math.round(CL / lobes / 2) - 4, -CD, z - lw], max: [x + Math.round(CL / lobes / 2) + 4, H + C + b.m(2), z + lw], r: Math.max(1, Math.round(lw / 2))}, Object.assign({layer: 3, noise: 1.3, tag: 'room'}, wet));
      if (crack < 0) crack = k;
    }
    const bw = Math.round(b.m(P.bridgeWidth, 4) / 2);
    let cross = true;
    if (P.bridge === 'natural') {
      b.fill('box', {min: [-bw - 2, -b.m(1.2), zc - cw - 6], max: [bw + 2, 0, zc + cw + 6]}, {mat: 'wall', layer: 4, noise: 0.5});
      b.carve('vault', {min: [-bw - 4, -CD, zc - cw + 2], max: [bw + 4, -b.m(1.2), zc + cw - 2], axis: 'x', rise: Math.max(2, Math.min(cw - 2, CD))}, {layer: 5, noise: 0.4, blend: 0, tag: 'detail'});
    } else if (P.bridge === 'rope') {
      b.fill('box', {min: [-bw, -1, zc - cw - 4], max: [bw, 0, zc + cw + 4]}, {mat: 'plank', layer: 4, noise: null});
      for (const s of [-1, 1]) {
        b.fill('box', {min: [s < 0 ? -bw - 1 : bw, b.m(0.9), zc - cw - 4], max: [s < 0 ? -bw : bw + 1, b.m(0.9) + 1, zc + cw + 4]}, {mat: 'cloth', layer: 4, noise: null});
        for (const z of [zc - cw - 4, zc + cw + 3]) b.fill('box', {min: [s < 0 ? -bw - 2 : bw, 0, z], max: [s < 0 ? -bw : bw + 2, b.m(1.1), z + 2]}, {mat: 'timber', layer: 4, noise: null});
      }
    } else cross = false;
    // Inset by the room's corner rounding: a spot in the rounded floor corner is in rock.
    const ins = Math.min(hw - 1, Math.round(Math.min(W, H) / 3) + 2);
    b.lightSpot([-hw + ins, 0, zc - cw - 6], 'floor', '+y');
    b.lightSpot([hw - ins, 0, zc + cw + 6], 'floor', '+y');
    if (cross) b.wayChain('w', [[0, 0, 3], [0, 0, zc - cw - 3], [0, 0, zc], [0, 0, zc + cw + 3], [0, 0, L - 3]], ['socket:in'], ['socket:out']);
    else { b.wayChain('n', [[0, 0, 3], [0, 0, zc - cw - 3]], ['socket:in'], []); b.wayChain('f', [[0, 0, zc + cw + 3], [0, 0, L - 3]], [], ['socket:out']); b.notes.push('no bridge: the far side is unreachable on foot'); }
    b.way('deep', [0, -CD, zc], [], {pit: true});
    b.socket('in', [0, 0, 0], '-z', W, H, 'rough');
    b.socket('out', [0, 0, L], '+z', W, H, 'rough');
    void crack;
  }
};

const CHOKE = {
  type: 'rubble_choke', label: 'Rubble choke', version: 1,
  size: {width: {min: 1, max: 10, def: 3}, length: {min: 4, max: 40, def: 14}, height: {min: 1.5, max: 10, def: 3}, ceiling: null},
  u: {decay: 0.7, wallRough: 0.55, lightCount: 0, lightType: 'none'},
  rows: [
    {sec: 'Choke', note: 'Loose rubble at its angle of repose: a long passage can be choked high, a short one only partly. Dig through it.'},
    {k: 'choke', n: 'choke height', min: 0, max: 1, step: 0.05, def: 0.8, d: 'Height of the rubble relative to the passage (clamped by repose and length).'},
    {k: 'chokeLength', n: 'choke length', min: 0.2, max: 1, step: 0.05, def: 0.8, d: 'Fraction of the piece the rubble spreads over.'},
    {k: 'gap', n: 'squeeze gap (m)', min: 0, max: 1.5, step: 0.05, def: 0.5, d: 'A crawl-sized gap kept open under the roof (0 = dig).'},
    {k: 'boulders', n: 'boulders', min: 0, max: 12, step: 1, int: true, def: 4, d: 'Big blocks wedged in the rubble.'},
    {k: 'radiusVar', n: 'roof irregularity', min: 0, max: 0.6, step: 0.05, def: 0.2, d: ''}
  ],
  PRESETS: {
    squeeze: {params: {}},
    sealed: {params: {choke: 1, gap: 0, chokeLength: 1, length: 20, boulders: 6}},
    boulder_field: {params: {choke: 0.4, boulders: 12, width: 6, height: 5}}
  },
  build(b, P) {
    const W = b.m(P.width, 8), L = b.m(P.length, 20), H = b.m(P.height, 12), hw = Math.round(W / 2);
    const main = b.room('rbox', {min: [-hw, 0, 0], max: [hw, H, L], r: Math.round(Math.min(W, H) / 3)}, {noise: 1 + P.radiusVar * 2});
    b.floor(-hw, 0, hw, L, 0);
    const zc = Math.round(L / 2) + b.jit(Math.round(L / 8), 'chz');
    const rz = Math.round((L * P.chokeLength) / 2);
    const want = Math.round(P.choke * H);
    b.mound(0, 0, zc, hw + b.m(1), rz, want);
    const top = Math.round(Math.min(want, (b.kit.repose * Math.min(hw + b.m(1), rz)) / 3));
    const gap = b.m(P.gap);
    if (gap > 0 && top > H - gap) b.carve('capsule', {a: [0, H - Math.round(gap / 2), zc - rz], b: [0, H - Math.round(gap / 2), zc + rz], r: Math.round(gap / 2) + 1}, {layer: 5, noise: 0.3, tag: 'detail'});
    for (let i = 0; i < P.boulders; i++) {
      const r = b.ri(4, Math.max(5, Math.round(Math.min(hw, H) * 0.6)), 'bld', i, 'r');
      const x = b.ri(-hw, hw, 'bld', i, 'x'), z = zc + b.ri(-rz, rz, 'bld', i, 'z');
      b.fill('ellipsoid', {min: [x - r, -2, z - r], max: [x + r, Math.round(r * 1.3), z + r]}, {mat: 'loose', layer: 4, noise: 0.6});
    }
    b.scatter.push({prim: main, kind: 'rubble', density: 0.8, size: [1, 3], surface: 'floor', mat: 'rubble', seedKey: 'choke'});
    const passY = Math.min(top, H - Math.max(gap, b.m(0.8)));
    b.wayChain('w', [[0, 0, 3], [0, Math.max(0, passY), zc], [0, 0, L - 3]], ['socket:in'], ['socket:out'], {dig: gap === 0 && top >= H - b.m(0.8), crawl: gap > 0});
    b.socket('in', [0, 0, 0], '-z', W, H, 'rough');
    b.socket('out', [0, 0, L], '+z', W, H, 'rough');
    void clamp;
  }
};

defineKit(KIT, [MOUTH, TUNNEL, CAVERN, SHAFT, CHASM, GROTTO, CRAWL, CHOKE]);
export default KIT;
