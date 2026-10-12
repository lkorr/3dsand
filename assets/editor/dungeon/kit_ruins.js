/* kit_ruins.js — the RUINS kit (PLAN_dungeons §4): a ruined temple. Dressed
 * stone, symmetry, decayed grandeur.
 *
 * Every piece: origin socket 'in' (or 'surface' for the entrance) at (0,0,0)
 * facing -Z, floor at y = 0, body to +Z. Rooms sit behind a doorway cut
 * through a wall of `wallThick`, so a 3 m corridor meets a 14 m hall through
 * a door-sized socket instead of a hall-sized one.
 *
 * Walls are SKINNED: a `skin` prim turns the natural rock round a carve into
 * dressed `wall` stone, so the temple reads as built, not dug.
 */
'use strict';
import { defineKit } from './dgutil.js';

export const KIT = {
  name: 'ruins', label: 'Ruins (temple)',
  roughMax: 1.5, floorMax: 1.5, octaves: 1, repose: 0.9,
  lightTypes: ['brazier', 'candles', 'torch', 'crystal', 'none'],
  lights: {brazier: {prop: 'ruins/brazier'}, candles: {prop: 'ruins/candle_cluster'}, lantern: {prop: 'mines/lantern'}},
  PALETTES: {
    temple: {wall: 'cobble', floor: 'flagstone', ceil: 'cobble', trim: 'limestone', support: 'limestone',
             rubble: 'gravel', accent: 'gold', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
             ore: 'gold', moss: 'old_moss', tile2: 'chalk_rock', veg: 'ivy', statue: 'quartzite', cloth: 'cloth', loose: 'cobble'},
    sandstone: {wall: 'sandstone', floor: 'red_sandstone', ceil: 'sandstone', trim: 'sandstone', support: 'sandstone',
                rubble: 'sand', accent: 'gold', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
                ore: 'gold', moss: 'old_moss', tile2: 'sandstone', veg: 'vine_hang', statue: 'red_sandstone', cloth: 'linen', loose: 'sandstone'},
    dark: {wall: 'basalt', floor: 'flagstone', ceil: 'basalt', trim: 'quartzite', support: 'basalt',
           rubble: 'cinder', accent: 'brass', light: 'candle_flame', lightCool: 'gem_arcane', liquid: 'water',
           ore: 'gold', moss: 'old_moss', tile2: 'basalt', veg: 'moss_hang', statue: 'basalt', cloth: 'cloth', loose: 'basalt'},
    marble: {wall: 'quartzite', floor: 'chalk_rock', ceil: 'quartzite', trim: 'chalk_rock', support: 'quartzite',
             rubble: 'gravel', accent: 'gold', light: 'candle_flame', lightCool: 'crystal', liquid: 'water',
             ore: 'gold', moss: 'old_moss', tile2: 'quartzite', veg: 'ivy', statue: 'chalk_rock', cloth: 'linen', loose: 'quartzite'}
  },
  DEFAULT_PALETTE: 'temple',
  /** Dead-end pieces grow() may cap an open socket with. */
  CAPS: [{type: 'crypt', params: {exits: 'dead end'}}, {type: 'sanctum', params: {secretDoor: false}}],
  GROW: {corridor: 5, hall: 2, stair: 1.5, crypt: 1.5, pit_room: 0.8, sanctum: 0.5, flooded_hall: 0.6, collapsed_passage: 0.8},
  TYPES: {}
};

const ROWS_DOOR = [
  {sec: 'Doors', note: 'The room is entered through a doorway cut in a wall; the socket is the doorway, not the room.'},
  {k: 'doorWidth', n: 'door width (m)', min: 1, max: 6, step: 0.1, def: 2.2, d: ''},
  {k: 'doorHeight', n: 'door height (m)', min: 1.8, max: 8, step: 0.1, def: 3, d: ''},
  {k: 'wallThick', n: 'wall thickness (m)', min: 0.4, max: 3, step: 0.1, def: 1, d: ''}
];
const TILE_ROWS = [
  {k: 'tilePattern', n: 'floor pattern', kind: 'enum', options: ['plain', 'checker', 'border', 'runner'], def: 'border',
   d: 'checker = a two-material checker (a prim `pattern`); border = a contrasting band along the walls; runner = a band down the middle.'},
  {k: 'tileSize', n: 'tile size (m)', min: 0.3, max: 3, step: 0.1, def: 1, d: 'Checker square size.'}
];

/** A column. Returns {broken, h}. style: round|square|fluted|broken. */
function column(b, P, x, z, y0, H, R, style, key) {
  const broken = style === 'broken' || b.r('col', key, 'broken') < P.decay * 0.35;
  const h = broken ? Math.round(H * (0.2 + 0.5 * b.r('col', key, 'h'))) : H;
  const base = Math.max(1, Math.round(R * 0.4));
  b.fill('box', {min: [x - R - 2, y0, z - R - 2], max: [x + R + 2, y0 + base, z + R + 2]}, {mat: 'trim', layer: 2, noise: null});
  if (style === 'square') b.fill('box', {min: [x - R, y0, z - R], max: [x + R, y0 + h, z + R]}, {mat: 'support', layer: 2, noise: null});
  else b.fill('cylinder', {min: [x - R, y0, z - R], max: [x + R, y0 + h, z + R], axis: 'y'}, {mat: 'support', layer: 2, noise: null});
  if (style === 'fluted' && R >= 3) {
    for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const cx = x + dx * R, cz = z + dz * R;
      b.carve('box', {min: [cx - 1 - (dx < 0 ? 0 : 0), y0 + base + 1, cz - 1], max: [cx + 1, y0 + h - base - 1, cz + 1]}, {layer: 3, noise: null, blend: 0, tag: 'detail'});
    }
  }
  if (!broken) b.fill('box', {min: [x - R - 1, y0 + h - base, z - R - 1], max: [x + R + 1, y0 + h, z + R + 1]}, {mat: 'trim', layer: 2, noise: null});
  else {
    // The fallen drum lies beside the stump; a little rubble.
    const ax = b.r('col', key, 'ax') < 0.5 ? 'x' : 'z';
    const len = Math.round(R * (2 + 2 * b.r('col', key, 'len')));
    const off = (b.r('col', key, 'side') < 0.5 ? -1 : 1) * (R * 2 + 2);
    const cx = ax === 'z' ? x + off : x, cz = ax === 'x' ? z + off : z;
    const mn = ax === 'x' ? [cx - len, y0, cz - R] : [cx - R, y0, cz - len];
    const mx = ax === 'x' ? [cx + len, y0 + 2 * R, cz + R] : [cx + R, y0 + 2 * R, cz + len];
    b.fill('cylinder', {min: mn, max: mx, axis: ax}, {mat: 'support', layer: 4, noise: null, mask: 'air'});
    b.mound(x, y0, z, R * 2 + 2, R * 2 + 2, R, {mat: 'rubble'});
  }
  return {broken, h};
}

function floorPattern(b, P, x0, z0, x1, z1, y, aisle) {
  const pat = P.tilePattern;
  const o = {};
  if (pat === 'checker') o.pattern = {kind: 'checker', size: b.m(P.tileSize, 3), mat2: 'tile2'};
  b.floor(x0, z0, x1, z1, y, o);
  const bw = b.m(0.6, 2);
  if (pat === 'border') {
    const m = {mat: 'tile2', layer: 3, noise: null, th: 1};
    b.fill('box', {min: [x0 + bw, y - 1, z0 + bw], max: [x1 - bw, y, z0 + 2 * bw]}, m);
    b.fill('box', {min: [x0 + bw, y - 1, z1 - 2 * bw], max: [x1 - bw, y, z1 - bw]}, m);
    b.fill('box', {min: [x0 + bw, y - 1, z0 + bw], max: [x0 + 2 * bw, y, z1 - bw]}, m);
    b.fill('box', {min: [x1 - 2 * bw, y - 1, z0 + bw], max: [x1 - bw, y, z1 - bw]}, m);
  } else if (pat === 'runner') {
    const hw = Math.max(bw, Math.round((aisle || b.m(2)) / 4));
    b.fill('box', {min: [-hw, y - 1, z0], max: [hw, y, z1]}, {mat: 'tile2', layer: 3, noise: null});
  }
}

/** Ceiling falls: a hole up into the vault, a rubble heap, a few blocks. */
function ceilingFalls(b, P, n, x0, z0, x1, z1, fy, H, key) {
  for (let i = 0; i < n; i++) {
    const r = b.ri(b.m(1.2), b.m(2.8), 'fall', key, i, 'r');
    const x = b.ri(x0 + r, Math.max(x0 + r + 1, x1 - r), 'fall', key, i, 'x');
    const z = b.ri(z0 + r, Math.max(z0 + r + 1, z1 - r), 'fall', key, i, 'z');
    b.carve('ellipsoid', {min: [x - r, H - r, z - r], max: [x + r, H + r * 2, z + r]}, {noise: 3, layer: 1, tag: 'room'});
    b.mound(x, fy, z, Math.round(r * 1.6), Math.round(r * 1.6), r);
    const nb = b.ri(1, 3, 'fall', key, i, 'nb');
    for (let k = 0; k < nb; k++) {
      const s = b.ri(3, 7, 'fall', key, i, 'blk', k);
      const bx = x + b.ri(-r, r, 'fall', key, i, 'bx', k), bz = z + b.ri(-r, r, 'fall', key, i, 'bz', k);
      b.fill('box', {min: [bx, fy, bz], max: [bx + s, fy + Math.max(2, s - 2), bz + Math.round(s * 1.3)]}, {mat: 'wall', layer: 4, noise: null, mask: 'air'});
    }
  }
}

