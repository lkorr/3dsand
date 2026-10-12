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
  lights: {fungus: {prop: 'caves/glow_mushroom_patch'}, candles: {prop: 'ruins/candle_cluster'}, lantern: {prop: 'mines/lantern'}},
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

/** A meandering tube from (0,y0,0) along +Z. Returns {pts, radii, prim}. */
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
    const x = Math.round(amp * env * Math.sin(ph + freq * t * 2) + shift * t);
    let r = R;
    if (i > 0 && i < n - 1) {
      r = R * (1 + (P.radiusVar || 0) * (b.r(key, 'rv', i) * 2 - 1));
      if (squeezeAt.has(i)) r *= (P.squeeze || 0.6);
    }
    r = Math.max(4, Math.round(r));
    const y = Math.round(rise * t) + (i === 0 || i === n - 1 ? R : r);   // floor near y = rise*t
    pts.push([x, y, Math.round(L * t)]);
    radii.push(r);
  }
  const prim = b.room('spline_tube', {pts, radii});
  // Flatten the floor: a second tube shifted down fills the bottom of the round.
  if ((P.flatten || 0) > 0) {
    const f = P.flatten;
    b.fill('spline_tube', {pts: pts.map((p, i) => [p[0], p[1] - Math.round(radii[i] * (2 - 0.45 * f)), p[2]]), radii},
           {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
  }
  return {pts, radii, prim};
}

const ROWS_TUBE = [
  {sec: 'Passage'},
  {k: 'radiusVar', n: 'radius variation', min: 0, max: 0.8, step: 0.05, def: 0.25, d: 'Seeded swelling and pinching along the passage.'},
  {k: 'meander', n: 'meander (m)', min: 0, max: 10, step: 0.1, def: 1.5, d: 'Sideways wander (zero at both ends so the sockets line up).'},
  {k: 'meanderFreq', n: 'meander frequency', min: 0.2, max: 4, step: 0.1, def: 1, d: 'Bends per length.'},
  {k: 'squeezes', n: 'squeeze points', min: 0, max: 6, step: 1, int: true, def: 1, d: 'Places where the passage pinches.'},
  {k: 'squeeze', n: 'squeeze factor', min: 0.3, max: 1, step: 0.05, def: 0.65, d: 'Radius multiplier at a squeeze.'},
  {k: 'flatten', n: 'floor flatten', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Fill the bottom of the round with floor so it can be walked.'}
];

const TUNNEL = {
  type: 'tunnel', label: 'Tunnel', version: 1,
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
    rift: {params: {width: 2, radiusVar: 0.5, meander: 2.5, squeezes: 3, squeeze: 0.45, wallRough: 0.9, moisture: 0.6}}
  },
  build(b, P) {
    const R = Math.round(b.m(P.width, 8) / 2), L = b.m(P.length, 20);
    const rise = b.m(P.rise), shift = b.m(P.endShift);
    const {pts, radii, prim} = tubePath(b, P, 'tun', L, R, {rise, shift});
    formations(b, P, prim);
    pts.forEach((p, i) => { if (i % 2 === 1) b.lightSpot([p[0], p[1] - radii[i] + 1, p[2]], 'floor', '+y'); });
    b.wayChain('w', pts.slice(1, -1).map((p, i) => [p[0], p[1] - radii[i + 1], p[2]]), ['socket:in'], ['socket:out']);
    b.socket('in', [0, 0, 0], '-z', 2 * R, 2 * R, 'round');
    b.socket('out', [shift, rise, L], '+z', 2 * R, 2 * R, 'round');
  }
};

const CRAWL = {
  type: 'crawl', label: 'Crawl', version: 1,
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
    bedding_plane: {params: {width: 4, height: 0.9, meander: 0.3, pinches: 1, floorRubble: 0.2}}
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
      const x = i === 0 || i === n - 1 ? 0 : Math.round(amp * env * Math.sin(ph + t * 7));
      const h = pinch.has(i) && i > 0 && i < n - 1 ? Math.max(5, Math.round(H * P.pinch)) : H;
      const y = Math.round(rise * t);
      const k = b.room('rbox', {min: [x - hw, y, z0 - 2], max: [x + hw, y + h, z1 + 2], r: Math.round(Math.min(W, h) / 3)}, {blend: Math.max(P.blend, 4)});
      if (first < 0) first = k;
      pts.push([x, y, Math.round((z0 + z1) / 2)]);
    }
    if (P.floorRubble > 0) b.scatter.push({prim: first, kind: 'rubble', density: +(P.floorRubble).toFixed(3), size: [1, 2], surface: 'floor', mat: 'rubble', seedKey: 'crawlfloor'});
    b.wayChain('w', pts, ['socket:in'], ['socket:out'], {crawl: true});
    b.socket('in', [0, 0, 0], '-z', W, H, 'rough');
    b.socket('out', [0, rise, L], '+z', W, H, 'rough');
  }
};

