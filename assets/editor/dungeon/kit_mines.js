/* kit_mines.js — the MINES kit (PLAN_dungeons §4): timber and stone,
 * rectilinear with sag, industrial clutter.
 *
 * Every piece: origin socket 'in' at (0,0,0) facing -Z, floor at y = 0, the
 * body running to +Z (see dgutil.js for the frame). Sizes in metres.
 *
 * Fragility is MATERIAL-driven (PLAN §0): timber sets are `timber` (burns),
 * rubble is `gravel` (loose powder, heaped at repose), rock is static stone.
 */
'use strict';
import { defineKit, rampProfile } from './dgutil.js';

export const KIT = {
  name: 'mines', label: 'Mines',
  roughMax: 4, floorMax: 3, octaves: 2, repose: 0.9,
  lightTypes: ['lantern', 'torch', 'candles', 'crystal', 'none'],
  lights: {lantern: {prop: 'mines/lantern'}, candles: {prop: 'ruins/candle_cluster'}},
  /** Material slots by NAME (resolved against materials.json at load). */
  PALETTES: {
    granite: {wall: 'stone', floor: 'packed_gravel', ceil: 'stone', trim: 'plank', support: 'timber',
              rubble: 'gravel', accent: 'iron', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
              ore: 'iron', moss: 'old_moss', rail: 'iron', spoil: 'gravel', loose: 'cobble'},
    sandstone: {wall: 'sandstone', floor: 'packed_earth', ceil: 'sandstone', trim: 'plank', support: 'timber',
                rubble: 'sand', accent: 'brass', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
                ore: 'gold', moss: 'old_moss', rail: 'iron', spoil: 'sand', loose: 'sandstone'},
    basalt: {wall: 'basalt', floor: 'packed_gravel', ceil: 'basalt', trim: 'plank', support: 'timber',
             rubble: 'cinder', accent: 'iron', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
             ore: 'brimstone', moss: 'old_moss', rail: 'iron', spoil: 'cinder', loose: 'basalt'},
    crystal: {wall: 'quartzite', floor: 'packed_gravel', ceil: 'quartzite', trim: 'plank', support: 'timber',
              rubble: 'gravel', accent: 'crystal_spire', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
              ore: 'crystal', moss: 'old_moss', rail: 'iron', spoil: 'gravel', loose: 'quartzite'}
  },
  DEFAULT_PALETTE: 'granite',
  /** Layout-assist weights (grow): what a mine grows into. */
  /** Dead-end pieces grow() may cap an open socket with. */
  CAPS: [{type: 'camp', params: {exits: 'dead end'}}, {type: 'stope', params: {exits: 'dead end'}}],
  GROW: {drift: 6, junction: 3, incline: 2, stope: 1.5, camp: 1, collapse: 0.7, flooded_sump: 0.6, shaft: 0.6},
  TYPES: {}
};

// ---------------------------------------------------------------------------
// shared pieces of mine furniture
// ---------------------------------------------------------------------------
/** One timber set across a passage of half-width hw, clear height H, at z.
 *  Returns {broken} so a caller can heap rubble. Keys: ('set', key, i). */
function timberSet(b, P, key, i, z, hw, H, t, opt) {
  opt = opt || {};
  const y0 = opt.y0 || 0;
  const brokenP = Math.min(1, (P.broken || 0) + P.decay * 0.25 + (opt.extraBroken || 0));
  const broken = b.r('set', key, i, 'broken') < brokenP;
  const mode = broken ? b.pick(['post', 'cap', 'both'], 'set', key, i, 'mode') : 'ok';
  const lean = b.jit(1, 'set', key, i, 'lean');
  if (mode !== 'post' && mode !== 'both') b.fill('box', {min: [-hw, y0, z], max: [-hw + t, y0 + H, z + t]}, {mat: 'support', layer: 3, noise: null});
  b.fill('box', {min: [hw - t + lean, y0, z], max: [hw + lean, y0 + H, z + t]}, {mat: 'support', layer: 3, noise: null});
  if (mode === 'cap' || mode === 'both') {
    // The cap came down: it lies across the floor, a little skewed.
    const s = b.jit(3, 'set', key, i, 'fall');
    b.fill('box', {min: [-hw + t + 1, y0, z + s], max: [hw - t - 1, y0 + t, z + s + t]}, {mat: 'support', layer: 4, noise: null, mask: 'air'});
    b.mound(b.jit(4, 'set', key, i, 'heapx'), y0, z + t, Math.max(6, hw * 0.8), Math.max(6, hw * 0.6), H * 0.25);
  } else {
    b.fill('box', {min: [-hw, y0 + H - t, z], max: [hw, y0 + H, z + t]}, {mat: 'support', layer: 3, noise: null});
    if (b.r('set', key, i, 'sag') < (P.sag || 0)) {
      const d = Math.max(1, Math.round(t * 0.6));
      b.fill('box', {min: [-Math.round(hw / 2), y0 + H - t - d, z], max: [Math.round(hw / 2), y0 + H - t, z + t]}, {mat: 'support', layer: 3, noise: null});
    }
  }
  return {broken, mode};
}

/** Rails along +Z from z0 to z1 (floor y0), gauge g, sleepers every sp. */
function rails(b, z0, z1, g, sp, y0, key) {
  y0 = y0 || 0;
  const hg = Math.round(g / 2);
  for (let z = z0 + 1, i = 0; z + 2 <= z1; z += sp, i++) {
    if (b.r('sleeper', key, i) < 0.08) continue;      // a missing sleeper here and there
    b.fill('box', {min: [-hg - 2, y0, z], max: [hg + 3, y0 + 1, z + 2]}, {mat: 'support', layer: 3, noise: null});
  }
  b.fill('box', {min: [-hg - 1, y0 + 1, z0], max: [-hg, y0 + 2, z1]}, {mat: 'rail', layer: 3, noise: null});
  b.fill('box', {min: [hg, y0 + 1, z0], max: [hg + 1, y0 + 2, z1]}, {mat: 'rail', layer: 3, noise: null});
}

/** A straight timbered drift section along +Z; shared by drift, collapse,
 *  camp approaches. Returns {room, setsZ}. */
function driftBody(b, P, z0, z1, W, H, C, key, opt) {
  opt = opt || {};
  const hw = Math.round(W / 2);
  const room = b.room('box', {min: [-hw, 0, z0], max: [hw, H, z1]});
  if (C >= 2) b.room('vault', {min: [-hw + 2, H - 1, z0], max: [hw - 2, H + C, z1], axis: 'z', rise: C + 1});
  b.floor(-hw, z0, hw, z1, 0);
  const t = b.m(P.beam || 0.25, 2);
  const S = Math.max(t + 4, b.m(P.setSpacing || 1.8));
  const setsZ = [];
  for (let i = 0, z = z0 + Math.round(S / 2); z + t <= z1 - 1; i++, z += S) {
    const zz = z + b.jit(Math.round(S * 0.15), 'setz', key, i);
    timberSet(b, P, key, i, zz, hw, H, t, opt);
    setsZ.push(zz);
    if ((P.lagging || 0) > 0 && b.r('lag', key, i) < P.lagging && zz + S < z1)
      b.fill('box', {min: [-hw, H, zz], max: [hw, H + 1, zz + S]}, {mat: 'trim', layer: 3, noise: null, mask: 'air'});
  }
  const every = Math.max(1, Math.round((P.lampSpacing || 6) / Math.max(0.5, P.setSpacing || 1.8)));
  setsZ.forEach((z, i) => { if (i % every === 0) b.lightSpot([0, H - t - 1, z], 'ceil', '-y'); });
  return {room, setsZ, t, hw};
}