// ---------------------------------------------------------------------------
// hall (shared by flooded_hall)
// ---------------------------------------------------------------------------
const HALL_ROWS = ROWS_DOOR.concat([
  {sec: 'Vault'},
  {k: 'vault', n: 'vault', kind: 'enum', options: ['flat', 'barrel', 'groin', 'dome'], def: 'barrel', d: 'groin = a barrel crossed by a vault per bay between columns.'},
  {sec: 'Columns'},
  {k: 'columnRows', n: 'column rows', kind: 'enum', options: ['0', '2', '4'], def: '2', d: 'Rows of columns (in pairs either side of the aisle).'},
  {k: 'columnCount', n: 'columns per row', min: 1, max: 16, step: 1, int: true, def: 4, d: ''},
  {k: 'columnRadius', n: 'column radius (m)', min: 0.2, max: 1.5, step: 0.05, def: 0.45, d: ''},
  {k: 'columnStyle', n: 'column style', kind: 'enum', options: ['round', 'square', 'fluted', 'broken'], def: 'round', d: 'broken = every column is a stump (decay breaks some of any style).'},
  {k: 'aisleWidth', n: 'aisle width (m)', min: 1, max: 20, step: 0.1, def: 5, d: 'Distance between the two inner rows.'},
  {sec: 'Furnishing'},
  {k: 'dais', n: 'dais', kind: 'bool', def: true, d: 'A raised platform at the far end.'},
  {k: 'daisHeight', n: 'dais height (m)', min: 0.2, max: 2, step: 0.1, def: 0.6, d: ''},
  {k: 'daisDepth', n: 'dais depth (m)', min: 1, max: 8, step: 0.1, def: 3, d: ''},
  {k: 'banners', n: 'banners', min: 0, max: 12, step: 1, int: true, def: 4, d: 'Hung on the side walls (cloth: burns).'},
  {k: 'niches', n: 'statue niches', kind: 'bool', def: false, d: 'Niches in the side walls with statues.'}
], TILE_ROWS, [
  {sec: 'Ruin'},
  {k: 'collapse', n: 'collapse', min: 0, max: 1, step: 0.05, def: 0.15, d: 'Ceiling falls: holes up into the vault, rubble heaps, fallen blocks.'},
  {k: 'exits', n: 'exits', kind: 'enum', options: ['through', 'dead end', 'sides', 'all'], def: 'through', d: 'Doorways besides the origin: the far end, the two sides, or all three.'}
]);

function hallBody(b, P, fy, waterLevel) {
  const wet = waterLevel !== null && waterLevel !== undefined ? {liquid: 'liquid', level: waterLevel} : {};
  const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 20), C = b.m(P.ceiling);
  const hw = Math.round(W / 2), T = b.m(P.wallThick, 4);
  const dw = Math.round(b.m(P.doorWidth, 10) / 2), dh = b.m(P.doorHeight, 18);
  const z0 = T, z1 = T + L, zc = Math.round((z0 + z1) / 2);
  b.doorway(0, z0 + 1, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, b.m(0.8))});
  const room = b.room('box', {min: [-hw, fy, z0], max: [hw, H, z1]}, wet);
  b.skin('box', {min: [-hw, fy, z0], max: [hw, H, z1]}, {skin: 3, mat: 'wall', noise: 'wall', layer: 1});
  const ncol = Number(P.columnRows) ? P.columnCount : 0;
  const bays = [];
  for (let i = 0; i <= ncol; i++) bays.push(Math.round(z0 + (L * i) / (ncol + 1)));
  bays.push(z1);
  if (P.vault === 'barrel' || P.vault === 'groin') {
    b.room('vault', {min: [-hw, H - 1, z0], max: [hw, H + C, z1], axis: 'z', rise: Math.max(2, C + 1)});
    b.skin('vault', {min: [-hw, H - 1, z0], max: [hw, H + C, z1], axis: 'z', rise: Math.max(2, C + 1)}, {skin: 3, mat: 'ceil', layer: 1});
    if (P.vault === 'groin' && ncol > 0)
      for (let i = 0; i + 1 < bays.length; i++)
        b.room('vault', {min: [-hw, H - 1, bays[i]], max: [hw, H + C, bays[i + 1]], axis: 'x', rise: Math.max(2, Math.min(C + 1, Math.round((bays[i + 1] - bays[i]) / 2)))});
  } else if (P.vault === 'dome' && C > 1) {
    const r = Math.min(hw, Math.round(L / 2));
    b.room('ellipsoid', {min: [-hw, H - C, zc - r], max: [hw, H + C, zc + r]});
    b.skin('ellipsoid', {min: [-hw, H - C, zc - r], max: [hw, H + C, zc + r]}, {skin: 3, mat: 'ceil', layer: 1});
  }
  const aisle = b.m(P.aisleWidth, 10);
  floorPattern(b, P, -hw, z0, hw, z1, fy, aisle);
  // Columns.
  const R = b.m(P.columnRadius, 2);
  const xs = [];
  if (P.columnRows === '2' || P.columnRows === '4') xs.push(-Math.round(aisle / 2), Math.round(aisle / 2));
  if (P.columnRows === '4') { const o = Math.round(aisle / 2 + (hw - aisle / 2) / 2); xs.push(-o, o); }
  const colZ = bays.slice(1, ncol + 1);
  xs.forEach((x, ri) => colZ.forEach((z, ci) => {
    if (Math.abs(x) + R + 2 > hw) return;
    const cx = x + b.jit(2, 'colx', ri, ci), cz = z + b.jit(3, 'colz', ri, ci);
    column(b, P, cx, cz, fy, H + Math.round(C / 2) + 2 - fy, R, P.columnStyle, ri * 64 + ci);
    b.lightSpot([cx + (x < 0 ? R + 1 : -R - 1), fy + b.m(2), cz], 'wall', x < 0 ? '+x' : '-x');
  }));
  // Wall lights between bays.
  for (let i = 0; i + 1 < bays.length; i++) {
    const z = Math.round((bays[i] + bays[i + 1]) / 2);
    b.lightSpot([-hw + 1, fy + b.m(2.2), z], 'wall', '+x');
    b.lightSpot([hw - 1, fy + b.m(2.2), z], 'wall', '-x');
  }
  // Niches + statues, banners.
  if (P.niches) for (let i = 0; i + 1 < bays.length; i++) for (const s of [-1, 1]) {
    const z = Math.round((bays[i] + bays[i + 1]) / 2), nw = b.m(0.7), nd = b.m(0.8);
    b.carve('box', {min: [s * hw, fy + b.m(0.5), z - nw], max: [s * (hw + nd), fy + b.m(3), z + nw]}, Object.assign({layer: 3, noise: null, blend: 0, tag: 'niche'}, wet));
    if (b.r('niche', i, s) < P.deco + 0.3) b.prop('ruins/statue', {height: 2.2}, [s * (hw + Math.round(nd / 2)), fy + b.m(0.5), z], s < 0 ? 90 : 270);
  }
  for (let i = 0; i < P.banners; i++) {
    const s = i % 2 ? 1 : -1;
    const z = Math.round(z0 + (L * (Math.floor(i / 2) + 1)) / (Math.ceil(P.banners / 2) + 1));
    b.prop('ruins/banner', {length: Math.min(4, P.height * 0.6)}, [s * (hw - 1), fy + Math.round(H * 0.85) - fy, z], s < 0 ? 90 : 270);
  }
  // Dais.
  let daisTop = fy;
  if (P.dais) {
    const dd = b.m(P.daisDepth, 6), dh2 = b.m(P.daisHeight, 1);
    b.fill('box', {min: [-hw, fy, z1 - dd], max: [hw, fy + dh2, z1]}, {mat: 'trim', layer: 2, noise: null});
    b.fill('box', {min: [-Math.round(hw * 0.6), fy, z1 - dd - b.m(0.5)], max: [Math.round(hw * 0.6), fy + Math.round(dh2 / 2), z1 - dd]}, {mat: 'trim', layer: 2, noise: null});
    daisTop = fy + dh2;
    if (P.deco > 0.2) for (const s of [-1, 1]) b.prop('ruins/brazier', {lit: 'light'}, [s * Math.round(hw * 0.5), daisTop, z1 - Math.round(dd / 2)], 180, {light: true});
  }
  ceilingFalls(b, P, Math.round(P.collapse * L / b.m(6)), -hw, z0, hw, z1, fy, H + C, 'hall');
  // Exits.
  const ex = P.exits;
  const ways = [];
  // A sunken (flooded) hall keeps its doors at the sill level, y = 0.
  const exitY = fy < 0 ? 0 : daisTop, sideY = fy < 0 ? 0 : fy;
  b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'arch', ['door']);
  b.way('door', [0, 0, Math.round(T / 2)], ['socket:in', 'c0']);
  const pts = [];
  for (let z = z0 + b.m(1.5); z < z1 - b.m(1.5); z += b.m(4)) pts.push([0, fy, z]);
  const names = b.wayChain('c', pts.length ? pts : [[0, fy, zc]], ['door'], []);
  const last = names[names.length - 1];
  if (ex === 'through' || ex === 'all') {
    b.carve('vault', {min: [-dw, exitY, z1 - (P.dais && fy >= 0 ? b.m(P.daisDepth) + 2 : 1)], max: [dw, exitY + dh, z1 + T], axis: 'z', rise: Math.min(dw, b.m(0.8))}, {layer: 3, noise: null, tag: 'door'});
    b.socket('out', [0, exitY, z1 + T], '+z', dw * 2, dh, 'arch', ['door']);
    b.way('back', [0, exitY, z1 + Math.round(T / 2)], [last, 'socket:out']);
    ways.push('back');
  }
  if (ex === 'sides' || ex === 'all') for (const [s, nm] of [[-1, 'left'], [1, 'right']]) {
    b.carve('vault', {min: [s < 0 ? -hw - T : hw - 1, sideY, zc - dw], max: [s < 0 ? -hw + 1 : hw + T, sideY + dh, zc + dw], axis: 'x', rise: Math.min(dw, b.m(0.8))}, {layer: 3, noise: null, tag: 'door'});
    b.socket(nm, [s * (hw + T), sideY, zc], s < 0 ? '-x' : '+x', dw * 2, dh, 'arch', ['door']);
    b.way(nm, [s * (hw - b.m(1)), sideY, zc], ['socket:' + nm]);
    ways.push(nm);
  }
  // Link side/back nodes to the nearest aisle node.
  for (const w of ways) {
    const node = b.waynodes.find(n => n.name === w);
    let best = names[0], bd = Infinity;
    for (const nm of names) { const n = b.waynodes.find(q => q.name === nm); const d = Math.abs(n.pos[2] - node.pos[2]); if (d < bd) { bd = d; best = nm; } }
    node.links.push(best);
    b.waynodes.find(q => q.name === best).links.push(w);
  }
  return {W, L, H, C, hw, T, z0, z1, zc, dw, dh, room};
}