/** A cave room: an ellipsoid cluster in a W x L footprint, entered through a
 *  short tube from the origin. Returns frame info for the caller. */
function caveRoom(b, P, key) {
  const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 20), C = b.m(P.ceiling);
  const hw = Math.round(W / 2), mR = Math.round(b.m(P.mouth, 10) / 2), A = b.m(1.5);
  b.carve('capsule', {a: [0, mR, Math.min(mR, A)], b: [0, mR, A + mR], r: mR}, {tag: 'door', blend: Math.max(P.blend, 4)});
  const z0 = A, z1 = A + L, zc = Math.round((z0 + z1) / 2);
  const D = b.m(1.5);
  const main = b.room('ellipsoid', {min: [-hw, -D, z0], max: [hw, H + C, z1]});
  const n = P.clusters || 0;
  for (let i = 0; i < n; i++) {
    const rx = Math.round(hw * (0.35 + 0.4 * b.r(key, 'cl', i, 'rx')));
    const rz = Math.round((L / 2) * (0.3 + 0.4 * b.r(key, 'cl', i, 'rz')));
    const ry = Math.round((H + C) * (0.35 + 0.35 * b.r(key, 'cl', i, 'ry')));
    const x = Math.round((b.r(key, 'cl', i, 'x') * 2 - 1) * (hw - rx * 0.5));
    const z = zc + Math.round((b.r(key, 'cl', i, 'z') * 2 - 1) * (L / 2 - rz * 0.5));
    b.room('ellipsoid', {min: [x - rx, -D, z - rz], max: [x + rx, -D + 2 * ry, z + rz]});
  }
  // Floor: fill the bowl up to a flat floor (flatten 1) or part way.
  const fy = -Math.round(D * (1 - (P.flatten === undefined ? 0.8 : P.flatten)));
  b.fill('box', {min: [-hw - 4, -D - 4, z0], max: [hw + 4, fy, z1]}, {mat: 'floor', noise: 'floor', layer: 2, mask: 'air'});
  // The main ellipsoid's footprint AT FLOOR HEIGHT is smaller than W x L:
  // floor features go through floorAt(u, v) (u, v in the unit disc) so they
  // land on the floor, not in the rock round it.
  const cy = (-D + H + C) / 2, hy = (H + C + D) / 2;
  const k = Math.sqrt(Math.max(0.05, 1 - ((fy + 0.5 - cy) / hy) ** 2));
  const floorAt = (u, v, margin) => {
    const r = Math.hypot(u, v), sc = r > 1 ? 1 / r : 1;
    const m = margin || 0;
    return [Math.round(u * sc * Math.max(1, k * hw - m)), fy, zc + Math.round(v * sc * Math.max(1, (k * L) / 2 - m))];
  };
  return {W, L, H, C, hw, mR, A, z0, z1, zc, D, fy, main, k, floorAt};
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
  const spots = [
    {name: 'out', a: [0, R, f.z1 - R], b: [0, R, f.z1 + b.m(1.5)], pos: [0, 0, f.z1 + b.m(1.5) + R], dir: '+z'},
    {name: 'right', a: [f.hw - R, R, f.zc], b: [f.hw + b.m(1.5), R, f.zc], pos: [f.hw + b.m(1.5) + R, 0, f.zc], dir: '+x'},
    {name: 'left', a: [-f.hw + R, R, f.zc], b: [-f.hw - b.m(1.5), R, f.zc], pos: [-f.hw - b.m(1.5) - R, 0, f.zc], dir: '-x'}
  ];
  for (let i = 0; i < Math.min(3, P.exits); i++) {
    const s = spots[i];
    b.carve('capsule', {a: s.a, b: s.b, r: R}, {tag: 'door', blend: Math.max(P.blend, 4)});
    b.socket(s.name, s.pos, s.dir, 2 * R, 2 * R, 'rough');
    const wp = [Math.round((s.a[0] + s.b[0]) / 2), f.fy, Math.round((s.a[2] + s.b[2]) / 2)];
    b.way('x_' + s.name, wp, ['c', 'socket:' + s.name]);
    out.push('x_' + s.name);
  }
  b.way('door', [0, f.fy, Math.round(f.A / 2) + 2], ['socket:in', 'c']);
  b.way('c', [0, f.fy, f.zc], ['door'].concat(out, ways || []));
  b.socket('in', [0, 0, 0], '-z', 2 * R, 2 * R, 'rough');
}