const ROWS_TIMBER = [
  {sec: 'Timber', note: 'Sets = two posts and a cap. Timber BURNS: a fire in a drift brings the sets down.'},
  {k: 'setSpacing', n: 'set spacing (m)', min: 0.8, max: 5, step: 0.1, def: 1.8, d: 'Distance between timber sets.'},
  {k: 'beam', n: 'beam thickness (m)', min: 0.1, max: 0.5, step: 0.05, def: 0.25, d: 'Post and cap size.'},
  {k: 'sag', n: 'sagging caps', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Chance a cap bows under the load (an extra block under its middle).'},
  {k: 'broken', n: 'broken sets', min: 0, max: 1, step: 0.05, def: 0.1, d: 'Chance a set lost a post or its cap (decay adds to it); a fallen cap heaps rubble.'},
  {k: 'lagging', n: 'lagging', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Chance a bay has plank lagging laid over its caps.'},
  {k: 'lampSpacing', n: 'lamp spacing (m)', min: 1, max: 20, step: 0.5, def: 6, d: 'Lamp hooks on every Nth cap; the light count picks among them.'}
];
const ROWS_RAIL = [
  {sec: 'Rails'},
  {k: 'rails', n: 'rails', kind: 'bool', def: true, d: 'Iron rails on timber sleepers down the middle.'},
  {k: 'gauge', n: 'gauge (m)', min: 0.4, max: 1.2, step: 0.05, def: 0.6, d: 'Distance between the rails.'},
  {k: 'sleeperSpacing', n: 'sleeper spacing (m)', min: 0.3, max: 1.5, step: 0.05, def: 0.6, d: ''}
];

// ---------------------------------------------------------------------------
// the piece types
// ---------------------------------------------------------------------------
const DRIFT = {
  type: 'drift', label: 'Drift', version: 1,
  size: {width: {min: 1.5, max: 6, def: 2.6}, length: {min: 3, max: 60, def: 12}, height: {min: 1.6, max: 5, def: 2.4},
         ceiling: {min: 0, max: 2, def: 0.3, d: 'Rounded back of the drift above the caps.'}},
  u: {wallRough: 0.4, lightCount: 2},
  rows: ROWS_TIMBER.concat(ROWS_RAIL, [
    {sec: 'Side niches + spoil'},
    {k: 'niches', n: 'niche frequency', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Chance per bay of a side niche (a refuge hole), alternating walls by seed.'},
    {k: 'nicheDepth', n: 'niche depth (m)', min: 0.4, max: 3, step: 0.1, def: 1.2, d: ''},
    {k: 'spoil', n: 'floor spoil', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Waste rock along the walls and on the floor.'}
  ]),
  PRESETS: {
    standard: {params: {}},
    narrow_old: {params: {width: 1.8, height: 1.9, setSpacing: 1.2, broken: 0.35, sag: 0.5, rails: false, decay: 0.6, moisture: 0.5, niches: 0.4, lightCount: 1, lightType: 'candles'}},
    haulage: {params: {width: 4, height: 3.2, ceiling: 0.8, setSpacing: 2.6, beam: 0.35, broken: 0, sag: 0.05, gauge: 0.9, lagging: 0.8, decay: 0.05, spoil: 0.1, lightCount: 4}},
    abandoned: {params: {broken: 0.6, sag: 0.7, decay: 0.85, moisture: 0.7, lagging: 0.1, spoil: 0.8, lightType: 'none', lightCount: 0}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 14), L = b.m(P.length, 20), C = b.m(P.ceiling);
    const {room, setsZ, hw} = driftBody(b, P, 0, L, W, H, C, 'd');
    if (P.rails) rails(b, 0, L, b.m(P.gauge, 4), b.m(P.sleeperSpacing, 3), 0, 'd');
    if (P.spoil > 0) b.scatter.push({prim: room, kind: 'rubble', density: +(P.spoil * 0.8).toFixed(3), size: [1, 3], surface: 'floor', mat: 'spoil', seedKey: 'spoil'});
    const nSpoil = Math.round(P.spoil * L / b.m(8));
    for (let i = 0; i < nSpoil; i++) {
      const side = b.r('spoil', i, 'side') < 0.5 ? -1 : 1;
      b.mound(side * (hw - 2), 0, b.ri(4, L - 4, 'spoil', i, 'z'), b.m(0.8), b.m(1.4), b.m(0.4), {mat: 'spoil'});
    }
    // Side niches between sets, with a crate or a barrel (deco).
    const D = b.m(P.nicheDepth, 4);
    for (let i = 0; i + 1 < setsZ.length; i++) {
      if (b.r('niche', i) >= P.niches) continue;
      const side = b.r('niche', i, 'side') < 0.5 ? -1 : 1;
      const zm = Math.round((setsZ[i] + setsZ[i + 1]) / 2), nw = b.m(0.7);
      b.carve('box', {min: [side * hw, 0, zm - nw], max: [side * (hw + D), b.m(1.8), zm + nw]}, {tag: 'niche'});
      if (b.r('niche', i, 'deco') < P.deco) {
        const what = b.pick(['mines/crate', 'mines/barrel', 'mines/crate'], 'niche', i, 'what');
        const at = [side * (hw + Math.round(D / 2)), 0, zm];
        b.prop(what, {}, at, side < 0 ? 90 : 270);
        if (what === 'mines/crate') b.ref('container', at, {name: 'niche_crate' + i, props: {loot: 'mine_common'}});
      }
    }
    const pts = [];
    for (let z = 3; z < L - 2; z += b.m(4)) pts.push([0, 0, z]);
    if (!pts.length) pts.push([0, 0, Math.round(L / 2)]);
    b.wayChain('w', pts, ['socket:in'], ['socket:out']);
    b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
    b.socket('out', [0, 0, L], '+z', W, H, 'rect');
  }
};

const JUNCTION = {
  type: 'junction', label: 'Crosscut / junction', version: 1,
  size: {width: {min: 1.5, max: 6, def: 2.6}, length: {min: 1, max: 20, def: 3, n: 'arm length (m)', d: 'Length of each arm beyond the hub.'},
         height: {min: 1.6, max: 5, def: 2.4}, ceiling: {min: 0, max: 3, def: 0.6}},
  u: {lightCount: 1},
  rows: ROWS_TIMBER.concat([
    {sec: 'Shape'},
    {k: 'shape', n: 'shape', kind: 'enum', options: ['T', 'X', 'L', 'Y'], def: 'T',
     d: 'T = left + right arms; X = left, right and straight on; L = one side arm (see side); Y = straight on, offset sideways.'},
    {k: 'side', n: 'L side', kind: 'enum', options: ['left', 'right'], def: 'left', d: ''},
    {k: 'offset', n: 'Y offset (m)', min: -6, max: 6, step: 0.1, def: 2, d: 'Sideways shift of the straight-on arm for a Y.'},
    {k: 'hubPad', n: 'hub widening (m)', min: 0, max: 3, step: 0.1, def: 0.6, d: 'How much wider than the arms the crossing is.'},
    {k: 'cornerPosts', n: 'corner posts', kind: 'bool', def: true, d: 'Heavy timber posts at the hub corners.'}
  ], ROWS_RAIL),
  PRESETS: {
    tee: {params: {shape: 'T'}},
    crossroads: {params: {shape: 'X', hubPad: 1.4, lightCount: 2, ceiling: 1}},
    dogleg: {params: {shape: 'Y', offset: 3.5, hubPad: 1.0}},
    bend_old: {params: {shape: 'L', side: 'right', decay: 0.7, broken: 0.5, rails: false, cornerPosts: false}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 14), A = b.m(P.length, 6), C = b.m(P.ceiling);
    const hub = W + 2 * b.m(P.hubPad), hh = Math.round(hub / 2), hw = Math.round(W / 2);
    const z0 = A, z1 = A + hub, zc = Math.round((z0 + z1) / 2);
    driftBody(b, P, 0, z0, W, H, C, 'in');
    const hubRoom = b.room('box', {min: [-hh, 0, z0], max: [hh, H + Math.round(C / 2), z1]});
    if (C >= 2) b.room('ellipsoid', {min: [-hh, H - C, z0], max: [hh, H + C, z1]});
    b.floor(-hh, z0, hh, z1, 0);
    const arms = {T: ['left', 'right'], X: ['left', 'right', 'out'], L: [P.side], Y: ['out']}[P.shape];
    const t = b.m(P.beam, 2);
    if (P.cornerPosts) for (const sx of [-1, 1]) for (const sz of [0, 1]) {
      const x = sx < 0 ? -hh : hh - t - 1, z = sz ? z1 - t - 1 : z0;
      b.fill('box', {min: [x, 0, z], max: [x + t + 1, H, z + t + 1]}, {mat: 'support', layer: 3, noise: null});
    }
    b.lightSpot([0, H - 1, zc], 'ceil', '-y');
    const hubNode = 'hub';
    const links = ['in2'];
    if (arms.includes('left')) {
      b.room('box', {min: [-hh - A, 0, zc - hw], max: [-hh + 1, H, zc + hw]});
      b.floor(-hh - A, zc - hw, -hh, zc + hw, 0);
      b.socket('left', [-hh - A, 0, zc], '-x', W, H, 'rect');
      b.way('l0', [-hh - Math.round(A / 2), 0, zc], [hubNode, 'socket:left']); links.push('l0');
    }
    if (arms.includes('right')) {
      b.room('box', {min: [hh - 1, 0, zc - hw], max: [hh + A, H, zc + hw]});
      b.floor(hh, zc - hw, hh + A, zc + hw, 0);
      b.socket('right', [hh + A, 0, zc], '+x', W, H, 'rect');
      b.way('r0', [hh + Math.round(A / 2), 0, zc], [hubNode, 'socket:right']); links.push('r0');
    }
    if (arms.includes('out')) {
      const ox = P.shape === 'Y' ? b.m(P.offset) : 0;
      const ohw = hw;
      b.room('box', {min: [ox - ohw, 0, z1 - 1], max: [ox + ohw, H, z1 + A]});
      if (P.shape === 'Y') b.room('box', {min: [Math.min(0, ox) - ohw, 0, z0], max: [Math.max(0, ox) + ohw, H, z1]});
      b.floor(ox - ohw, z1, ox + ohw, z1 + A, 0);
      b.socket('out', [ox, 0, z1 + A], '+z', W, H, 'rect');
      b.way('o0', [ox, 0, z1 + Math.round(A / 2)], [hubNode, 'socket:out']); links.push('o0');
    }
    if (P.rails) rails(b, 0, z1 - 2, b.m(P.gauge, 4), b.m(P.sleeperSpacing, 3), 0, 'j');
    b.way('in2', [0, 0, Math.round(A / 2)], ['socket:in', hubNode]);
    b.way(hubNode, [0, 0, zc], links);
    b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
    void hubRoom;
  }
};

const INCLINE = {
  type: 'incline', label: 'Incline', version: 1,
  size: {width: {min: 1.5, max: 6, def: 2.6}, length: {min: 3, max: 60, def: 12}, height: {min: 1.6, max: 5, def: 2.4}, ceiling: null},
  u: {lightCount: 1},
  rows: [
    {sec: 'Slope'},
    {k: 'rise', n: 'rise (m)', min: 0.5, max: 30, step: 0.1, def: 4, d: 'Height gained over the length.'},
    {k: 'direction', n: 'direction', kind: 'enum', options: ['up', 'down'], def: 'down', d: 'Seen from the origin socket.'},
    {k: 'mode', n: 'floor', kind: 'enum', options: ['stairs', 'ramp', 'rails'], def: 'stairs', d: 'stairs = cut steps; ramp = a smooth voxel slope; rails = a smooth slope with a rail track (a haulage incline).'},
    {k: 'stepHeight', n: 'step height (m)', min: 0.1, max: 0.4, step: 0.05, def: 0.2, d: ''},
    {k: 'handrail', n: 'handrail', kind: 'bool', def: true, d: 'A timber rail along one wall.'}
  ].concat(ROWS_TIMBER),
  PRESETS: {
    stair_down: {params: {}},
    haulage_up: {params: {mode: 'rails', direction: 'up', rise: 3, length: 20, width: 3, lightCount: 2}},
    steep_old: {params: {rise: 8, length: 9, stepHeight: 0.3, decay: 0.6, broken: 0.4, handrail: false}},
    long_ramp: {params: {mode: 'ramp', rise: 6, length: 30, setSpacing: 3}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 14), L = b.m(P.length, 20);
    const sgn = P.direction === 'up' ? 1 : -1;
    const hw = Math.round(W / 2);
    // stairs = cut steps; ramp / rails = a smooth slope (1-voxel steps).
    const stepH = P.mode === 'stairs' ? b.m(P.stepHeight, 1) : 1;
    const R = Math.max(1, Math.round(b.m(P.rise, 1) / stepH)) * stepH;
    const dir = sgn > 0 ? '+z' : '-z', y0 = sgn > 0 ? 0 : -R;
    const main = b.flight('carve', dir, -hw, hw, 0, L, y0, R, stepH, {height: H}, {tag: 'room'});
    b.flight('fill', dir, -hw, hw, 0, L, y0, R, stepH, {below: 2}, {mat: 'floor', layer: 2, noise: null});
    if (P.mode === 'rails') {
      const hg = Math.round(b.m(0.6, 4) / 2);
      b.flight('fill', dir, -hg - 1, -hg, 0, L, y0, R, stepH, {height: 1}, {mat: 'rail', layer: 3, noise: null});
      b.flight('fill', dir, hg, hg + 1, 0, L, y0, R, stepH, {height: 1}, {mat: 'rail', layer: 3, noise: null});
    }
    const rp = b.prims[main.prim];
    const yAt = (z) => rampProfile(rp, [0, 0, Math.min(rp.max[2] - 1, Math.max(rp.min[2], z)) + 0.5]);
    const t = b.m(P.beam, 2), S = Math.max(t + 4, b.m(P.setSpacing));
    for (let i = 0, z = Math.round(S / 2); z + t < L; i++, z += S) {
      const ys = Math.round(Math.min(yAt(z), yAt(z + t)));
      timberSet(b, P, 'inc', i, z, hw, H, t, {y0: ys});
      if (i % 3 === 0) b.lightSpot([0, ys + H - t - 1, z], 'ceil', '-y');
    }
    if (P.handrail) b.flight('fill', dir, hw - 2, hw - 1, 0, L, y0 + b.m(0.9), R, stepH, {height: 1}, {mat: 'support', layer: 3, noise: null});
    const pts = [];
    for (let z = 2; z < L - 1; z += b.m(3)) pts.push([0, Math.round(yAt(z)), z]);
    b.wayChain('w', pts, ['socket:in'], ['socket:out']);
    b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
    b.socket('out', [0, sgn * R, L], '+z', W, H, 'rect');
  }
};

const SHAFT = {
  type: 'shaft', label: 'Shaft', version: 1,
  size: {width: {min: 1.5, max: 8, def: 2.4, n: 'shaft size (m)', d: 'Square shaft, side length.'},
         length: {min: 3, max: 120, def: 15, n: 'depth (m)', d: 'From the collar (origin) down to the foot.'},
         height: {min: 1.6, max: 5, def: 2.4, n: 'station height (m)', d: 'Clear height of the landings and the foot station.'}, ceiling: null},
  u: {lightCount: 2, wallRough: 0.2},
  rows: [
    {sec: 'Shaft'},
    {k: 'landings', n: 'landings', min: 0, max: 6, step: 1, int: true, def: 1, d: 'Intermediate stations (each with a side socket).'},
    {k: 'stationDepth', n: 'station depth (m)', min: 1, max: 6, step: 0.1, def: 2.5, d: 'How far a station is cut back from the shaft wall.'},
    {k: 'ladder', n: 'ladder', kind: 'bool', def: true, d: 'A timber ladder down one wall (a climbable route for the nav graph).'},
    {k: 'cribSpacing', n: 'cribbing spacing (m)', min: 0.5, max: 6, step: 0.1, def: 2, d: 'Square timber frames lining the shaft.'},
    {k: 'surfaceTop', n: 'surface collar', kind: 'bool', def: false, d: 'The shaft opens at the surface: a stone collar, and a headframe if asked. Makes this an entrance piece.'},
    {k: 'headframe', n: 'headframe', kind: 'bool', def: true, d: 'Timber A-frame and wheel over a surface collar.'},
    {k: 'cage', n: 'cage at the foot', kind: 'bool', def: false, d: 'An iron hoist cage standing at the bottom.'},
    {k: 'sump', n: 'sump water (m)', min: 0, max: 3, step: 0.1, def: 0, d: 'Water standing below the foot station floor.'}
  ],
  PRESETS: {
    winze: {params: {}},
    main_shaft: {params: {width: 3.6, length: 30, landings: 2, surfaceTop: true, headframe: true, cage: true, lightCount: 4}},
    ladderway: {params: {width: 1.8, length: 12, landings: 0, cribSpacing: 1, decay: 0.4}},
    drowned: {params: {length: 20, sump: 2.5, ladder: false, decay: 0.7, moisture: 0.9, broken: 0.4}}
  },
  build(b, P) {
    const S = b.m(P.width, 10), D = b.m(P.length, 20), H = b.m(P.height, 14), sd = b.m(P.stationDepth, 8);
    const hs = Math.round(S / 2);
    b.surface = P.surfaceTop;
    const top = P.surfaceTop ? b.m(1) : 0;
    b.room('box', {min: [-hs, -D, -hs], max: [hs, top, hs]});
    // Cribbing: square frames.
    const cs = b.m(P.cribSpacing, 4);
    for (let y = -D + H + 2, i = 0; y < -2; y += cs, i++) {
      if (b.r('crib', i) < P.decay * 0.3) continue;
      for (const s of [-1, 1]) {
        b.fill('box', {min: [-hs, y, s < 0 ? -hs : hs - 1], max: [hs, y + 2, s < 0 ? -hs + 1 : hs]}, {mat: 'support', layer: 3, noise: null});
        b.fill('box', {min: [s < 0 ? -hs : hs - 1, y, -hs], max: [s < 0 ? -hs + 1 : hs, y + 2, hs]}, {mat: 'support', layer: 3, noise: null});
      }
    }
    // The foot station on +Z, and landings alternating sides.
    const sideOf = ['+z', '+x', '-x', '-z'];
    const station = (name, y, dir) => {
      const hw = Math.round(Math.min(S, b.m(2.6)) / 2);
      let min, max, pos;
      if (dir === '+z') { min = [-hw, y, hs - 1]; max = [hw, y + H, hs + sd]; pos = [0, y, hs + sd]; }
      else if (dir === '-z') { min = [-hw, y, -hs - sd]; max = [hw, y + H, -hs + 1]; pos = [0, y, -hs - sd]; }
      else if (dir === '+x') { min = [hs - 1, y, -hw]; max = [hs + sd, y + H, hw]; pos = [hs + sd, y, 0]; }
      else { min = [-hs - sd, y, -hw]; max = [-hs + 1, y + H, hw]; pos = [-hs - sd, y, 0]; }
      b.room('box', {min, max});
      b.fill('box', {min: [min[0], y - 2, min[2]], max: [max[0], y, max[2]]}, {mat: 'floor', noise: 'floor', layer: 2});
      b.socket(name, pos, dir, hw * 2, H, 'rect');
      const mid = [Math.round((min[0] + max[0]) / 2), y, Math.round((min[2] + max[2]) / 2)];
      b.lightSpot([mid[0], y + H - 2, mid[2]], 'ceil', '-y');
      return mid;
    };
    const ways = [];
    // The foot: a floor across the shaft at -D (above the sump if any).
    const sump = b.m(P.sump);
    if (sump > 0) {
      b.room('box', {min: [-hs, -D - sump, -hs], max: [hs, -D, hs]}, {liquid: 'liquid', level: -D - 2});
      // planking over the sump
      b.fill('box', {min: [-hs, -D - 1, -hs], max: [hs, -D, hs]}, {mat: 'trim', layer: 3, noise: null});
    } else b.fill('box', {min: [-hs, -D - 2, -hs], max: [hs, -D, hs]}, {mat: 'floor', noise: 'floor', layer: 2});
    const foot = station('foot', -D, '+z');
    ways.push({name: 'foot', pos: foot, sock: 'foot'});
    for (let k = 0; k < P.landings; k++) {
      const y = -D + Math.round((D * (k + 1)) / (P.landings + 1));
      const dir = sideOf[1 + (k % 3)];
      const mid = station('level' + k, y, dir);
      ways.push({name: 'lv' + k, pos: mid, sock: 'level' + k});
    }
    if (P.ladder) b.ladder('mines/ladder', P.length, [0, -D, -hs + 1], 0);
    if (P.cage) b.prop('mines/ore_cart', {style: 'cage'}, [0, -D, 0], 0);
    if (P.surfaceTop) {
      // Stone collar round the mouth, then the headframe over it.
      b.fill('box', {min: [-hs - 4, -b.m(1.2), -hs - 4], max: [hs + 4, top, hs + 4]}, {mat: 'loose', layer: 1, noise: null});
      b.carve('box', {min: [-hs, -b.m(1.2), -hs], max: [hs, top + 1, hs]}, {layer: 2, noise: null, tag: 'door'});
      b.carve('box', {min: [-hs - 8, top, -hs - 8], max: [hs + 8, top + b.m(4), hs + 8]}, {layer: 1, noise: 0.3, blend: 8, tag: 'apron'});
      if (P.headframe) {
        const fh = b.m(5), t = 3;
        for (const s of [-1, 1]) b.fill('box', {min: [s * (hs + 3) - 1, top, -1], max: [s * (hs + 3) + t - 1, top + fh, t - 1]}, {mat: 'support', layer: 3, noise: null});
        b.fill('box', {min: [-hs - 4, top + fh - t, -1], max: [hs + 5, top + fh, t - 1]}, {mat: 'support', layer: 3, noise: null});
        b.fill('cylinder', {min: [-2, top + fh - t - 8, -4], max: [2, top + fh - t, 4], axis: 'x'}, {mat: 'accent', layer: 3, noise: null});
      }
    }
    // Nav: vertical links are a climb (ladder) or a drop. `climb` on a node
    // exempts its links from the slope check (validate).
    const order = ways.slice().sort((a, c) => a.pos[1] - c.pos[1]);
    order.forEach((w, i) => {
      const links = ['socket:' + w.sock];
      if (i > 0) links.push(order[i - 1].name);
      links.push(i + 1 < order.length ? order[i + 1].name : 'top');
      b.way(w.name, w.pos, links, {climb: true, ladder: P.ladder});
    });
    b.way('top', [0, 0, 0], [order[order.length - 1].name, 'socket:top'], {climb: true, ladder: P.ladder});
    b.socket('top', [0, 0, 0], '+y', S, S, 'shaft');
  }
};

const STOPE = {
  type: 'stope', label: 'Stope (ore chamber)', version: 1,
  size: {width: {min: 4, max: 40, def: 12}, length: {min: 4, max: 60, def: 16}, height: {min: 2, max: 20, def: 5}, ceiling: {min: 0, max: 8, def: 1.5}},
  u: {wallRough: 0.7, lightCount: 3, deco: 0.6},
  rows: [
    {sec: 'Ore'},
    {k: 'oreType', n: 'ore', kind: 'enum', options: ['palette', 'iron', 'gold', 'crystal', 'brimstone', 'copper_bar', 'quartzite'], def: 'palette', d: 'Material of the veins (palette = the style\'s ore slot).'},
    {k: 'oreDensity', n: 'vein density', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Ore pockets in the walls and the vein scatter.'},
    {sec: 'Pillars'},
    {k: 'pillarSpacing', n: 'pillar spacing (m)', min: 2, max: 15, step: 0.1, def: 5, d: 'Room-and-pillar grid; pillars are rock left standing.'},
    {k: 'pillarSize', n: 'pillar size (m)', min: 0, max: 4, step: 0.1, def: 1.4, d: '0 = no pillars.'},
    {k: 'robbed', n: 'robbed pillars', min: 0, max: 1, step: 0.05, def: 0.15, d: 'Chance a pillar was mined thin (an hourglass).'},
    {sec: 'Workings'},
    {k: 'benches', n: 'benches', min: 0, max: 4, step: 1, int: true, def: 1, d: 'Terraced floor steps toward the back.'},
    {k: 'scaffolds', n: 'scaffolds', min: 0, max: 6, step: 1, int: true, def: 2, d: 'Timber working platforms against the walls.'},
    {k: 'scaffoldHeight', n: 'scaffold height (m)', min: 1, max: 8, step: 0.1, def: 2.5, d: ''},
    {k: 'exits', n: 'exits', kind: 'enum', options: ['dead end', 'through', 'three'], def: 'through', d: 'Sockets besides the origin.'}
  ],
  PRESETS: {
    pillared: {params: {}},
    crystal_gallery: {params: {oreType: 'crystal', oreDensity: 0.8, height: 8, ceiling: 3, pillarSize: 2.2, lightType: 'crystal', lightCount: 5, benches: 0}},
    gold_bench: {params: {oreType: 'gold', oreDensity: 0.5, pillarSize: 0, benches: 3, scaffolds: 3, width: 10, length: 20}},
    worked_out: {params: {oreDensity: 0.05, robbed: 0.7, decay: 0.7, scaffolds: 1, deco: 0.2, lightCount: 0}}
  },
  build(b, P) {
    const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), dw = b.m(1.4), dh = b.m(2.4), T = b.m(1);
    b.doorway(0, T + 1, 0, 0, dw * 2, dh, 'rect');
    const z0 = T, z1 = T + L;
    const room = b.room('rbox', {min: [-hw, 0, z0], max: [hw, H, z1], r: Math.min(8, Math.round(H / 4))}, {noise: 1.4});
    if (C >= 2) b.room('ellipsoid', {min: [-hw, H - C, z0], max: [hw, H + C, z1]}, {noise: 1.4});
    b.floor(-hw, z0, hw, z1, 0);
    const oreMat = P.oreType === 'palette' ? 'ore' : P.oreType;
    if (P.oreDensity > 0) {
      b.scatter.push({prim: room, kind: 'ore_vein', density: +(P.oreDensity * 0.9).toFixed(3), size: [2, 6], surface: 'wall', mat: oreMat, seedKey: 'ore'});
      const nPock = Math.round(P.oreDensity * 10);
      for (let i = 0; i < nPock; i++) {
        const side = b.pick(['l', 'r', 'b'], 'pocket', i, 'side');
        const r = b.ri(4, 9, 'pocket', i, 'r');
        const y = b.ri(r, Math.max(r + 1, H - r), 'pocket', i, 'y');
        const z = side === 'b' ? z1 : b.ri(z0 + 6, z1 - 6, 'pocket', i, 'z');
        const x = side === 'l' ? -hw : side === 'r' ? hw : b.ri(-hw + 6, hw - 6, 'pocket', i, 'x');
        b.fill('ellipsoid', {min: [x - r, y - r, z - r], max: [x + r, y + r, z + r]}, {mat: oreMat, layer: 1, noise: 0.5});
      }
    }
    // Benches: the floor steps up toward the back.
    const bh = b.m(0.8);
    for (let k = 1; k <= P.benches; k++) {
      const zb = z0 + Math.round(L * (1 - k / (P.benches + 1)));
      b.fill('box', {min: [-hw - 4, 0, zb], max: [hw + 4, k * bh, z1 + 4]}, {mat: 'wall', layer: 2, noise: 0.5, mask: 'air'});
    }
    const benchY = (z) => {
      let y = 0;
      for (let k = 1; k <= P.benches; k++) if (z >= z0 + Math.round(L * (1 - k / (P.benches + 1)))) y = k * bh;
      return y;
    };
    // Pillar grid.
    const ps = b.m(P.pillarSize), sp = b.m(P.pillarSpacing, 20);
    const pillars = [];
    if (ps > 0) {
      const nx = Math.max(0, Math.floor((W - sp * 0.6) / sp)), nz = Math.max(0, Math.floor((L - sp * 0.6) / sp));
      for (let ix = 0; ix < nx; ix++) for (let iz = 0; iz < nz; iz++) {
        const x = Math.round(-hw + (W * (ix + 1)) / (nx + 1)) + b.jit(sp * 0.2, 'pil', ix, iz, 'x');
        const z = Math.round(z0 + (L * (iz + 1)) / (nz + 1)) + b.jit(sp * 0.2, 'pil', ix, iz, 'z');
        const h = Math.round(ps / 2);
        b.fill('rbox', {min: [x - h, -1, z - h], max: [x + h, H + C + 2, z + h], r: Math.round(h / 3)}, {mat: 'wall', layer: 2, noise: 0.6});
        if (b.r('pil', ix, iz, 'robbed') < P.robbed)
          b.carve('box', {min: [x - h - 2, Math.round(H * 0.35), z - h - 2], max: [x + h + 2, Math.round(H * 0.65), z + h + 2]}, {layer: 3, noise: 0.8, blend: 3});
        pillars.push([x, z, h]);
        b.lightSpot([x, Math.round(H * 0.55), z + h], 'wall', '+z');
      }
    }
    // Scaffolds: four posts and a deck against a wall.
    const sh = b.m(P.scaffoldHeight, 6);
    for (let i = 0; i < P.scaffolds; i++) {
      const side = b.r('scaf', i, 'side') < 0.5 ? -1 : 1;
      const z = b.ri(z0 + 8, Math.max(z0 + 9, z1 - 20), 'scaf', i, 'z');
      const len = b.m(3), dep = b.m(1.4);
      const xa = side < 0 ? -hw : hw - dep, xb = xa + dep;
      const y0 = benchY(z);
      for (const zz of [z, z + len - 2]) for (const xx of [xa, xb - 2])
        b.fill('box', {min: [xx, y0, zz], max: [xx + 2, y0 + sh, zz + 2]}, {mat: 'support', layer: 3, noise: null, mask: 'air'});
      b.fill('box', {min: [xa, y0 + sh, z], max: [xb, y0 + sh + 1, z + len]}, {mat: 'trim', layer: 3, noise: null, mask: 'air'});
      b.prop('mines/ladder', {length: P.scaffoldHeight + 0.2}, [side < 0 ? xb : xa - 1, y0, z + Math.round(len / 2)], side < 0 ? 90 : 270);
    }
    // Props: an ore cart by the door, a tool rack, crates.
    b.prop('mines/ore_cart', {load: P.oreType === 'palette' ? 'ore' : P.oreType}, [b.m(1.2), 0, z0 + b.m(2)], 0);
    const nd = b.deco(2);
    for (let i = 0; i < nd; i++) {
      const what = b.pick(['mines/pickaxe_rack', 'mines/crate', 'mines/barrel'], 'deco', i);
      const side = b.r('deco', i, 'side') < 0.5 ? -1 : 1;
      const z = b.ri(z0 + 6, Math.max(z0 + 7, z0 + Math.round(L / 2)), 'deco', i, 'z');
      b.prop(what, {}, [side * (hw - 6), benchY(z), z], side < 0 ? 90 : 270);
    }
    b.lightSpot([-hw + 3, b.m(1.8), z0 + b.m(1.5)], 'wall', '+x');
    b.lightSpot([hw - 3, b.m(1.8), z1 - b.m(1.5)], 'wall', '-x');
    // Sockets + nav.
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'rect');
    const zc = Math.round((z0 + z1) / 2);
    const ins = ['socket:in', 'c'];
    b.way('door', [0, 0, Math.round(T / 2) + 2], ins);
    const cLinks = ['door'];
    if (P.exits !== 'dead end') {
      const yb = benchY(z1 - 2);
      b.doorway(z1 - 1, z1 + T, 0, yb, dw * 2, dh, 'rect');
      b.socket('out', [0, yb, z1 + T], '+z', dw * 2, dh, 'rect');
      b.way('back', [0, yb, z1 - b.m(1)], ['c', 'socket:out']); cLinks.push('back');
    }
    if (P.exits === 'three') {
      const y = benchY(zc);
      b.carve('box', {min: [hw - 1, y, zc - dw], max: [hw + T, y + dh, zc + dw]}, {tag: 'door'});
      b.socket('side', [hw + T, y, zc], '+x', dw * 2, dh, 'rect');
      b.way('east', [hw - b.m(1), y, zc], ['c', 'socket:side']); cLinks.push('east');
    }
    b.way('c', [pillars.length && pillars.some(p => Math.abs(p[0]) < p[2] + 3 && Math.abs(p[1] - zc) < p[2] + 3) ? -hw + b.m(1.5) : 0, benchY(zc), zc], cLinks);
  }
};

const COLLAPSE = {
  type: 'collapse', label: 'Collapse (cave-in)', version: 1,
  size: {width: {min: 1.5, max: 6, def: 2.6}, length: {min: 4, max: 40, def: 12}, height: {min: 1.6, max: 5, def: 2.4}, ceiling: {min: 0, max: 2, def: 0.3}},
  u: {decay: 0.8, lightCount: 0, moisture: 0.3},
  rows: ROWS_TIMBER.concat([
    {sec: 'Cave-in'},
    {k: 'blockage', n: 'blockage', min: 0, max: 1, step: 0.05, def: 0.7, d: 'How high the rubble cone reaches, as a fraction of the drift height (1 = sealed; it is loose gravel, so dig through).'},
    {k: 'coneLength', n: 'cone length (m)', min: 2, max: 30, step: 0.1, def: 8, d: 'Footprint of the cone along the drift; the cone stays at the angle of repose, so a short cone cannot be tall.'},
    {k: 'boulders', n: 'boulders', min: 0, max: 12, step: 1, int: true, def: 3, d: 'Rock blocks stuck in the cone.'},
    {k: 'cavity', n: 'roof cavity (m)', min: 0, max: 8, step: 0.1, def: 2, d: 'The hole the roof fell out of, above the cone.'},
    {k: 'looseCeiling', n: 'loose ceiling trap', kind: 'bool', def: false, d: 'Suggests a collapse trap over the cone (the trap turns the roof slab to powder).'}
  ]),
  PRESETS: {
    partial: {params: {}},
    sealed: {params: {blockage: 1, coneLength: 14, boulders: 6, cavity: 3.5}},
    fresh_fall: {params: {blockage: 0.4, cavity: 4, looseCeiling: true, broken: 0.8}},
    old_choke: {params: {blockage: 0.85, boulders: 1, cavity: 0.5, moisture: 0.8, decay: 1}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 14), L = b.m(P.length, 20), C = b.m(P.ceiling);
    const {room, hw} = driftBody(b, P, 0, L, W, H, C, 'c', {extraBroken: 0.5});
    void room;
    const zc = Math.round(L / 2) + b.jit(Math.round(L / 6), 'conez');
    const rz = Math.round(b.m(P.coneLength, 8) / 2);
    const want = Math.round(P.blockage * (H + C));
    b.mound(b.jit(3, 'conex'), 0, zc, hw + b.m(1), rz, want);
    for (let i = 0; i < P.boulders; i++) {
      const r = b.ri(3, 7, 'boulder', i, 'r');
      const x = b.ri(-hw + r, hw - r, 'boulder', i, 'x'), z = zc + b.ri(-rz + r, rz - r, 'boulder', i, 'z');
      b.fill('ellipsoid', {min: [x - r, -1, z - r], max: [x + r, Math.round(r * 1.4), z + r]}, {mat: 'loose', layer: 4, noise: 0.5});
    }
    const cav = b.m(P.cavity);
    if (cav > 0) b.carve('ellipsoid', {min: [-hw - 2, H - 2, zc - rz], max: [hw + 2, H + C + cav, zc + rz]}, {noise: 1.5, layer: 1, tag: 'room'});
    if (P.looseCeiling)
      b.ref('trap', [0, H + C, zc], {name: 'roof_fall', props: {type: 'collapse_floor', region: {min: [-hw, H + C, zc - rz], max: [hw, H + C + Math.max(4, cav), zc + rz]}, on: 'plate', note: 'ceiling slab over the cone'}});
    const top = Math.min(H + C - b.m(0.6), Math.round(Math.min(want, (b.kit.repose * Math.min(hw + b.m(1), rz)) / 3)));
    b.wayChain('w', [[0, 0, 3], [0, Math.max(0, top), zc], [0, 0, L - 3]], ['socket:in'], ['socket:out'], {dig: P.blockage >= 0.95});
    b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
    b.socket('out', [0, 0, L], '+z', W, H, 'rect');
  }
};

const SUMP = {
  type: 'flooded_sump', label: 'Flooded sump', version: 1,
  size: {width: {min: 3, max: 30, def: 8}, length: {min: 4, max: 40, def: 10}, height: {min: 2, max: 10, def: 3.5}, ceiling: {min: 0, max: 5, def: 1}},
  u: {moisture: 0.8, lightCount: 1, wallRough: 0.6},
  rows: [
    {sec: 'Water'},
    {k: 'sumpDepth', n: 'sump depth (m)', min: 0.5, max: 10, step: 0.1, def: 2.5, d: 'Depth of the pit below the floor.'},
    {k: 'waterLevel', n: 'water level', min: 0, max: 1, step: 0.05, def: 0.85, d: 'Fraction of the pit depth filled. The water is born full and flat (at rest).'},
    {k: 'sills', n: 'door sills', kind: 'bool', def: false, d: 'Raise sills at the doors so the water may stand above the floor (otherwise it is capped one voxel under the floor so it cannot run out).'},
    {k: 'ledge', n: 'walkway (m)', min: 0, max: 4, step: 0.1, def: 1.2, d: 'Dry ledge along the left wall; 0 = swim.'},
    {k: 'pumpGear', n: 'pump gear', kind: 'bool', def: true, d: 'Barrels and a ladder by the pit (deco).'}
  ],
  PRESETS: {
    sump: {params: {}},
    drowned_level: {params: {sumpDepth: 4, waterLevel: 1, sills: true, ledge: 0, width: 12, length: 16}},
    shallow_pit: {params: {sumpDepth: 1.2, waterLevel: 0.6, ledge: 2, decay: 0.5}}
  },
  build(b, P) {
    const W = b.m(P.width, 20), L = b.m(P.length, 20), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), T = b.m(1), dw = b.m(1.3), dh = b.m(2.4);
    b.doorway(0, T + 1, 0, 0, dw * 2, dh, 'rect');
    const z0 = T, z1 = T + L;
    const roomI = b.room('rbox', {min: [-hw, 0, z0], max: [hw, H, z1], r: 6});
    if (C >= 2) b.room('ellipsoid', {min: [-hw, H - C, z0], max: [hw, H + C, z1]});
    const led = b.m(P.ledge), SD = b.m(P.sumpDepth, 4);
    const px0 = -hw + led, pz0 = z0 + b.m(1), pz1 = z1 - b.m(1);
    b.floor(-hw, z0, hw, z1, 0);
    let lvl = -SD + Math.round(P.waterLevel * SD);
    if (P.sills) {
      lvl = Math.min(lvl, b.m(0.5));
      const sh = Math.max(0, lvl + 1);
      if (sh > 0) for (const z of [z0, z1 - 2]) b.fill('box', {min: [-hw, 0, z], max: [hw, sh, z + 2]}, {mat: 'wall', layer: 4, noise: null});
    } else lvl = Math.min(lvl, -1);
    // Water = the carve's liquid up to a flat level (PLAN 3.4.1 `level`): born full and at rest.
    const wet = lvl > -SD ? {liquid: 'liquid', level: lvl - 1} : {};
    b.carve('rbox', {min: [px0, -SD, pz0], max: [hw, 1, pz1], r: 4}, Object.assign({layer: 3, noise: 0.8, blend: 2}, wet));
    if (lvl > 0) { b.prims[roomI].mat = 'liquid'; b.prims[roomI].level = lvl - 1; }   // standing over the floor, behind the sills
    if (P.pumpGear && P.deco > 0.2) {
      b.prop('mines/barrel', {}, [-hw + 3, 0, z0 + 4], 90);
      b.ladder('mines/ladder', P.sumpDepth + 0.3, [px0 + 2, -SD, Math.round((pz0 + pz1) / 2)], 90);
    }
    b.lightSpot([-hw + 2, b.m(1.8), Math.round((z0 + z1) / 2)], 'wall', '+x');
    b.lightSpot([hw - 2, b.m(1.8), Math.round((z0 + z1) / 2)], 'wall', '-x');
    b.doorway(z1 - 1, z1 + T, 0, 0, dw * 2, dh, 'rect');
    const lx = led > 3 ? -hw + Math.round(led / 2) : 0;
    b.wayChain('w', [[0, 0, Math.round(T / 2)], [lx, 0, z0 + 2], [lx, 0, z1 - 2], [0, 0, z1 + Math.round(T / 2)]], ['socket:in'], ['socket:out'], {swim: led <= 3});
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'rect');
    b.socket('out', [0, 0, z1 + T], '+z', dw * 2, dh, 'rect');
  }
};

const CAMP = {
  type: 'camp', label: 'Miners\' camp', version: 1,
  size: {width: {min: 3, max: 16, def: 6}, length: {min: 3, max: 20, def: 8}, height: {min: 2, max: 5, def: 2.8}, ceiling: {min: 0, max: 3, def: 0.5}},
  u: {deco: 0.7, lightCount: 2, decay: 0.15},
  rows: ROWS_TIMBER.concat([
    {sec: 'Furnishing'},
    {k: 'bunks', n: 'bunks', min: 0, max: 8, step: 1, int: true, def: 3, d: 'Timber bunks along the walls.'},
    {k: 'table', n: 'table', kind: 'bool', def: true, d: 'A table with benches in the middle.'},
    {k: 'crates', n: 'crates + barrels', min: 0, max: 10, step: 1, int: true, def: 4, d: 'Stores along the back wall.'},
    {k: 'stove', n: 'stove', kind: 'bool', def: true, d: 'A brazier for warmth (a light).'},
    {k: 'tools', n: 'tool racks', min: 0, max: 4, step: 1, int: true, def: 1, d: ''},
    {k: 'exits', n: 'exits', kind: 'enum', options: ['dead end', 'through', 'side'], def: 'dead end', d: ''}
  ]),
  PRESETS: {
    bunkroom: {params: {}},
    stores: {params: {bunks: 0, crates: 10, table: false, stove: false, tools: 3, width: 5, length: 10}},
    ransacked: {params: {decay: 0.8, deco: 0.3, broken: 0.5, stove: false, lightCount: 0}},
    foremans_room: {params: {bunks: 1, table: true, crates: 2, tools: 0, width: 5, length: 6, lightType: 'candles', exits: 'through'}}
  },
  build(b, P) {
    const W = b.m(P.width, 20), L = b.m(P.length, 20), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), T = b.m(1), dw = b.m(1.2), dh = b.m(2.3);
    b.doorway(0, T + 1, 0, 0, dw * 2, dh, 'rect');
    const z0 = T, z1 = T + L;
    b.room('box', {min: [-hw, 0, z0], max: [hw, H, z1]});
    if (C >= 2) b.room('vault', {min: [-hw + 2, H - 1, z0], max: [hw - 2, H + C, z1], axis: 'z', rise: C + 1});
    b.floor(-hw, z0, hw, z1, 0, {mat: 'trim'});
    const t = b.m(P.beam, 2), S = Math.max(t + 4, b.m(P.setSpacing));
    for (let i = 0, z = z0 + 2; z + t <= z1; i++, z += S) timberSet(b, P, 'camp', i, z, hw, H, t);
    // Bunks along the left then right wall, head to the wall, foot into the room.
    const bl = b.m(2), bw = b.m(0.9);
    let placed = 0;
    for (let i = 0; i < P.bunks; i++) {
      const side = i % 2 ? 1 : -1;
      const row = Math.floor(i / 2);
      const z = z0 + b.m(1) + row * (bw + 4);
      if (z + bw > z1 - b.m(1.2)) break;
      b.prop('mines/bunk', {}, [side * (hw - Math.round(bl / 2) - 1), 0, z + Math.round(bw / 2)], side < 0 ? 90 : 270);
      b.ref('marker', [side * (hw - Math.round(bl / 2) - 1), 0, z + Math.round(bw / 2)], {name: 'bunk' + i, props: {tags: ['bed']}});
      placed++;
    }
    if (placed < P.bunks) b.notes.push((P.bunks - placed) + ' bunk(s) did not fit');
    const zc = Math.round((z0 + z1) / 2);
    if (P.table) b.prop('mines/table', {benches: true}, [0, 0, zc], b.r('tableyaw') < 0.5 ? 0 : 90);
    for (let i = 0; i < P.crates; i++) {
      const x = -hw + 6 + i * b.m(1.1);
      if (x > hw - 6) break;
      const what = b.r('crate', i) < 0.6 ? 'mines/crate' : 'mines/barrel';
      b.prop(what, {}, [x, 0, z1 - 6], 180);
      if (what === 'mines/crate' && i === 0) b.ref('container', [x, 0, z1 - 6], {name: 'stores', props: {loot: 'mine_camp'}});
    }
    for (let i = 0; i < P.tools; i++) b.prop('mines/pickaxe_rack', {}, [hw - 2, 0, z0 + b.m(1) + i * b.m(1.3)], 270);
    if (P.stove) {
      b.prop('ruins/brazier', {style: 'iron', lit: 'light'}, [Math.round(hw / 2), 0, z1 - b.m(1.5)], 0, {light: true});
      b.ref('light', [Math.round(hw / 2), 3, z1 - b.m(1.5)], {name: 'stove', props: {type: 'brazier', warm: true}});
    }
    b.lightSpot([0, H - t - 1, z0 + 2], 'ceil', '-y');
    b.lightSpot([0, H - t - 1, zc], 'ceil', '-y');
    b.lightSpot([-hw + 1, b.m(1.8), zc], 'wall', '+x');
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'rect', ['door']);
    const links = ['door'];
    b.way('door', [0, 0, Math.round(T / 2) + 1], ['socket:in', 'c']);
    if (P.exits === 'through') {
      b.doorway(z1 - 1, z1 + T, 0, 0, dw * 2, dh, 'rect');
      b.socket('out', [0, 0, z1 + T], '+z', dw * 2, dh, 'rect');
      b.way('back', [0, 0, z1 - 2], ['c', 'socket:out']); links.push('back');
    } else if (P.exits === 'side') {
      b.carve('box', {min: [hw - 1, 0, zc - dw], max: [hw + T, dh, zc + dw]}, {tag: 'door'});
      b.socket('side', [hw + T, 0, zc], '+x', dw * 2, dh, 'rect');
      b.way('east', [hw - 3, 0, zc], ['c', 'socket:side']); links.push('east');
    }
    b.way('c', [P.table ? -Math.round(hw / 2) : 0, 0, zc], links);
  }
};