const HALL = {
  type: 'hall', label: 'Hall', version: 1,
  size: {width: {min: 4, max: 40, def: 12}, length: {min: 4, max: 80, def: 20}, height: {min: 2.5, max: 20, def: 6}, ceiling: {min: 0, max: 15, def: 3}},
  u: {wallRough: 0.3, lightCount: 4, decay: 0.3},
  rows: HALL_ROWS,
  PRESETS: {
    nave: {params: {}},
    hypostyle: {params: {columnRows: '4', columnCount: 6, width: 18, length: 24, height: 5, vault: 'flat', columnStyle: 'square', tilePattern: 'checker', banners: 0, dais: false}},
    domed_hall: {params: {vault: 'dome', ceiling: 6, width: 14, length: 14, columnRows: '2', columnCount: 2, columnStyle: 'fluted', aisleWidth: 8, tilePattern: 'runner'}},
    fallen_nave: {params: {columnStyle: 'fluted', decay: 0.85, collapse: 0.7, banners: 1, moisture: 0.5, lightCount: 1, vault: 'groin'}}
  },
  build(b, P) { hallBody(b, P, 0); }
};

const FLOODED_HALL = {
  type: 'flooded_hall', label: 'Flooded hall', version: 1,
  size: HALL.size,
  u: {moisture: 0.8, decay: 0.5, lightCount: 2},
  rows: HALL_ROWS.concat([
    {sec: 'Water'},
    {k: 'sunk', n: 'sunken floor (m)', min: 0.5, max: 6, step: 0.1, def: 1.5, d: 'The hall floor lies this far below the doors; steps lead down at both ends.'},
    {k: 'waterLevel', n: 'water level', min: 0, max: 1, step: 0.05, def: 0.8, d: 'Fraction of the sunken depth that is flooded (capped one voxel under the door sills, so the water stays put).'}
  ]),
  PRESETS: {
    cistern: {params: {columnRows: '4', columnCount: 6, vault: 'groin', sunk: 3, waterLevel: 0.95, dais: false, banners: 0, tilePattern: 'plain', width: 16, length: 24}},
    drowned_nave: {params: {sunk: 1.2, waterLevel: 1, collapse: 0.3, banners: 2}},
    shallows: {params: {sunk: 0.6, waterLevel: 0.6, columnStyle: 'broken', vault: 'flat', height: 4}}
  },
  build(b, P) {
    const S = b.m(P.sunk, 4);
    const wl = Math.min(-1, -S + Math.round(P.waterLevel * S));
    // The water is the hall carve's own liquid up to a flat level (PLAN 3.4.1 `level`).
    const h = hallBody(b, P, -S, wl > -S ? wl - 1 : null);
    const run = Math.max(S + 2, Math.round(S * 1.5)), sh = Math.max(1, Math.round(S / Math.max(1, Math.round(S / 2))));
    // Solid flights down from the door sills to the sunken floor.
    b.flight('fill', '-z', -h.dw - 2, h.dw + 2, h.z0, h.z0 + run, -S, S, sh, {below: S}, {mat: 'trim', layer: 3, noise: null});
    if (P.exits === 'through' || P.exits === 'all') b.flight('fill', '+z', -h.dw - 2, h.dw + 2, h.z1 - run, h.z1, -S, S, sh, {below: S}, {mat: 'trim', layer: 4, noise: null});
    for (const n of b.waynodes) if (n.pos[1] < 0) n.swim = wl > n.pos[1] + b.m(1.2);
  }
};

const CORRIDOR = {
  type: 'corridor', label: 'Corridor', version: 1,
  size: {width: {min: 1, max: 8, def: 2.4}, length: {min: 2, max: 80, def: 12}, height: {min: 1.8, max: 8, def: 2.8}, ceiling: {min: 0, max: 4, def: 1}},
  u: {wallRough: 0.2, lightCount: 2, lightType: 'torch'},
  rows: [
    {sec: 'Profile'},
    {k: 'profile', n: 'profile', kind: 'enum', options: ['arch', 'flat', 'pointed'], def: 'arch', d: 'pointed = a taller, narrower arch.'},
    {k: 'archSpacing', n: 'rib spacing (m)', min: 0, max: 10, step: 0.1, def: 3, d: 'Transverse rib arches standing proud of the walls; 0 = none.'},
    {k: 'ribDepth', n: 'rib depth (vox)', min: 1, max: 4, step: 1, int: true, def: 1, d: 'How far the ribs stand proud.'},
    {sec: 'Alcoves'},
    {k: 'alcoves', n: 'alcove frequency', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Chance per bay of a wall niche (urns, candles, bones).'},
    {k: 'alcoveDepth', n: 'alcove depth (m)', min: 0.3, max: 2, step: 0.1, def: 0.6, d: ''},
    {k: 'pairAlcoves', n: 'paired alcoves', kind: 'bool', def: false, d: 'Alcoves come in facing pairs (formal) instead of alternating by seed.'}
  ].concat(TILE_ROWS),
  PRESETS: {
    processional: {params: {}},
    narrow_vaulted: {params: {width: 1.6, height: 2.4, ceiling: 1.4, profile: 'pointed', archSpacing: 1.8, alcoves: 0.1, tilePattern: 'plain'}},
    wide_gallery: {params: {width: 4.5, height: 4, ceiling: 2, archSpacing: 4, ribDepth: 2, alcoves: 0.8, pairAlcoves: true, tilePattern: 'runner', lightCount: 4, lightType: 'brazier'}},
    forgotten: {params: {decay: 0.8, moisture: 0.6, alcoves: 0.5, lightCount: 0, tilePattern: 'checker'}}
  },
  build(b, P) {
    const W = b.m(P.width, 8), L = b.m(P.length, 20), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2);
    const rise = P.profile === 'flat' ? 0 : P.profile === 'pointed' ? Math.max(3, Math.round(Math.max(C, W * 0.8))) : Math.max(2, Math.min(C || hw, hw * 2));
    const prof = P.profile === 'flat' ? 'rect' : 'arch';
    b.passage(0, L, 0, 0, W, H, prof, {rise});
    if (prof === 'arch') b.skin('vault', {min: [-hw, 0, 0], max: [hw, H + rise, L], axis: 'z', rise}, {skin: 2, mat: 'wall', noise: 'wall', layer: 1});
    else b.skin('box', {min: [-hw, 0, 0], max: [hw, H, L]}, {skin: 2, mat: 'wall', noise: 'wall', layer: 1});
    floorPattern(b, P, -hw, 0, hw, L, 0, W);
    const sp = b.m(P.archSpacing);
    const bays = [];
    if (sp > 0) {
      const rd = P.ribDepth, rt = 3;
      for (let z = Math.round(sp / 2), i = 0; z + rt < L; z += sp, i++) {
        bays.push(z);
        if (b.r('rib', i) < P.decay * 0.3) continue;            // a fallen rib
        if (prof === 'arch') {
          b.fill('vault', {min: [-hw - 2, 0, z], max: [hw + 2, H + rise + 2, z + rt], axis: 'z', rise: rise + 2}, {mat: 'trim', layer: 2, noise: null});
          b.carve('vault', {min: [-hw + rd, 0, z - 1], max: [hw - rd, H + rise - rd, z + rt + 1], axis: 'z', rise: Math.max(1, rise - rd)}, {layer: 3, noise: null, blend: 0, tag: 'detail'});
        } else {
          b.fill('box', {min: [-hw - 2, 0, z], max: [hw + 2, H + 2, z + rt]}, {mat: 'trim', layer: 2, noise: null});
          b.carve('box', {min: [-hw + rd, 0, z - 1], max: [hw - rd, H - rd, z + rt + 1]}, {layer: 3, noise: null, blend: 0, tag: 'detail'});
        }
        b.lightSpot([-hw + rd + 1, b.m(2), z + 1], 'wall', '+x');
        b.lightSpot([hw - rd - 1, b.m(2), z + 1], 'wall', '-x');
      }
    } else for (let z = b.m(2); z < L - 2; z += b.m(4)) { b.lightSpot([-hw + 1, b.m(2), z], 'wall', '+x'); b.lightSpot([hw - 1, b.m(2), z], 'wall', '-x'); }
    // Alcoves between ribs (or every 3 m).
    const ad = b.m(P.alcoveDepth, 2), aw = b.m(0.5), ah = b.m(1.6);
    const mids = [];
    const marks = bays.length > 1 ? bays : Array.from({length: Math.max(1, Math.floor(L / b.m(3)))}, (_, i) => b.m(1.5) + i * b.m(3));
    for (let i = 0; i + 1 < marks.length; i++) mids.push(Math.round((marks[i] + marks[i + 1]) / 2));
    mids.forEach((z, i) => {
      if (b.r('alc', i) >= P.alcoves) return;
      const sides = P.pairAlcoves ? [-1, 1] : [b.r('alc', i, 'side') < 0.5 ? -1 : 1];
      for (const s of sides) {
        b.carve('vault', {min: [s * hw, b.m(0.7), z - aw], max: [s * (hw + ad), b.m(0.7) + ah, z + aw], axis: 'x', rise: aw}, {layer: 3, noise: null, blend: 0, tag: 'niche'});
        if (b.r('alc', i, s, 'deco') < P.deco) {
          const what = b.pick(['ruins/urn', 'ruins/candle_cluster', 'ruins/bones', 'ruins/urn'], 'alc', i, s, 'what');
          b.prop(what, {}, [s * (hw + Math.round(ad / 2)), b.m(0.7), z], s < 0 ? 90 : 270);
          if (what === 'ruins/urn' && b.r('alc', i, s, 'loot') < 0.3) b.ref('container', [s * (hw + Math.round(ad / 2)), b.m(0.7), z], {name: 'urn' + i, props: {loot: 'tomb_offering'}});
        }
      }
    });
    const pts = [];
    for (let z = 2; z < L - 1; z += b.m(4)) pts.push([0, 0, z]);
    b.wayChain('w', pts.length ? pts : [[0, 0, Math.round(L / 2)]], ['socket:in'], ['socket:out']);
    b.socket('in', [0, 0, 0], '-z', W, H, prof);
    b.socket('out', [0, 0, L], '+z', W, H, prof);
  }
};