const CAVERN = {
  type: 'cavern', label: 'Cavern', version: 1,
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
  ]),
  PRESETS: {
    chamber: {params: {}},
    great_dome: {params: {width: 36, length: 40, height: 10, ceiling: 16, clusters: 5, columns: 4, stalactites: 0.5, flowstone: 0.6, lightCount: 6, exits: 3}},
    crystal_geode: {params: {width: 12, length: 14, height: 5, ceiling: 6, clusters: 1, crystals: 1, lightType: 'crystal', lightCount: 6, flowstone: 0, pool: 0}},
    fungal_grotto: {params: {mushrooms: 8, moisture: 0.8, pool: 0.4, lightType: 'fungus', lightCount: 5, stalagmites: 0.05}},
    breakdown_hall: {params: {width: 24, length: 30, boulders: 10, decay: 0.9, flatten: 0.4, columns: 0, ceiling: 7}}
  },
  build(b, P) {
    const f = caveRoom(b, P, 'cav');
    formations(b, P, f.main);
    for (let i = 0; i < P.columns; i++) {
      const r = b.ri(b.m(0.4), b.m(1.2), 'col', i, 'r');
      const [x, , z] = f.floorAt(...disc(b, 'col', i), r * 2);
      if (Math.abs(x) < f.mR + r + 4) continue;              // keep the axis clear for the way through
      b.fill('capsule', {a: [x, f.fy - 2, z], b: [x, f.H + f.C, z], r}, {mat: 'support', layer: 2, noise: 0.35});
    }
    const nFlow = Math.round(P.flowstone * 8);
    for (let i = 0; i < nFlow; i++) {
      const side = b.r('flow', i, 'side') < 0.5 ? -1 : 1, r = b.ri(b.m(0.8), b.m(2.2), 'flow', i, 'r');
      const z = b.ri(f.z0 + r, f.z1 - r, 'flow', i, 'z');
      const x = Math.round(side * f.hw * 0.85);
      b.fill('ellipsoid', {min: [x - r, f.fy, z - r], max: [x + r, f.fy + Math.round(r * 2.5), z + r]}, {mat: 'trim', layer: 2, noise: 0.6});
    }
    if (P.crystals > 0) b.scatter.push({prim: f.main, kind: 'crystal', density: +(P.crystals * 0.6).toFixed(3), size: [2, 5 + Math.round(P.crystals * 8)], surface: 'wall', mat: 'accent', seedKey: 'crystal'});
    for (let i = 0; i < Math.round(P.crystals * 4); i++)
      b.prop('caves/crystal_cluster', {size: 0.6 + P.crystals}, f.floorAt(...disc(b, 'cc', i), 8), b.ri(0, 3, 'cc', i, 'yaw') * 90);
    for (let i = 0; i < P.mushrooms; i++)
      b.prop('caves/glow_mushroom_patch', {}, f.floorAt(...disc(b, 'mush', i), 8), b.ri(0, 3, 'mush', i, 'yaw') * 90, {light: true});
    for (let i = 0; i < P.boulders; i++) {
      const r = b.ri(4, 12, 'bld', i, 'r');
      const [x, , z] = f.floorAt(...disc(b, 'bld', i), r);
      b.fill('ellipsoid', {min: [x - r, f.fy - 2, z - Math.round(r * 1.2)], max: [x + r, f.fy + Math.round(r * 1.3), z + Math.round(r * 1.2)]}, {mat: 'loose', layer: 4, noise: 0.4, mask: 'air'});
    }
    if (P.pool > 0) {
      const pr = Math.round(Math.min(f.hw, f.L / 2) * 0.6 * P.pool) + 4, pd = Math.max(4, Math.round(pr * 0.35));
      const [px, , pz] = f.floorAt(...disc(b, 'pool'), pr);
      b.carve('ellipsoid', {min: [px - pr, f.fy - pd, pz - pr], max: [px + pr, f.fy + pd, pz + pr]}, {layer: 3, noise: 0.25, tag: 'room', liquid: 'liquid', level: f.fy - 2});
    }
    for (let i = 0; i < 8; i++) {
      const a = (i / 8) * Math.PI * 2;
      b.lightSpot(f.floorAt(Math.sin(a) * 0.8, Math.cos(a) * 0.8, 4), 'floor', '+y');
    }
    roomExits(b, P, f);
  }
};