const ADIT = {
  type: 'adit_entrance', label: 'Adit entrance', version: 1, surface: true,
  size: {width: {min: 1.5, max: 6, def: 2.8}, length: {min: 2, max: 40, def: 8, n: 'tunnel length (m)', d: 'From the portal to the inner socket.'},
         height: {min: 1.6, max: 5, def: 2.6}, ceiling: {min: 0, max: 2, def: 0.3}},
  u: {lightCount: 2, lightType: 'lantern', decay: 0.2},
  rows: ROWS_TIMBER.concat(ROWS_RAIL, [
    {sec: 'Portal + cut', note: 'The cut is the trench dug into the slope to reach the portal: the terrain-blend apron. The anchor (origin socket) is the outer end of the cut, at grade.'},
    {k: 'cutLength', n: 'cut length (m)', min: 0, max: 20, step: 0.1, def: 5, d: 'Open trench from the anchor to the portal.'},
    {k: 'cutHeight', n: 'cut face height (m)', min: 1, max: 20, step: 0.1, def: 6, d: 'How high the cut clears the hillside above the trench floor.'},
    {k: 'cutFlare', n: 'cut flare', min: 0, max: 1, step: 0.05, def: 0.5, d: 'How much the trench walls lean back with height (0 = vertical).'},
    {k: 'portal', n: 'portal', kind: 'enum', options: ['timber', 'stone', 'none'], def: 'timber', d: 'Doubled timber set, a stone arch collar, or bare rock.'},
    {k: 'headboard', n: 'headboard sign', kind: 'bool', def: true, d: 'A plank sign over the portal (a marker ref carries its text).'},
    {k: 'boarded', n: 'boarded up', min: 0, max: 1, step: 0.05, def: 0, d: 'Planks nailed across the portal (fraction of its height; leave a gap to squeeze through).'},
    {k: 'spoilHeap', n: 'spoil heap', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Waste rock tipped beside the cut.'},
    {k: 'railsOut', n: 'rails out', kind: 'bool', def: true, d: 'The track runs out through the cut.'}
  ]),
  PRESETS: {
    working: {params: {}},
    abandoned: {params: {decay: 0.7, boarded: 0.6, broken: 0.4, spoilHeap: 0.8, lightCount: 0, headboard: true, moisture: 0.5}},
    stone_portal: {params: {portal: 'stone', width: 3.4, height: 3.2, ceiling: 1, cutLength: 8, cutHeight: 9, cutFlare: 0.2, railsOut: true}},
    prospect_hole: {params: {width: 1.8, height: 1.9, length: 4, cutLength: 2, cutHeight: 3, portal: 'none', rails: false, railsOut: false, spoilHeap: 0.3, headboard: false}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 14), L = b.m(P.length, 10), C = b.m(P.ceiling);
    const CL = b.m(P.cutLength), CH = b.m(P.cutHeight, 10), hw = Math.round(W / 2);
    // The cut: stepped trench whose walls lean back with height, open to the sky.
    const steps = 4, sh = Math.ceil(CH / steps);
    for (let k = 0; k <= steps; k++) {
      const half = hw + 2 + Math.round(k * sh * P.cutFlare);
      const y0 = k * sh, y1 = k === steps ? CH + b.m(8) : (k + 1) * sh;
      b.carve('box', {min: [-half, y0, -b.m(2)], max: [half, y1, CL + (k === 0 ? 1 : 0)]}, {layer: 1, noise: 0.6, blend: 6, tag: k === 0 ? 'room' : 'apron'});
    }
    // Apron floor out of the cut: a slab with a little berm under it.
    b.fill('box', {min: [-hw - 4, -b.m(1.5), -b.m(4)], max: [hw + 4, 0, CL]}, {mat: 'floor', noise: 'floor', layer: 2, mask: 'air'});
    b.floor(-hw - 2, -b.m(4), hw + 2, CL, 0);
    // The tunnel.
    driftBody(b, P, CL, CL + L, W, H, C, 'adit');
    const t = b.m(P.beam, 2);
    if (P.portal === 'timber') {
      for (const z of [CL, CL + t + 1]) {
        b.fill('box', {min: [-hw - t, 0, z], max: [-hw, H + t, z + t]}, {mat: 'support', layer: 3, noise: null});
        b.fill('box', {min: [hw, 0, z], max: [hw + t, H + t, z + t]}, {mat: 'support', layer: 3, noise: null});
        b.fill('box', {min: [-hw - t - 2, H, z], max: [hw + t + 2, H + t, z + t]}, {mat: 'support', layer: 3, noise: null});
      }
    } else if (P.portal === 'stone') {
      const rise = Math.max(4, C);
      b.fill('vault', {min: [-hw - 4, 0, CL - 1], max: [hw + 4, H + rise + 4, CL + 5], axis: 'z', rise: rise + 4}, {mat: 'loose', layer: 2, noise: null});
      b.carve('vault', {min: [-hw, 0, CL - 2], max: [hw, H + rise, CL + 6], axis: 'z', rise}, {layer: 3, noise: null, blend: 0, tag: 'door'});
    }
    if (P.headboard) {
      b.fill('box', {min: [-b.m(0.6), H + t + 1, CL - 1], max: [b.m(0.6), H + t + 5, CL]}, {mat: 'trim', layer: 3, noise: null});
      b.ref('marker', [0, H + t + 3, CL - 1], {name: 'sign', props: {tags: ['sign'], text: 'MINE'}});
    }
    if (P.boarded > 0) {
      const top = Math.round(H * P.boarded);
      for (let y = 1, i = 0; y < top; y += 3, i++) {
        if (b.r('board', i) < 0.15 + P.decay * 0.3) continue;      // missing / broken boards
        b.fill('box', {min: [-hw - 1, y, CL + t + 1], max: [hw + 1, y + 2, CL + t + 2]}, {mat: 'trim', layer: 4, noise: null});
      }
    }
    if (P.spoilHeap > 0) {
      const side = b.r('spoilside') < 0.5 ? -1 : 1;
      const R = Math.round(b.m(1.5) + P.spoilHeap * b.m(3.5));
      b.mound(side * (hw + R), 0, -Math.round(R * 0.6), R, R, R, {mat: 'spoil'});
    }
    if (P.rails) rails(b, P.railsOut ? -b.m(3) : CL, CL + L, b.m(P.gauge, 4), b.m(P.sleeperSpacing, 3), 0, 'adit');
    b.lightSpot([-hw - t - 1, H - 2, CL - 1], 'wall', '-z');
    b.lightSpot([hw + t + 1, H - 2, CL - 1], 'wall', '-z');
    b.wayChain('w', [[0, 0, -b.m(2)], [0, 0, CL], [0, 0, CL + Math.round(L / 2)], [0, 0, CL + L - 2]], ['socket:surface'], ['socket:in']);
    b.socket('surface', [0, 0, 0], '-z', W + 4, CH, 'rect', ['surface']);
    b.socket('in', [0, 0, CL + L], '+z', W, H, 'rect');
  }
};

defineKit(KIT, [ADIT, DRIFT, JUNCTION, INCLINE, SHAFT, STOPE, COLLAPSE, SUMP, CAMP]);
export default KIT;