/** One straight flight. travel: '+z'|'-z'|'+x'|'-x' (direction of travel);
 *  across [a0,a1) on the other axis; along [s0,s1) on the travel axis; floor
 *  y0 at the start, y0+rise at the end. mode 'carve' = the walking space
 *  (clear H) + a floor band + a dressed-stone skin; 'slab' = a floating stone
 *  slab th thick under the walking surface. Returns the rise built. */
function flight(b, travel, a0, a1, s0, s1, y0, rise, H, stepH, mode, th) {
  const up = rise >= 0, h = Math.max(1, Math.round(stepH));
  const R = Math.max(1, Math.round(Math.abs(rise) / h)) * h;
  const flip = {'+z': '-z', '-z': '+z', '+x': '-x', '-x': '+x'};
  const dir = up ? travel : flip[travel], lowY = up ? y0 : y0 - R;
  if (mode === 'carve') {
    b.flight('carve', dir, a0, a1, s0, s1, lowY, R, h, {height: H}, {tag: 'room'});
    b.flight('fill', dir, a0, a1, s0, s1, lowY, R, h, {below: 2}, {mat: 'floor', layer: 2, noise: null});
    b.flight('skin', dir, a0, a1, s0, s1, lowY, R, h, {height: H}, {skin: 2, mat: 'wall', layer: 1});
  } else b.flight('fill', dir, a0, a1, s0, s1, lowY, R, h, {below: th}, {mat: 'trim', layer: 2, noise: null});
  return up ? R : -R;
}

const STAIR = {
  type: 'stair', label: 'Stair', version: 1,
  size: {width: {min: 1, max: 6, def: 2}, length: null, height: {min: 1.8, max: 6, def: 2.6, n: 'headroom (m)'}, ceiling: null},
  u: {lightCount: 2, lightType: 'torch'},
  rows: [
    {sec: 'Stair'},
    {k: 'kind', n: 'kind', kind: 'enum', options: ['straight', 'switchback', 'spiral'], def: 'straight',
     d: 'spiral = a square newel stair round a solid core (always climbs from the origin: enter from the top socket to go down).'},
    {k: 'direction', n: 'direction', kind: 'enum', options: ['down', 'up'], def: 'down', d: 'Straight and switchback only.'},
    {k: 'rise', n: 'rise (m)', min: 1, max: 40, step: 0.1, def: 5, d: ''},
    {k: 'stepHeight', n: 'step height (m)', min: 0.1, max: 0.4, step: 0.02, def: 0.2, d: ''},
    {k: 'pitch', n: 'pitch', min: 0.3, max: 1, step: 0.05, def: 0.6, d: 'Rise over run of a flight (1 = 45 degrees).'},
    {k: 'landing', n: 'landing (m)', min: 0.5, max: 4, step: 0.1, def: 1.2, d: 'Flat run at each end of a straight stair.'},
    {k: 'gap', n: 'switchback wall (m)', min: 0.4, max: 3, step: 0.1, def: 0.6, d: 'Rock left between the two flights of a switchback.'},
    {k: 'core', n: 'spiral core (m)', min: 0.6, max: 6, step: 0.1, def: 2, d: 'Side of the solid newel; a bigger core = longer flights = more headroom.'}
  ],
  PRESETS: {
    straight_down: {params: {}},
    grand_switchback: {params: {kind: 'switchback', width: 3, rise: 8, height: 3.2, pitch: 0.55, lightCount: 3}},
    tower_spiral: {params: {kind: 'spiral', width: 1.4, rise: 10, core: 2.6, stepHeight: 0.24}},
    steep_crack: {params: {width: 1.2, rise: 6, pitch: 0.95, stepHeight: 0.3, decay: 0.6, landing: 0.5}}
  },
  build(b, P) {
    const W = b.m(P.width, 8), H = b.m(P.height, 16), hw = Math.round(W / 2), sh = b.m(P.stepHeight, 1);
    const sgn = P.kind === 'spiral' || P.direction === 'up' ? 1 : -1;
    const R = b.m(P.rise, 4);
    if (P.kind === 'straight') {
      const land = b.m(P.landing, 3), run = Math.round(R / P.pitch);
      b.passage(0, land + 1, 0, 0, W, H, 'rect');
      b.floor(-hw, 0, hw, land, 0);
      const r = flight(b, '+z', -hw, hw, land, land + run, 0, sgn * R, H, sh, 'carve');
      b.passage(land + run - 1, 2 * land + run, 0, r, W, H, 'rect');
      b.floor(-hw, land + run, hw, 2 * land + run, r);
      for (let z = land + 4; z < land + run; z += b.m(3)) b.lightSpot([-hw + 1, Math.round(r * (z - land) / run) + b.m(2), z], 'wall', '+x');
      b.wayChain('w', [[0, 0, Math.round(land / 2)], [0, Math.round(r / 2), land + Math.round(run / 2)], [0, r, land + run + Math.round(land / 2)]], ['socket:in'], ['socket:out']);
      b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
      b.socket('out', [0, r, 2 * land + run], '+z', W, H, 'rect');
    } else if (P.kind === 'switchback') {
      const half = Math.round(R / 2), run = Math.round(half / P.pitch), gap = b.m(P.gap, 3);
      const land = b.m(1);
      b.passage(0, land + 1, 0, 0, W, H, 'rect');
      b.floor(-hw, 0, hw, land, 0);
      const r1 = flight(b, '+z', -hw, hw, land, land + run, 0, sgn * half, H, sh, 'carve');
      // landing across both flights
      const x1 = hw + gap + W;
      b.room('box', {min: [-hw, r1, land + run - 1], max: [x1, r1 + H, land + run + W]});
      b.floor(-hw, land + run, x1, land + run + W, r1);
      const r2 = flight(b, '-z', hw + gap, x1, land, land + run, r1, sgn * half, H, sh, 'carve');
      const y2 = r1 + r2;
      const xo = hw + gap + hw;
      b.room('box', {min: [hw + gap, y2, 0], max: [x1, y2 + H, land + 1]});
      b.floor(hw + gap, 0, x1, land, y2);
      b.lightSpot([0, r1 + H - 2, land + run + Math.round(W / 2)], 'ceil', '-y');
      b.lightSpot([x1 - 1, r1 + b.m(2), land + run + Math.round(W / 2)], 'wall', '-x');
      b.wayChain('w', [[0, 0, Math.round(land / 2)], [0, Math.round(r1 / 2), land + Math.round(run / 2)], [0, r1, land + run + Math.round(W / 2)],
                       [xo, r1, land + run + Math.round(W / 2)], [xo, r1 + Math.round(r2 / 2), land + Math.round(run / 2)], [xo, y2, Math.round(land / 2)]], ['socket:in'], ['socket:out']);
      b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
      b.socket('out', [xo, y2, 0], '-z', W, H, 'rect');
    } else {
      // Square spiral: a solid core of side 2c, flights along its four sides,
      // landings in the corners, all inside one carved well.
      const c = Math.round(b.m(P.core, 6) / 2), Hs = c + W, th = b.m(0.4, 3);
      const runF = 2 * c;
      const nF = Math.max(1, Math.ceil(R / (runF * P.pitch)));
      const stepsPer = Math.max(1, Math.round(R / nF / sh)), rf = stepsPer * sh, RT = nF * rf;
      if (4 * rf - th < H) b.notes.push('spiral headroom ' + ((4 * rf - th) / 10).toFixed(1) + ' m is under the clear height: widen the core or raise the pitch');
      const cx = Math.round((Hs + c) / 2), zc = Hs;       // well centre; the SW corner centre is the origin
      b.room('box', {min: [cx - Hs, 0, zc - Hs], max: [cx + Hs, RT + H, zc + Hs]});
      b.skin('box', {min: [cx - Hs, 0, zc - Hs], max: [cx + Hs, RT + H, zc + Hs]}, {skin: 2, mat: 'wall', layer: 1});
      b.fill('box', {min: [cx - c, -1, zc - c], max: [cx + c, RT + H, zc + c]}, {mat: 'support', layer: 2, noise: null});
      b.floor(cx - Hs, zc - Hs, cx + Hs, zc + Hs, 0);
      // corners: 0 SW, 1 NW, 2 NE, 3 SE; flights between them.
      const corner = [[cx - Hs, zc - Hs], [cx - Hs, zc + c], [cx + c, zc + c], [cx + c, zc - Hs]];
      const legs = [
        {travel: '+z', a: [cx - Hs, cx - c], s: [zc - c, zc + c]},
        {travel: '+x', a: [zc + c, zc + Hs], s: [cx - c, cx + c]},
        {travel: '-z', a: [cx + c, cx + Hs], s: [zc - c, zc + c]},
        {travel: '-x', a: [zc - Hs, zc - c], s: [cx - c, cx + c]}
      ];
      const ways = [];
      for (let f = 0; f < nF; f++) {
        const lg = legs[f % 4], y0 = f * rf;
        flight(b, lg.travel, lg.a[0], lg.a[1], lg.s[0], lg.s[1], y0, rf, H, sh, 'slab', th);
        const k = (f + 1) % 4, y1 = (f + 1) * rf;
        b.fill('box', {min: [corner[k][0], y1 - th, corner[k][1]], max: [corner[k][0] + W, y1, corner[k][1] + W]}, {mat: 'trim', layer: 2, noise: null});
        ways.push([corner[k][0] + hw, y1, corner[k][1] + hw]);
        if (f % 2 === 0) b.lightSpot([cx - c - 1, y1 + b.m(2), zc], 'wall', '-x');
      }
      b.wayChain('w', [[0, 0, hw]].concat(ways), ['socket:in'], ['socket:out'], {climb: true});
      const k = nF % 4, cc = corner[k];
      const exits = [['-z', [cc[0] + hw, RT, cc[1]]], ['-x', [cc[0], RT, cc[1] + hw]], ['+z', [cc[0] + hw, RT, cc[1] + W]], ['+x', [cc[0] + W, RT, cc[1] + hw]]];
      const [dir, pos] = exits[k];
      b.socket('in', [0, 0, 0], '-z', W, H, 'rect');
      b.socket('out', pos, dir, W, H, 'rect');
    }
  }
};