const GROTTO = {
  type: 'grotto_pool', label: 'Grotto pool', version: 1,
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
  ]),
  PRESETS: {
    still_pool: {params: {}},
    underground_lake: {params: {width: 30, length: 36, height: 6, ceiling: 8, poolSize: 0.9, poolDepth: 5, glow: 0.3, exits: 2, beach: 0.1}},
    glow_grotto: {params: {glow: 1, poolSize: 0.4, island: true, mushrooms: 0, lightCount: 6, stalactites: 0.6}},
    sea_pool: {params: {poolSize: 0.8, poolDepth: 3, beach: 1, glow: 0.1, lightType: 'none', lightCount: 0}}
  },
  build(b, P) {
    const f = caveRoom(b, P, 'grot');
    formations(b, P, f.main);
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
    {k: 'footPool', n: 'plunge pool (m)', min: 0, max: 4, step: 0.1, def: 0, d: 'Water at the bottom.'}
  ],
  PRESETS: {
    pitch: {params: {}},
    deep_pothole: {params: {length: 40, width: 5, wander: 3, ledges: 5, surfaceTop: true, lightCount: 0}},
    narrow_drop: {params: {width: 1.6, length: 8, ledges: 0, rope: false}},
    plunge: {params: {length: 20, footPool: 2.5, moisture: 0.9, ledges: 3, width: 4}}
  },
  build(b, P) {
    const R = Math.round(b.m(P.width, 6) / 2), D = b.m(P.length, 10), Hf = b.m(P.height, 10), wand = b.m(P.wander);
    b.surface = P.surfaceTop;
    const n = Math.max(3, Math.round(D / b.m(3)) + 1);
    const pts = [], radii = [];
    for (let i = 0; i < n; i++) {
      const t = i / (n - 1);
      const env = Math.sin(Math.PI * t);
      pts.push([Math.round(wand * env * (b.r('sw', i, 'x') * 2 - 1)), Math.round(-D * t), Math.round(wand * env * (b.r('sw', i, 'z') * 2 - 1))]);
      radii.push(i === 0 ? R : Math.round(R * (0.85 + 0.3 * b.r('sw', i, 'r'))));
    }
    const shaft = b.room('spline_tube', {pts, radii});
    // The foot chamber and its side exit.
    const fc = b.room('ellipsoid', {min: [-R - 6, -D - b.m(0.5), -R - 6], max: [R + 6, -D + Hf, R + 6 + b.m(2)]});
    b.fill('box', {min: [-R - 10, -D - b.m(1), -R - 10], max: [R + 10, -D, R + 10 + b.m(4)]}, {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
    const Rx = Math.max(Math.round(b.m(1.4)), Math.min(R, b.m(1.5)));
    b.carve('capsule', {a: [0, -D + Rx, R], b: [0, -D + Rx, R + 6 + b.m(3)], r: Rx}, {tag: 'door', blend: Math.max(P.blend, 4)});
    if (P.footPool > 0) {
      const pd = b.m(P.footPool);
      b.carve('ellipsoid', {min: [-R, -D - pd, -R], max: [R, -D + 2, R]}, {layer: 3, noise: 0.3, tag: 'room', liquid: 'liquid', level: -D - 2});
    }
    for (let k = 0; k < P.ledges; k++) {
      const i = 1 + Math.floor(b.r('ledge', k) * (n - 2));
      const p = pts[i], r = radii[i], lw = b.m(P.ledgeWidth, 3);
      const dir = b.ri(0, 3, 'ledge', k, 'dir');
      const [dx, dz] = [[1, 0], [-1, 0], [0, 1], [0, -1]][dir];
      const cx = p[0] + dx * (r - Math.round(lw / 2)), cz = p[2] + dz * (r - Math.round(lw / 2));
      b.fill('ellipsoid', {min: [cx - lw - (dz ? b.m(1) : 0), p[1] - 4, cz - lw - (dx ? b.m(1) : 0)], max: [cx + lw + (dz ? b.m(1) : 0), p[1], cz + lw + (dx ? b.m(1) : 0)]}, {mat: 'wall', layer: 2, noise: 0.4});
    }
    formations(b, Object.assign({}, P, {stalactites: 0, stalagmites: 0.3, stalLength: 0.8}), fc);
    if (P.rope) b.ladder('caves/rope_ladder', P.length, [pts[0][0], -D, pts[0][2] - R + 2], 0);
    if (P.surfaceTop) b.carve('ellipsoid', {min: [-R * 2, -R, -R * 2], max: [R * 2, R * 2, R * 2]}, {layer: 1, noise: 1.2, blend: 8, tag: 'apron'});
    b.lightSpot([0, -D, R + 4], 'floor', '+y');
    b.lightSpot([R - 2, Math.round(-D / 2), 0], 'wall', '-x');
    b.way('top', [0, 0, 0], ['socket:top', 'foot'], {climb: true, ladder: P.rope});
    b.way('foot', [0, -D, 0], ['top', 'exit'], {climb: true, ladder: P.rope});
    b.way('exit', [0, -D, R + 6 + b.m(2)], ['foot', 'socket:foot']);
    b.socket('top', [0, 0, 0], '+y', 2 * R, 2 * R, 'shaft', P.surfaceTop ? ['surface'] : []);
    b.socket('foot', [0, -D, R + 6 + b.m(3) + Rx], '+z', 2 * Rx, 2 * Rx, 'round');
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
    b.lightSpot([-hw + 2, 0, zc - cw - 6], 'floor', '+y');
    b.lightSpot([hw - 2, 0, zc + cw + 6], 'floor', '+y');
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