const CRYPT = {
  type: 'crypt', label: 'Crypt', version: 1,
  size: {width: {min: 3, max: 20, def: 6}, length: {min: 4, max: 40, def: 12}, height: {min: 2, max: 8, def: 3}, ceiling: {min: 0, max: 6, def: 1.5}},
  u: {decay: 0.45, moisture: 0.3, lightCount: 2, lightType: 'candles'},
  rows: ROWS_DOOR.concat([
    {sec: 'Burial niches (loculi)'},
    {k: 'tiers', n: 'niche tiers', min: 0, max: 5, step: 1, int: true, def: 3, d: 'Rows of niches up each long wall.'},
    {k: 'nicheCols', n: 'niches per tier', min: 1, max: 20, step: 1, int: true, def: 5, d: ''},
    {k: 'nicheWidth', n: 'niche width (m)', min: 0.4, max: 2.5, step: 0.1, def: 1.8, d: 'Along the wall (a body lies along it).'},
    {k: 'nicheHeight', n: 'niche height (m)', min: 0.3, max: 1.5, step: 0.05, def: 0.6, d: ''},
    {k: 'nicheDepth', n: 'niche depth (m)', min: 0.3, max: 2.5, step: 0.1, def: 0.7, d: ''},
    {k: 'filled', n: 'occupied niches', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Chance a niche holds bones.'},
    {sec: 'Floor'},
    {k: 'sarcophagi', n: 'sarcophagi', min: 0, max: 8, step: 1, int: true, def: 2, d: 'Down the middle.'},
    {k: 'ossuary', n: 'bone scatter', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Bones strewn on the floor.'},
    {k: 'keyNiche', n: 'key niche', kind: 'bool', def: false, d: 'Suggest a container in one seeded niche (where a key goes).'},
    {k: 'guardians', n: 'guardian spawner', kind: 'bool', def: false, d: 'Suggest a spawner ref in the crypt.'},
    {k: 'exits', n: 'exits', kind: 'enum', options: ['dead end', 'through'], def: 'dead end', d: ''}
  ]),
  PRESETS: {
    catacomb: {params: {}},
    noble_tomb: {params: {width: 7, length: 9, height: 3.5, ceiling: 2.5, tiers: 1, nicheCols: 3, sarcophagi: 3, ossuary: 0.05, keyNiche: true, lightType: 'brazier'}},
    bone_gallery: {params: {width: 3, length: 20, tiers: 4, nicheCols: 8, nicheWidth: 1.2, nicheHeight: 0.45, sarcophagi: 0, ossuary: 0.8, guardians: true, exits: 'through'}},
    plundered: {params: {filled: 0.15, sarcophagi: 2, decay: 0.85, ossuary: 0.5, lightCount: 0}}
  },
  build(b, P) {
    const W = b.m(P.width, 20), L = b.m(P.length, 20), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), T = b.m(P.wallThick, 4);
    const dw = Math.round(b.m(P.doorWidth, 10) / 2), dh = Math.min(H, b.m(P.doorHeight, 18));
    const z0 = T, z1 = T + L;
    b.doorway(0, z0 + 1, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, 6)});
    const room = b.room('box', {min: [-hw, 0, z0], max: [hw, H, z1]});
    if (C >= 2) b.room('vault', {min: [-hw, H - 1, z0], max: [hw, H + C, z1], axis: 'z', rise: C + 1});
    b.skin('box', {min: [-hw, 0, z0], max: [hw, H + C, z1]}, {skin: 3, mat: 'wall', noise: 'wall', layer: 1});
    b.floor(-hw, z0, hw, z1, 0);
    // Loculi.
    const nw = b.m(P.nicheWidth, 4), nh = b.m(P.nicheHeight, 3), nd = b.m(P.nicheDepth, 3);
    const niches = [];
    const gapY = Math.max(2, Math.round((H - b.m(0.4) - P.tiers * nh) / Math.max(1, P.tiers)));
    for (const s of [-1, 1]) for (let t = 0; t < P.tiers; t++) for (let c = 0; c < P.nicheCols; c++) {
      const span = L - b.m(1);
      const z = Math.round(z0 + b.m(0.5) + (span * (c + 0.5)) / P.nicheCols);
      if (nw > span / P.nicheCols - 2) continue;
      const y = b.m(0.4) + t * (nh + gapY);
      if (y + nh > H - 1) continue;
      b.carve('box', {min: [s * hw, y, z - Math.round(nw / 2)], max: [s * (hw + nd), y + nh, z + Math.round(nw / 2)]}, {layer: 3, noise: null, blend: 0, tag: 'niche'});
      niches.push({s, t, c, pos: [s * (hw + Math.round(nd / 2)), y, z]});
    }
    niches.forEach((n, i) => {
      if (b.r('loc', n.s, n.t, n.c) < P.filled) b.prop('ruins/bones', {layout: 'laid'}, n.pos, 0);
    });
    if (P.keyNiche && niches.length) {
      const n = niches[Math.floor(b.r('keyniche') * niches.length)];
      b.ref('container', n.pos, {name: 'key_niche', props: {loot: 'crypt_key', tags: ['key_holder']}});
    }
    if (P.ossuary > 0) b.scatter.push({prim: room, kind: 'bones', density: +(P.ossuary * 0.8).toFixed(3), size: [1, 3], surface: 'floor', mat: 'bone', seedKey: 'ossuary'});
    for (let i = 0; i < P.sarcophagi; i++) {
      const z = Math.round(z0 + (L * (i + 1)) / (P.sarcophagi + 1));
      b.prop('ruins/sarcophagus', {broken: b.r('sarc', i) < P.decay * 0.6}, [b.jit(4, 'sarc', i), 0, z], 0);
      b.ref('container', [0, 0, z], {name: 'sarcophagus' + i, props: {loot: 'tomb_offering'}});
    }
    if (P.guardians) b.ref('spawner', [0, 0, Math.round((z0 + z1) / 2)], {name: 'guardians', props: {mobs: [{base: 'human', weight: 1}], count: 2, radius: Math.round(hw * 0.8), respawn: 'never', behavior: 'guard'}});
    for (let z = z0 + b.m(1.5); z < z1; z += b.m(3)) { b.lightSpot([-hw + 1, b.m(1.6), z], 'wall', '+x'); b.lightSpot([hw - 1, b.m(1.6), z], 'wall', '-x'); }
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'arch', ['door']);
    const pts = [[0, 0, Math.round(T / 2)]];
    for (let z = z0 + b.m(1); z < z1 - 2; z += b.m(4)) pts.push([P.sarcophagi ? -Math.round(hw / 2) : 0, 0, z]);
    if (P.exits === 'through') {
      b.doorway(z1 - 1, z1 + T, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, 6)});
      b.socket('out', [0, 0, z1 + T], '+z', dw * 2, dh, 'arch', ['door']);
      pts.push([0, 0, z1 + Math.round(T / 2)]);
      b.wayChain('w', pts, ['socket:in'], ['socket:out']);
    } else b.wayChain('w', pts, ['socket:in'], []);
  }
};

const PIT_ROOM = {
  type: 'pit_room', label: 'Pit room', version: 1,
  size: {width: {min: 4, max: 30, def: 10}, length: {min: 4, max: 40, def: 12}, height: {min: 2.5, max: 15, def: 5}, ceiling: {min: 0, max: 8, def: 1}},
  u: {lightCount: 3, lightType: 'brazier'},
  rows: ROWS_DOOR.concat([
    {sec: 'Pit'},
    {k: 'pitWidth', n: 'pit width', min: 0.2, max: 1, step: 0.05, def: 0.7, d: 'Fraction of the room width.'},
    {k: 'pitLength', n: 'pit length', min: 0.2, max: 1, step: 0.05, def: 0.7, d: 'Fraction of the room length.'},
    {k: 'pitDepth', n: 'pit depth (m)', min: 1, max: 30, step: 0.1, def: 6, d: ''},
    {k: 'ledge', n: 'ledge (m)', min: 0, max: 4, step: 0.1, def: 1, d: 'Walkway between pit and walls (whatever the pit width leaves).'},
    {k: 'bridge', n: 'bridge', kind: 'enum', options: ['stone', 'plank', 'broken', 'none'], def: 'stone', d: 'Across the pit from door to door. broken = a plank bridge with a gap.'},
    {k: 'bridgeWidth', n: 'bridge width (m)', min: 0.5, max: 4, step: 0.1, def: 1.4, d: ''},
    {k: 'liquid', n: 'pit liquid', kind: 'enum', options: ['none', 'water', 'lava'], def: 'none', d: 'Born full and flat.'},
    {k: 'liquidLevel', n: 'liquid level', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Fraction of the pit depth.'},
    {k: 'spikes', n: 'spikes', kind: 'bool', def: false, d: 'Iron spikes on the pit floor (+ a spikes trap ref).'},
    {k: 'arena', n: 'arena seating', kind: 'bool', def: false, d: 'Stepped tiers along the side walls.'}
  ]),
  PRESETS: {
    bridge_pit: {params: {}},
    lava_trench: {params: {liquid: 'lava', liquidLevel: 0.4, pitWidth: 1, ledge: 0, pitDepth: 4, bridge: 'stone', bridgeWidth: 1}},
    spike_trap: {params: {bridge: 'broken', spikes: true, pitDepth: 3, ledge: 0.6, lightType: 'torch'}},
    arena: {params: {width: 16, length: 16, height: 7, arena: true, pitWidth: 0.55, pitLength: 0.55, pitDepth: 2, bridge: 'none', ledge: 3, lightCount: 6}}
  },
  build(b, P) {
    const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 20), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), T = b.m(P.wallThick, 4);
    const dw = Math.round(b.m(P.doorWidth, 10) / 2), dh = Math.min(H, b.m(P.doorHeight, 18));
    const z0 = T, z1 = T + L, zc = Math.round((z0 + z1) / 2);
    b.doorway(0, z0 + 1, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, 6)});
    b.room('box', {min: [-hw, 0, z0], max: [hw, H, z1]});
    if (C >= 2) b.room('vault', {min: [-hw, H - 1, z0], max: [hw, H + C, z1], axis: 'z', rise: C + 1});
    b.skin('box', {min: [-hw, 0, z0], max: [hw, H + C, z1]}, {skin: 3, mat: 'wall', noise: 'wall', layer: 1});
    b.floor(-hw, z0, hw, z1, 0);
    const led = b.m(P.ledge);
    const pw = Math.min(W - 2 * led, Math.round(W * P.pitWidth)), pl = Math.min(L - 2 * b.m(0.8), Math.round(L * P.pitLength));
    const D = b.m(P.pitDepth, 6), px = Math.round(pw / 2), pz0 = zc - Math.round(pl / 2), pz1 = pz0 + pl;
    // The pit liquid is the pit carve's own (PLAN 3.4.1 `level`): born full, flat, at rest.
    const lv = Math.min(-2, -D + Math.max(1, Math.round(P.liquidLevel * D)));
    const wet = P.liquid !== 'none' ? {liquid: P.liquid === 'lava' ? 'lava' : 'liquid', level: lv} : {};
    const pit = b.carve('box', {min: [-px, -D, pz0], max: [px, 1, pz1]}, Object.assign({layer: 3, noise: 0.5, blend: 2, tag: 'room'}, wet));
    b.skin('box', {min: [-px, -D, pz0], max: [px, 0, pz1]}, {skin: 2, mat: 'wall', layer: 4});
    if (P.spikes) {
      // Spikes are a stalagmite scatter in the accent metal: no prim per spike.
      b.scatter.push({prim: pit, kind: 'stalagmite', density: 0.7, size: [3, 5], surface: 'floor', mat: 'accent', seedKey: 'spikes', spacing: 4});
      b.ref('trap', [0, -D, zc], {name: 'spikes', props: {type: 'spikes', region: {min: [-px, -D, pz0], max: [px, -D + b.m(0.6), pz1]}}});
    }
    const bw = Math.round(b.m(P.bridgeWidth, 4) / 2);
    let crossing = false;
    if (P.bridge === 'stone') {
      b.fill('box', {min: [-bw, -b.m(0.6), pz0 - 1], max: [bw, 0, pz1 + 1]}, {mat: 'trim', layer: 4, noise: null});
      b.carve('vault', {min: [-bw - 1, -D, pz0 + 2], max: [bw + 1, -b.m(0.6), pz1 - 2], axis: 'x', rise: Math.min(Math.round(pl / 2) - 2, D - b.m(0.8))}, {layer: 5, noise: null, blend: 0, tag: 'detail'});
      crossing = true;
    } else if (P.bridge === 'plank' || P.bridge === 'broken') {
      const gap = P.bridge === 'broken' ? b.m(1.6) : 0, zm = zc + b.jit(Math.round(pl / 6), 'gap');
      b.fill('box', {min: [-bw, -1, pz0 - 2], max: [bw, 0, zm - Math.round(gap / 2)]}, {mat: 'plank', layer: 4, noise: null});
      b.fill('box', {min: [-bw, -1, zm + Math.round(gap / 2)], max: [bw, 0, pz1 + 2]}, {mat: 'plank', layer: 4, noise: null});
      for (const s of [-1, 1]) b.fill('box', {min: [s < 0 ? -bw - 1 : bw, 0, pz0 - 2], max: [s < 0 ? -bw : bw + 1, b.m(0.9), pz1 + 2]}, {mat: 'timber', layer: 4, noise: null});
      if (P.bridge === 'broken') b.carve('box', {min: [-bw - 2, -1, zm - Math.round(gap / 2)], max: [bw + 2, b.m(1), zm + Math.round(gap / 2)]}, {layer: 5, noise: null, blend: 0, tag: 'detail'});
      crossing = P.bridge === 'plank';
    }
    if (P.arena) for (let t = 0; t < 3; t++) for (const s of [-1, 1]) {
      const w = b.m(0.8);
      const xa = s < 0 ? -hw : hw - (3 - t) * w;
      b.fill('box', {min: [xa, 0, z0 + b.m(1)], max: [xa + (3 - t) * w, (t + 1) * b.m(0.45), z1 - b.m(1)]}, {mat: 'trim', layer: 2, noise: null});
    }
    for (const [x, z] of [[-hw + 2, z0 + 2], [hw - 2, z0 + 2], [-hw + 2, z1 - 2], [hw - 2, z1 - 2]]) b.lightSpot([x, b.m(2), z], 'wall', x < 0 ? '+x' : '-x');
    b.lightSpot([0, H - 2, zc], 'ceil', '-y');
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'arch', ['door']);
    b.doorway(z1 - 1, z1 + T, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, 6)});
    b.socket('out', [0, 0, z1 + T], '+z', dw * 2, dh, 'arch', ['door']);
    const ledgeOk = led >= b.m(0.6);
    const lx = -px - Math.max(2, Math.round(led / 2));
    if (crossing) b.wayChain('w', [[0, 0, Math.round(T / 2)], [0, 0, pz0 - 2], [0, 0, zc], [0, 0, pz1 + 2], [0, 0, z1 + Math.round(T / 2)]], ['socket:in'], ['socket:out']);
    else if (ledgeOk) b.wayChain('w', [[0, 0, Math.round(T / 2)], [0, 0, pz0 - 2], [lx, 0, pz0 - 2], [lx, 0, pz1 + 2], [0, 0, pz1 + 2], [0, 0, z1 + Math.round(T / 2)]], ['socket:in'], ['socket:out']);
    else { b.way('near', [0, 0, Math.round(T / 2)], ['socket:in']); b.way('far', [0, 0, z1 + Math.round(T / 2)], ['socket:out']); b.notes.push('no way across the pit (no bridge, ledge under 0.6 m)'); }
    b.way('pit', [0, -D, zc], [], {pit: true});
  }
};

const SANCTUM = {
  type: 'sanctum', label: 'Sanctum', version: 1,
  size: {width: {min: 4, max: 30, def: 10}, length: {min: 4, max: 40, def: 12}, height: {min: 3, max: 20, def: 6}, ceiling: {min: 0, max: 15, def: 4}},
  u: {lightCount: 4, lightType: 'brazier', decay: 0.2, deco: 0.7},
  rows: ROWS_DOOR.concat([
    {sec: 'Sanctum'},
    {k: 'apse', n: 'apse', kind: 'bool', def: true, d: 'A rounded end behind the altar.'},
    {k: 'dome', n: 'dome', kind: 'bool', def: true, d: 'Dome over the middle (height = ceiling rise).'},
    {k: 'daisHeight', n: 'dais height (m)', min: 0, max: 3, step: 0.1, def: 0.9, d: ''},
    {k: 'daisSteps', n: 'dais steps', min: 1, max: 8, step: 1, int: true, def: 3, d: ''},
    {k: 'altar', n: 'altar', kind: 'bool', def: true, d: ''},
    {k: 'statues', n: 'statues', min: 0, max: 8, step: 1, int: true, def: 2, d: 'Flanking the dais, then along the walls.'},
    {k: 'braziers', n: 'braziers', min: 0, max: 8, step: 1, int: true, def: 2, d: ''},
    {k: 'pillarRing', n: 'pillar ring', min: 0, max: 12, step: 1, int: true, def: 0, d: 'Columns in a ring round the dome.'},
    {k: 'secretDoor', n: 'secret back door', kind: 'bool', def: false, d: 'A narrow passage out behind the apse (a socket).'}
  ], TILE_ROWS),
  PRESETS: {
    shrine: {params: {}},
    great_sanctum: {params: {width: 16, length: 18, height: 8, ceiling: 7, pillarRing: 8, statues: 4, braziers: 4, tilePattern: 'checker', lightCount: 6}},
    hidden_chapel: {params: {width: 6, length: 7, height: 3.5, ceiling: 1.5, dome: false, apse: false, statues: 1, braziers: 0, lightType: 'candles', secretDoor: true}},
    desecrated: {params: {decay: 0.9, moisture: 0.4, statues: 3, braziers: 1, lightCount: 1}}
  },
  build(b, P) {
    const W = b.m(P.width, 30), L = b.m(P.length, 30), H = b.m(P.height, 20), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), T = b.m(P.wallThick, 4);
    const dw = Math.round(b.m(P.doorWidth, 10) / 2), dh = Math.min(H, b.m(P.doorHeight, 18));
    const z0 = T, z1 = T + L, zc = Math.round((z0 + z1) / 2);
    b.doorway(0, z0 + 1, 0, 0, dw * 2, dh, 'arch', {rise: Math.min(dw, 6)});
    b.room('box', {min: [-hw, 0, z0], max: [hw, H, z1]});
    b.skin('box', {min: [-hw, 0, z0], max: [hw, H, z1]}, {skin: 3, mat: 'wall', noise: 'wall', layer: 1});
    let zEnd = z1;
    if (P.apse) {
      b.room('cylinder', {min: [-hw, 0, z1 - hw], max: [hw, H, z1 + hw], axis: 'y'});
      b.skin('cylinder', {min: [-hw, 0, z1 - hw], max: [hw, H, z1 + hw], axis: 'y'}, {skin: 3, mat: 'wall', layer: 1});
      zEnd = z1 + Math.round(hw * 0.8);
    }
    if (P.dome && C > 1) {
      const r = Math.min(hw, Math.round(L / 2));
      b.room('ellipsoid', {min: [-r, H - C, zc - r], max: [r, H + C, zc + r]});
      b.skin('ellipsoid', {min: [-r, H - C, zc - r], max: [r, H + C, zc + r]}, {skin: 3, mat: 'ceil', layer: 1});
    } else if (C > 1) b.room('vault', {min: [-hw, H - 1, z0], max: [hw, H + C, z1], axis: 'z', rise: C + 1});
    floorPattern(b, P, -hw, z0, hw, z1 + (P.apse ? hw : 0), 0, b.m(2));
    // Dais: stepped, at the far end.
    const DH = b.m(P.daisHeight), ns = P.daisSteps, dd = b.m(3);
    let top = 0;
    if (DH > 0) for (let s = 0; s < ns; s++) {
      const y = Math.round((DH * (s + 1)) / ns);
      const zs = zEnd - dd - (ns - 1 - s) * b.m(0.4);
      b.fill('box', {min: [-Math.round(hw * 0.7), 0, zs], max: [Math.round(hw * 0.7), y, zEnd + hw]}, {mat: 'trim', layer: 2, noise: null, mask: 'air'});
      top = y;
    }
    const altarZ = zEnd - Math.round(dd / 2);
    if (P.altar) { b.prop('ruins/altar', {}, [0, top, altarZ], 180); b.ref('marker', [0, top, altarZ - b.m(1)], {name: 'altar', props: {tags: ['altar']}}); }
    for (let i = 0; i < P.statues; i++) {
      const s = i % 2 ? 1 : -1, row = Math.floor(i / 2);
      const z = altarZ - b.m(1.5) - row * b.m(3);
      b.prop('ruins/statue', {height: Math.min(3.5, P.height * 0.55)}, [s * Math.round(hw * 0.75), z > zEnd - dd ? top : 0, z], s < 0 ? 90 : 270);
    }
    for (let i = 0; i < P.braziers; i++) {
      const s = i % 2 ? 1 : -1, row = Math.floor(i / 2);
      const z = zEnd - dd - b.m(1) - row * b.m(3);
      b.prop('ruins/brazier', {lit: 'light'}, [s * Math.round(hw * 0.4), 0, z], 0, {light: true});
    }
    if (P.pillarRing > 0) {
      const r = Math.round(Math.min(hw, L / 2) * 0.75);
      for (let i = 0; i < P.pillarRing; i++) {
        const a = (2 * Math.PI * (i + 0.5)) / P.pillarRing;
        const x = Math.round(Math.sin(a) * r), z = zc + Math.round(Math.cos(a) * r);
        if (Math.abs(x) < dw + 3 && z < zc) continue;           // keep the way in clear
        column(b, P, x, z, 0, H + 2, b.m(0.35, 2), 'round', 'ring' + i);
      }
    }
    if (P.deco > 0.3) b.ref('container', [Math.round(hw * 0.5), top, altarZ], {name: 'offering_chest', props: {loot: 'sanctum'}});
    for (const s of [-1, 1]) for (let z = z0 + b.m(1.5); z < z1; z += b.m(4)) b.lightSpot([s * (hw - 1), b.m(2.2), z], 'wall', s < 0 ? '+x' : '-x');
    b.socket('in', [0, 0, 0], '-z', dw * 2, dh, 'arch', ['door']);
    const pts = [[0, 0, Math.round(T / 2)], [0, 0, z0 + b.m(1.5)], [0, 0, zc], [b.m(1.2), top, altarZ - b.m(1.2)]];
    if (P.secretDoor) {
      const sw = b.m(0.5);
      b.carve('box', {min: [-sw, top, zEnd - 2], max: [sw, top + b.m(2.1), zEnd + hw + T]}, {layer: 3, noise: null, tag: 'door'});
      b.socket('back', [0, top, zEnd + hw + T], '+z', sw * 2, b.m(2.1), 'rect', ['secret']);
      pts.push([0, top, zEnd + hw]);
      b.wayChain('w', pts, ['socket:in'], ['socket:back']);
    } else b.wayChain('w', pts, ['socket:in'], []);
  }
};

const COLLAPSED = {
  type: 'collapsed_passage', label: 'Collapsed passage', version: 1,
  size: CORRIDOR.size,
  u: {decay: 0.85, moisture: 0.35, lightCount: 0, lightType: 'torch'},
  rows: [
    {sec: 'Collapse'},
    {k: 'falls', n: 'falls', min: 1, max: 6, step: 1, int: true, def: 1, d: 'Separate roof falls along the passage.'},
    {k: 'blockage', n: 'blockage', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Height of each rubble heap relative to the passage (clamped to the angle of repose).'},
    {k: 'blocks', n: 'fallen blocks', min: 0, max: 12, step: 1, int: true, def: 4, d: 'Dressed blocks from the vault lying in the rubble.'},
    {k: 'cracks', n: 'cracked walls', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Extra wall roughness along the passage.'},
    {k: 'cavity', n: 'roof cavity (m)', min: 0, max: 6, step: 0.1, def: 1.5, d: 'How far up the vault broke open.'},
    {k: 'profile', n: 'profile', kind: 'enum', options: ['arch', 'flat'], def: 'arch', d: ''}
  ],
  PRESETS: {
    fallen: {params: {}},
    blocked: {params: {blockage: 1, falls: 1, length: 16, blocks: 8, cavity: 3}},
    shattered: {params: {falls: 3, blockage: 0.35, cracks: 0.9, length: 20}}
  },
  build(b, P) {
    const W = b.m(P.width, 8), L = b.m(P.length, 20), H = b.m(P.height, 16), C = b.m(P.ceiling);
    const hw = Math.round(W / 2), rise = P.profile === 'arch' ? Math.max(2, Math.min(C || hw, hw * 2)) : 0;
    const prof = P.profile === 'arch' ? 'arch' : 'rect';
    b.passage(0, L, 0, 0, W, H, prof, {rise, noise: 1 + P.cracks * 2});
    b.skin('box', {min: [-hw, 0, 0], max: [hw, H + rise, L]}, {skin: 2, mat: 'wall', noise: 1 + P.cracks * 2, layer: 1});
    b.floor(-hw, 0, hw, L, 0);
    const tops = [];
    for (let i = 0; i < P.falls; i++) {
      const z = Math.round((L * (i + 0.5)) / P.falls) + b.jit(Math.round(L / (4 * P.falls)), 'fallz', i);
      const rz = Math.max(b.m(1.5), Math.round(L / (2 * P.falls)) - 2);
      const want = Math.round(P.blockage * (H + rise));
      b.mound(b.jit(3, 'fallx', i), 0, z, hw + b.m(0.5), rz, want);
      tops.push([z, Math.min(H + rise - b.m(0.6), Math.round(Math.min(want, (b.kit.repose * Math.min(hw + b.m(0.5), rz)) / 3)))]);
      const cav = b.m(P.cavity);
      if (cav > 0) b.carve('ellipsoid', {min: [-hw, H + rise - 3, z - Math.round(rz / 2)], max: [hw, H + rise + cav, z + Math.round(rz / 2)]}, {noise: 2.5, layer: 1, tag: 'room'});
    }
    for (let k = 0; k < P.blocks; k++) {
      const s = b.ri(3, 7, 'blk', k), x = b.ri(-hw + 1, hw - s - 1, 'blk', k, 'x'), z = b.ri(2, L - s - 2, 'blk', k, 'z');
      b.fill('box', {min: [x, 0, z], max: [x + s, Math.max(2, s - 2), z + Math.round(s * 1.4)]}, {mat: 'trim', layer: 4, noise: null, mask: 'air'});
    }
    const pts = [[0, 0, 2]].concat(tops.map(([z, y]) => [0, Math.max(0, y), z]), [[0, 0, L - 2]]);
    b.wayChain('w', pts, ['socket:in'], ['socket:out'], {dig: P.blockage >= 0.95});
    b.socket('in', [0, 0, 0], '-z', W, H, prof);
    b.socket('out', [0, 0, L], '+z', W, H, prof);
  }
};

const RUIN_ENTRANCE = {
  type: 'ruin_entrance', label: 'Ruin entrance', version: 1, surface: true,
  size: {width: {min: 1.5, max: 8, def: 3, n: 'stair width (m)'}, length: null, height: {min: 2, max: 6, def: 3, n: 'headroom (m)'}, ceiling: null},
  u: {lightCount: 2, lightType: 'brazier', decay: 0.55, moisture: 0.3},
  rows: [
    {sec: 'Descent', note: 'The anchor (origin socket) is the top of the stair at grade; the stair runs down +Z to the inner socket.'},
    {k: 'drop', n: 'depth (m)', min: 2, max: 30, step: 0.1, def: 6, d: 'How far the stair goes down.'},
    {k: 'pitch', n: 'pitch', min: 0.3, max: 1, step: 0.05, def: 0.6, d: 'Rise over run.'},
    {k: 'stepHeight', n: 'step height (m)', min: 0.1, max: 0.4, step: 0.02, def: 0.2, d: ''},
    {k: 'foot', n: 'foot passage (m)', min: 1, max: 10, step: 0.1, def: 2, d: 'Level run at the bottom before the inner socket.'},
    {sec: 'Colonnade', note: 'Above ground: a paved platform round the stairwell, columns along it, a parapet.'},
    {k: 'platform', n: 'platform margin (m)', min: 1, max: 12, step: 0.1, def: 3, d: 'Paving beyond the stairwell on each side (the terrain-blend apron: the ground above it is cleared and the gaps under it filled).'},
    {k: 'columns', n: 'columns per side', min: 0, max: 12, step: 1, int: true, def: 4, d: ''},
    {k: 'columnHeight', n: 'column height (m)', min: 1, max: 12, step: 0.1, def: 4.5, d: ''},
    {k: 'columnStyle', n: 'column style', kind: 'enum', options: ['round', 'square', 'fluted', 'broken'], def: 'fluted', d: ''},
    {k: 'lintels', n: 'lintels', kind: 'bool', def: true, d: 'Stone beams along the tops of standing column pairs.'},
    {k: 'toppled', n: 'toppled columns', min: 0, max: 8, step: 1, int: true, def: 2, d: 'Columns lying on the paving.'},
    {k: 'parapet', n: 'parapet (m)', min: 0, max: 2, step: 0.1, def: 0.8, d: 'Low wall along the stairwell edges.'},
    {k: 'overgrowth', n: 'overgrowth', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Ivy and moss on the paving and stones.'}
  ],
  PRESETS: {
    colonnade: {params: {}},
    grand_portico: {params: {width: 5, drop: 10, platform: 6, columns: 6, columnHeight: 7, columnStyle: 'round', toppled: 1, decay: 0.3, overgrowth: 0.2, lightCount: 4}},
    sunken_stair: {params: {width: 2, drop: 5, platform: 1.5, columns: 0, toppled: 0, parapet: 1.2, overgrowth: 0.8, lightType: 'none', lightCount: 0}},
    toppled_ruin: {params: {columns: 5, columnStyle: 'broken', toppled: 5, lintels: false, decay: 0.9, overgrowth: 0.9}}
  },
  build(b, P) {
    const W = b.m(P.width, 10), H = b.m(P.height, 18), hw = Math.round(W / 2);
    const D = b.m(P.drop, 10), run = Math.round(D / P.pitch), foot = b.m(P.foot, 6);
    const open = Math.round((run * Math.min(H, D)) / D);     // where the stair's headroom goes under grade
    const PM = b.m(P.platform, 6), pz0 = -PM, pz1 = open + Math.round(PM / 2);
    // Apron: clear the ground above the platform, fill the gaps under it, pave it.
    b.carve('box', {min: [-hw - PM, 0, pz0], max: [hw + PM, b.m(8), pz1]}, {layer: 1, noise: 0.5, blend: 10, tag: 'apron'});
    b.fill('box', {min: [-hw - PM, -b.m(1.5), pz0], max: [hw + PM, 0, pz1]}, {mat: 'floor', layer: 2, noise: 'floor', mask: 'air'});
    b.fill('box', {min: [-hw - PM, -2, pz0], max: [hw + PM, 0, pz1]}, {mat: 'floor', layer: 2, noise: null});
    // The stair (carved after the paving so it cuts through it).
    const sh = b.m(P.stepHeight, 1), n = Math.max(1, Math.round(D / sh)), R = n * sh;
    b.flight('carve', '-z', -hw, hw, 0, run, -R, R, sh, {height: H}, {tag: 'room', layer: 3});
    b.flight('fill', '-z', -hw, hw, 0, run, -R, R, sh, {below: 2}, {mat: 'trim', layer: 4, noise: null});
    b.flight('skin', '-z', -hw, hw, 0, run, -R, R, sh, {height: H}, {skin: 3, mat: 'wall', layer: 4});
    b.passage(run - 1, run + foot, 0, -R, W, H, 'arch', {rise: Math.min(hw, b.m(1)), layer: 3});
    b.skin('vault', {min: [-hw, -R, run - 1], max: [hw, -R + H + Math.min(hw, b.m(1)), run + foot], axis: 'z', rise: Math.min(hw, b.m(1))}, {skin: 3, mat: 'wall', layer: 4});
    b.floor(-hw, run, hw, run + foot, -R, {layer: 4});
    // Parapet along the open part of the stairwell.
    const ph = b.m(P.parapet);
    if (ph > 0) for (const s of [-1, 1])
      b.fill('box', {min: [s < 0 ? -hw - 3 : hw, 0, 0], max: [s < 0 ? -hw : hw + 3, ph, open]}, {mat: 'wall', layer: 4, noise: null});
    // Colonnade on the platform, either side of the stairwell.
    const cH = b.m(P.columnHeight, 10), cr = b.m(0.35, 2), cx = hw + Math.round(PM / 2);
    const standing = {};
    for (let i = 0; i < P.columns; i++) for (const s of [-1, 1]) {
      const z = Math.round(pz0 + ((pz1 - pz0) * (i + 0.5)) / P.columns);
      const res = column(b, P, s * cx + b.jit(2, 'ccx', i, s), z, 0, cH, cr, P.columnStyle, 'ent' + i + '_' + s);
      standing[i + '_' + s] = !res.broken ? z : null;
      if (i % 2 === 0) b.lightSpot([s * (cx - cr - 1), b.m(1.2), z], 'floor', s < 0 ? '+x' : '-x');
    }
    if (P.lintels) for (const s of [-1, 1]) for (let i = 0; i + 1 < P.columns; i++) {
      const za = standing[i + '_' + s], zb = standing[(i + 1) + '_' + s];
      if (za === null || zb === null || za === undefined || zb === undefined) continue;
      b.fill('box', {min: [s * cx - cr - 1, cH, za - cr], max: [s * cx + cr + 1, cH + b.m(0.5), zb + cr]}, {mat: 'trim', layer: 3, noise: null});
    }
    for (let i = 0; i < P.toppled; i++) {
      const ax = b.r('top', i, 'ax') < 0.5 ? 'x' : 'z', len = b.ri(b.m(1.5), b.m(4), 'top', i, 'len');
      const x = b.ri(-hw - PM + len, hw + PM - len, 'top', i, 'x'), z = b.ri(pz0 + 2, pz1 - 2, 'top', i, 'z');
      if (Math.abs(x) < hw + cr + 2 && z > 0 && z < open) continue;        // not across the stairwell
      const mn = ax === 'x' ? [x - len, 0, z - cr] : [x - cr, 0, z - len], mx = ax === 'x' ? [x + len, 2 * cr, z + cr] : [x + cr, 2 * cr, z + len];
      b.fill('cylinder', {min: mn, max: mx, axis: ax}, {mat: 'support', layer: 4, noise: null, mask: 'air'});
    }
    if (P.overgrowth > 0) {
      const apron = b.prims.findIndex(p => p.tag === 'apron');
      b.scatter.push({prim: apron, kind: 'moss', density: +(P.overgrowth * 0.9).toFixed(3), size: [1, 4], surface: 'floor', mat: 'veg', seedKey: 'overgrowth'});
      b.scatter.push({prim: apron, kind: 'moss', density: +(P.overgrowth * 0.6).toFixed(3), size: [2, 6], surface: 'wall', mat: 'veg', seedKey: 'ivy'});
    }
    b.wayChain('w', [[0, 0, -b.m(1)], [0, -Math.round(R / 2), Math.round(run / 2)], [0, -R, run + Math.round(foot / 2)]], ['socket:surface'], ['socket:in']);
    b.socket('surface', [0, 0, 0], '-z', W + 2 * PM, H, 'rect', ['surface']);
    b.socket('in', [0, -R, run + foot], '+z', W, H, 'arch', ['door']);
  }
};

defineKit(KIT, [RUIN_ENTRANCE, HALL, CORRIDOR, STAIR, CRYPT, PIT_ROOM, SANCTUM, FLOODED_HALL, COLLAPSED]);
export default KIT;
