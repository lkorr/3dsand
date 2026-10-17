#!/usr/bin/env node
// design_world.mjs — builds the shipped world, TIRNHALLOW, into
// assets/worldmap/default/ (docs/WORLD_brief.md is the design, docs/
// WORLD_tirnhallow.md the gazetteer, docs/PLAN_worldbuilder.md wave 3) from
// the World map page's own pure modules: terraingen.js (ranges, erosion,
// landforms), hydrology.js (Priority-Flood, D8 flow), climate.js (auto-
// biomes), riverbake.js (the engine's river bake, for the checks), spline.js
// and maptiles.js (the tile codec). REPRODUCIBLE: same script, same files.
//
//   node scripts/design_world.mjs [--dry] [--png DIR] [--stage terrain]
//     --dry            build and render, write nothing into assets/
//     --png DIR        overview renders (default build/world)
//     --stage terrain  stop after the height plane (the land's renders)
//
// What it keeps of the map that was there: version 2, the extent, the planes'
// settings, the sea (112), the terrain block (its treeline raised for a world
// with mountains), HARROWBY (the locale, its soften site, the spawn, the
// home lake, its eight village paths, refs/ and locales/ untouched) and the
// ground under it: the posts near the village keep the height they had
// (scripts/design_world_harrowby.json, written once from the plane as it
// was) and the new land eases into them. The rest of the height plane, the
// biome plane and the vector layers are this script's.
//
// RE-RUNNING OVERWRITES the planes and map.json's vector layers: hand edits
// made in the tuner after a run are the owner's, and a re-run discards them
// (docs/EDITOR_GUIDE.md section 15.8).
//
// THE PIPELINE (each step a function below, in this order):
//   1  the coast: a hand-drawn continent + islands, domain-warped (4 octaves)
//   2  zones -> the ground before erosion (soft-blended base + relief per zone),
//      the coast ramp per coast kind (cliffs, dunes, deltas), the sea floor
//   3  the lowland valleys the named rivers will take (carved before erosion,
//      so the erosion grows their tributaries)
//   4  the landscape-evolution erosion of the whole land (terraingen
//      erodeLand, on a 51.2 m proxy), then per-zone smoothing (downs, vale,
//      steppe, fen: gentle; the Eldwood and the hills keep their valleys)
//   5  the ranges (terraingen rangeField: uplift + stream power, dendritic),
//      the Greyspine's north face compressed into a steep wall, Kingsgate's col
//   6  Mount Cinder (volcano + its own erosion + a breached caldera), the
//      glacial troughs and the three fjords, Mirrormere's basin and the
//      Merewall, the Mere Stair, the flats (fen, delta, salt marsh)
//   7  the POSTS (6.4 m): bicubic + rock detail, then what 25.6 m cannot hold:
//      tablelands' caprock, Redreach's mesas and buttes, the slot canyon, the
//      Shardspire, the crater, karst towers and sea stacks, the white cliffs,
//      lava flows, dunes, tarns, fen pools, the oasis, Harrowby's ground
//   8  water: Priority-Flood lakes, D8 flow, the named rivers traced down it
//   9  climate -> biomes, the brief's zones enforced (locks)
//  10  roads routed (A*, grade-limited, switchbacks) and conformed, regions,
//      markers, water sites; map.json + the plane tiles written
import {readFileSync, writeFileSync, readdirSync, mkdirSync, existsSync, unlinkSync} from 'node:fs';
import {join, dirname} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {writePNG} from './pngout.mjs';
import * as WL from './worldlib.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const imp = p => import(pathToFileURL(join(ROOT, p)).href);
const TG = await imp('assets/editor/terraingen.js');
const HY = await imp('assets/editor/hydrology.js');
const CL = await imp('assets/editor/climate.js');
const MT = await imp('assets/editor/maptiles.js');
const SPL = await imp('assets/editor/spline.js');
const RB = await imp('assets/editor/riverbake.js');
const {clamp, lerp, sstep} = WL;

const args = process.argv.slice(2);
const flag = k => args.includes(k);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const DRY = flag('--dry'), STAGE = opt('--stage', 'all');
const PNG = opt('--png', join(ROOT, 'build', 'world'));
const MAPDIR = join(ROOT, 'assets', 'worldmap', 'default');

const SEA = 112, VPM = 10, KM = 10000, MIN = -100352, SIZE = 200704, H_DEF = -312;
const V = km => Math.round(km * KM);                    // km -> voxels
const Y = m => SEA + m * VPM;                           // metres above sea -> absolute voxel Y
const P2 = pts => pts.map(p => [V(p[0]), V(p[1])]);     // a km polygon / polyline -> voxels
const kmBox = b => [V(b[0]), V(b[1]), V(b[2]), V(b[3])];
const t0 = performance.now();
const log = s => console.log(`[${((performance.now() - t0) / 1000).toFixed(1).padStart(6)} s] ${s}`);

// The grids: the DESIGN grid at 25.6 m (every large-scale step), the POSTS at
// 6.4 m (the height plane itself). Both post-aligned: sample i at MIN + i * cell.
const CC = 256, NC = SIZE / CC + 1, PCELL = 64, NP = SIZE / PCELL + 1;
const C = TG.makeGrid(NC, NC, MIN, MIN, CC);
const cx = n => C.x0 + (n % C.w) * C.cell, cz = n => C.z0 + ((n / C.w) | 0) * C.cell;

/* ===================================================================================
 * 1. THE COAST (km, x east, z SOUTH: north = -z, the top of the map page)
 * =================================================================================== */
const COAST = [
  // Rimeholt: the north coast (the three fjords are carved into it later)
  [-6.6, -8.6], [-5.6, -8.95], [-4.6, -8.7], [-3.6, -9.05], [-2.6, -8.8], [-1.9, -9.1], [-0.9, -8.7], [0.4, -8.55],
  [1.4, -8.85], [2.6, -8.6], [3.4, -9.0], [4.6, -8.75], [5.6, -8.55], [6.2, -8.95], [7.0, -8.55], [7.6, -8.1],
  [8.05, -7.45], [8.35, -6.6],
  // the east coast: Sunscour (a bight), Redreach (the Redmouth)
  [8.65, -5.8], [8.55, -5.1], [9.0, -4.5], [8.95, -3.9], [8.4, -3.45], [8.25, -2.8], [8.7, -2.2], [9.15, -1.2], [8.95, -0.2],
  [8.4, 0.3], [7.85, 0.75], [7.9, 1.25], [8.5, 1.6], [9.1, 2.4], [9.2, 3.3], [8.85, 4.1], [9.05, 5.0], [8.8, 5.9],
  // Ashfall's black-sand corner
  [9.0, 6.8], [8.75, 7.8], [8.1, 8.6], [7.2, 8.95], [6.3, 9.15], [5.3, 8.9], [4.6, 9.0],
  // Gull Bay
  [4.25, 8.5], [4.05, 7.8], [3.6, 7.2], [2.9, 6.75], [2.2, 6.5], [1.6, 6.7], [1.15, 7.25], [0.85, 8.0], [0.6, 8.65],
  // the Whitecliff coast
  [0.0, 8.9], [-0.9, 8.7], [-1.8, 8.95], [-2.8, 8.75], [-3.5, 8.45],
  // the Teeth Sound
  [-3.85, 7.9], [-4.1, 7.2], [-4.5, 6.6], [-4.95, 6.75], [-5.2, 7.4], [-5.05, 8.1], [-5.35, 8.85],
  // the Verdant Teeth
  [-6.0, 9.15], [-6.9, 8.85], [-7.6, 9.2], [-8.4, 8.85], [-9.0, 8.4], [-9.2, 7.6], [-8.75, 7.0], [-9.05, 6.3], [-8.9, 5.6],
  // Saltreach and the Eldwater's firth
  [-9.15, 4.8], [-8.95, 3.8], [-9.15, 2.7], [-9.0, 1.9], [-8.3, 1.55], [-7.9, 1.2], [-8.35, 0.85], [-9.05, 0.55],
  [-9.15, -0.6], [-8.95, -1.6], [-9.1, -2.6], [-8.7, -3.5], [-8.2, -4.1],
  // the Greyspine's drowned west end
  [-7.85, -4.6], [-7.6, -5.3], [-7.35, -6.1], [-7.4, -6.9], [-7.0, -7.6], [-6.85, -8.2],
];
const circle = (x, z, r, n = 14) => Array.from({length: n}, (_, k) => [x + r * Math.cos(2 * Math.PI * k / n), z + r * Math.sin(2 * Math.PI * k / n)]);
const ISLANDS = [
  // the Mere Isles: the Greyspine going under the sea, north-west
  circle(-8.3, -6.0, 0.45), circle(-8.95, -6.85, 0.3), circle(-7.95, -7.45, 0.55), circle(-8.7, -7.95, 0.26),
  circle(-9.15, -5.45, 0.22), circle(-7.75, -8.55, 0.3), circle(-9.2, -7.6, 0.16),
  // the Teeth's two karst islets
  circle(-9.45, 7.85, 0.18, 10), circle(-4.62, 9.3, 0.2, 10),
  // the Saltreach spit, a lagoon behind it
  [[-9.05, -2.75], [-9.32, -2.1], [-9.42, -1.0], [-9.42, 0.15], [-9.37, 0.3], [-9.34, -0.9], [-9.3, -1.9], [-9.0, -2.55]],
];
const COAST_WARP = [[11000, 40000, 501], [4000, 12000, 511], [1300, 3500, 521], [350, 900, 531]];
function coastWarp(x, z) {
  let wx = 0, wz = 0;
  for (const [A, L, s] of COAST_WARP) { wx += A * TG.fbm(x / L, z / L, s, 3); wz += A * TG.fbm(x / L + 4.1, z / L - 2.3, s + 4, 3); }
  return [wx, wz];
}
const SD_RAW = WL.sdf(C, [P2(COAST), ...ISLANDS.map(P2)]);
/** The warped signed distance to the coast (voxels, + = land) at world (x, z). */
const coastSD = (x, z) => { const [wx, wz] = coastWarp(x, z); return WL.bil(C, SD_RAW, x + wx, z + wz); };
/** The sea floor at signed distance d < 0: a shelf, then the open sea (the plane's default, -42.4 m). */
function seaFloor(d) {
  const v = SEA - (14 + 410 * (1 - Math.exp(d / 9000)));
  return v < H_DEF + 6 ? H_DEF : v;
}

/* ===================================================================================
 * 2. ZONES: base height (m above sea) + relief (amp m, wavelength L km, style),
 * blended softly (blur r cells; outline domain-warped by warp m at warpL km).
 * smooth: [blur cells, strength] after the erosion (gentle lowlands).
 * =================================================================================== */
const ZONES = [
  {id: 'default', poly: [[-10, -10], [10, -10], [10, 10], [-10, 10]], base: 45, amp: 25, L: 2.5, r: 0, wt: 0.02},
  {id: 'hearthvale', poly: [[-2.4, -0.5], [3.0, -0.5], [3.1, 5.3], [0.6, 6.4], [-1.2, 5.5], [-1.6, 2.6], [-2.6, 1.2]], base: 18, amp: 16, L: 1.4, r: 14, smooth: [4, 0.65]},
  {id: 'amberwood', poly: [[-4.3, -2.7], [-0.7, -3.0], [-0.6, -1.1], [-1.4, 0.0], [-2.4, 0.6], [-4.2, 0.9]], base: 130, amp: 80, L: 1.2, style: 'ridged', r: 16, smooth: [2, 0.35]},
  {id: 'heatherhigh', poly: [[-7.7, -4.7], [-6.6, -4.95], [-5.5, -4.7], [-4.4, -5.0], [-3.4, -4.8], [-3.6, -4.0], [-3.2, -3.2], [-3.5, -2.2], [-4.4, -1.9], [-5.0, -1.6], [-6.0, -1.9], [-6.9, -1.6], [-7.6, -1.8], [-7.4, -2.7], [-7.8, -3.6]], base: 440, amp: 45, L: 1.6, r: 6, warp: 450, warpL: 1.1, lift: 420},
  {id: 'eldwood', poly: [[-8.7, -1.5], [-4.4, -1.4], [-4.3, 2.2], [-5.0, 4.2], [-8.7, 4.4]], base: 95, amp: 80, L: 1.3, style: 'ridged', r: 16},
  {id: 'glimmerfen', poly: [[-4.1, 2.5], [-1.5, 2.4], [-1.25, 5.6], [-2.4, 6.1], [-4.25, 5.8]], base: 8.5, amp: 2, L: 0.8, r: 12, smooth: [4, 0.9]},
  {id: 'saltreach', poly: [[-9.5, -3.2], [-8.2, -3.3], [-7.9, -2.4], [-8.35, -1.5], [-7.85, -0.6], [-8.25, 0.3], [-7.7, 1.1], [-8.3, 2.0], [-8.0, 2.9], [-8.45, 3.8], [-8.1, 4.6], [-8.3, 5.2], [-9.5, 5.4]], base: 9, amp: 5, L: 0.9, r: 12, smooth: [3, 0.7], warp: 550, warpL: 1.3},
  {id: 'teeth', poly: [[-9.5, 5.2], [-5.2, 5.3], [-4.8, 6.5], [-4.9, 9.6], [-9.5, 9.6]], base: 35, amp: 25, L: 1.6, r: 14, smooth: [3, 0.5]},
  {id: 'downs', poly: [[-3.9, 5.5], [0.9, 5.4], [1.1, 7.2], [0.5, 9.2], [-3.9, 9.2]], base: 95, amp: 40, L: 2.4, r: 16, smooth: [6, 0.85], lift: 85},
  {id: 'gullbay', poly: [[0.9, 5.4], [4.4, 5.6], [4.6, 9.2], [0.6, 9.2]], base: 10, amp: 8, L: 1.2, r: 14, smooth: [4, 0.7]},
  {id: 'steppe', poly: [[2.6, -3.0], [3.6, -2.7], [4.4, -2.9], [5.2, -2.6], [5.0, -1.4], [5.4, 0.5], [4.9, 1.3], [5.0, 2.2], [4.0, 1.9], [2.8, 2.0], [3.2, 1.1], [3.0, 0.4], [2.7, -0.8], [3.0, -1.9]], base: 180, amp: 25, L: 3, r: 8, warp: 400, warpL: 1.4, smooth: [4, 0.5], lift: 175},
  {id: 'shardspire', poly: [[3.9, -5.4], [4.8, -5.7], [5.9, -5.5], [6.8, -5.7], [7.3, -5.2], [6.9, -4.4], [7.25, -3.6], [6.8, -2.9], [7.0, -2.5], [6.1, -2.2], [5.2, -2.4], [4.5, -2.7], [4.0, -3.3], [4.3, -4.2]], base: 430, amp: 50, L: 1.4, r: 5, warp: 550, warpL: 0.9, smooth: [6, 0.85], lift: 440},
  {id: 'redreach', poly: [[5.0, -1.3], [8.6, -1.5], [8.7, 4.6], [5.2, 4.6], [4.9, 1.0]], base: 150, amp: 20, L: 2, r: 10, smooth: [5, 0.85]},
  {id: 'sunscour', poly: [[7.6, -4.6], [9.4, -4.6], [9.4, -1.0], [7.6, -1.1]], base: 28, amp: 12, L: 1.5, r: 16, smooth: [4, 0.7]},
  {id: 'goldsea', poly: [[3.0, 2.0], [5.4, 2.1], [5.6, 5.2], [4.4, 5.8], [3.1, 5.6]], base: 60, amp: 18, L: 2.5, r: 18, smooth: [5, 0.75]},
  {id: 'ashfall', poly: [[4.6, 5.0], [9.4, 4.8], [9.4, 9.4], [4.6, 9.4]], base: 35, amp: 14, L: 1.6, r: 16, smooth: [3, 0.5]},
  {id: 'rimeholt', poly: [[-7.2, -9.4], [9.0, -9.4], [9.0, -7.9], [-7.2, -7.9]], base: 150, amp: 110, L: 0.9, style: 'ridged', r: 10, warp: 600, warpL: 1.2},
  {id: 'mereisles', poly: [[-9.6, -8.9], [-7.4, -8.9], [-7.4, -5.0], [-9.6, -5.0]], base: 45, amp: 40, L: 0.6, style: 'ridged', r: 4},
  {id: 'mirrormere', poly: [[-0.8, -3.2], [2.6, -3.2], [2.7, -0.7], [-0.6, -0.7]], base: 75, amp: 25, L: 1.0, r: 14, smooth: [3, 0.4]},
];
// coast character: how far inland the land takes to reach its zone's height (m)
const COAST_KIND = [
  {poly: [[-4.0, 7.6], [0.7, 7.6], [0.7, 9.6], [-4.0, 9.6]], ramp: 45, r: 6},          // Whitecliff: the white cliffs
  {poly: [[-9.6, 5.6], [-5.0, 5.6], [-5.0, 9.6], [-9.6, 9.6]], ramp: 120, r: 6},        // the Teeth: sea-cave cliffs
  {poly: [[-9.6, -3.5], [-8.0, -3.5], [-8.0, 5.4], [-9.6, 5.4]], ramp: 900, r: 10},     // Saltreach: dunes, gentle
  {poly: [[0.6, 6.0], [4.6, 6.0], [4.6, 9.4], [0.6, 9.4]], ramp: 1000, r: 10},          // Gull Bay: the delta
  {poly: [[-7.2, -9.6], [9.4, -9.6], [9.4, -8.0], [-7.2, -8.0]], ramp: 110, r: 10},     // Rimeholt: the fjord coast, steep
  {poly: [[-9.6, -9.4], [-7.0, -9.4], [-7.0, -4.4], [-9.6, -4.4]], ramp: 140, r: 6},    // Mere Isles + the spine's end
];
const ZONE_MASK = {};
function zoneMask(id, r = null) {
  const Z = ZONES.find(z => z.id === id);
  return WL.softMask(C, P2(Z.poly), {warpAmp: (Z.warp || 250) * VPM, warpL: (Z.warpL || 2.2) * KM, seed: Z.base * 7 + 13, r: r == null ? Z.r : r});
}
function buildBase(G) {
  const N = G.w * G.hh, num = new Float32Array(N), den = new Float32Array(N);
  for (const Z of ZONES) {
    const m = Z.id === 'default' ? null : (ZONE_MASK[Z.id] = zoneMask(Z.id));
    const L = Z.L * KM, amp = Z.amp * VPM, sd0 = (Z.base * 31 + 7) | 0;
    for (let n = 0; n < N; n++) {
      const w = m ? m[n] : Z.wt;
      if (w < 1e-4) continue;
      const x = cx(n), z = cz(n);
      let nz;
      if (Z.style === 'ridged') { const [qx, qz] = TG.warp(x / L, z / L, sd0 + 3, 0.5); nz = TG.ridged(qx, qz, sd0, 5) * 1.6 - 0.75; }
      else nz = TG.fbm(x / L, z / L, sd0, 5);
      num[n] += w * (Z.base * VPM + amp * nz);
      den[n] += w;
    }
  }
  const ramp = new Float32Array(N).fill(500 * VPM);
  for (const K of COAST_KIND) {
    const m = WL.softMask(G, P2(K.poly), {warpAmp: 1500, warpL: 15000, seed: K.ramp, r: K.r});
    for (let n = 0; n < N; n++) if (m[n] > 0) ramp[n] = ramp[n] * (1 - m[n]) + K.ramp * VPM * m[n];
  }
  for (let n = 0; n < N; n++) {
    const inland = num[n] / Math.max(1e-6, den[n]), d = SD[n];
    // the shore: 0.8 m at the waterline, then the ramp up to the zone's ground
    if (d >= 0) G.h[n] = Math.max(SEA + 8, SEA + 8 + (inland - SEA - 8) * Math.pow(sstep(0, ramp[n], d), 0.85));
    else G.h[n] = seaFloor(d);
  }
}
/** UPLIFT: the erosion wears a raised zone (a plateau, the downs) far below its
 *  design; each zone with `lift` m is raised as a block by the difference between
 *  that and its mean, through its soft mask -- the eroded relief rides up with
 *  it and the mask's edge becomes the escarpment. */
function liftZones(G) {
  for (const Z of ZONES) {
    if (Z.lift == null) continue;
    const m = WL.softMask(G, P2(Z.poly), {warpAmp: 9000, warpL: 20000, seed: Z.base * 3 + 1, r: Z.r + 2});
    { const m2 = WL.softMask(G, P2(Z.poly), {warpAmp: 6000, warpL: 3500, seed: Z.base * 5 + 7, r: 2}); for (let n = 0; n < m.length; n++) m[n] = 0.5 * m[n] + 0.5 * m2[n]; }
    let sum = 0, cnt = 0;
    for (let n = 0; n < G.h.length; n++) if (m[n] > 0.7 && G.h[n] > SEA) { sum += G.h[n]; cnt++; }
    const lift = Y(Z.lift) - sum / Math.max(1, cnt);
    if (lift <= 0) continue;
    for (let n = 0; n < G.h.length; n++) if (G.h[n] > SEA && SD[n] > 0) G.h[n] += lift * sstep(0.05, 0.65, m[n]) * sstep(0, 3000, SD[n]);
    log(`lift ${Z.id}: +${(lift / VPM).toFixed(0)} m`);
  }
}
function smoothZones(G) {
  for (const Z of ZONES) {
    if (!Z.smooth) continue;
    const [r, s] = Z.smooth, m = ZONE_MASK[Z.id], b = TG.blur(G.h, G.w, G.hh, r, 2);
    for (let n = 0; n < G.h.length; n++) if (m[n] > 0 && G.h[n] > SEA) G.h[n] = lerp(G.h[n], Math.max(SEA + 4, b[n]), m[n] * s);
  }
}

/* ===================================================================================
 * 3. VALLEYS the drainage is steered down (the named rivers are TRACED from the
 * final ground's flow, step 8; these make sure the flow goes where the story
 * says). [x, z, floor m] knots; hw km; core = share of flat floor; keep = how
 * much of the ground's own relief the floor keeps; wig = meander (m). `late`
 * valleys are cut after the erosion (the Mere Stair through the moraine).
 * =================================================================================== */
const VALLEYS = [
  {id: 'silverrun', late: true, hw: 0.6, core: 0.35, keep: 0.12, wig: 160, pts: [[1.3, -0.45, 13], [1.05, 0.3, 9], [0.95, 1.4, 8.2],
    [1.15, 2.6, 7.4], [1.2, 2.9, 7.2], [1.55, 4.3, 6.2], [1.9, 5.5, 4.8], [2.15, 6.35, 2.0]]},
  {id: 'kingsdale', late: true, hw: 0.4, core: 0.45, keep: 0.08, wig: 120, pts: [[-1.5, -5.3, 600], [-1.35, -4.6, 440], [-1.1, -3.9, 290], [-0.8, -3.2, 150], [-0.45, -2.6, 55]]},
  {id: 'merestair', late: true, hw: 0.28, core: 0.3, keep: 0.05, wig: 0, pts: [[1.5, -1.12, 44], [1.47, -0.85, 42], [1.4, -0.62, 24], [1.3, -0.4, 13], [1.2, -0.1, 10]]},
  {id: 'eldwater', hw: 0.9, core: 0.3, keep: 0.2, wig: 260, pts: [[-5.6, -1.6, 200], [-5.9, -0.8, 100], [-6.3, -0.1, 55], [-7.0, 0.6, 28],
    [-7.6, 1.0, 10], [-8.1, 1.1, 0.5]]},
  {id: 'glimmer', late: true, hw: 0.4, core: 0.5, keep: 0.1, wig: 120, pts: [[-1.9, 3.6, 9.5], [-2.45, 4.25, 8.8], [-2.55, 4.95, 8.0], [-3.3, 5.15, 7.2], [-3.55, 5.8, 6.0], [-4.2, 6.05, 3.6], [-4.55, 6.65, 1.2]]},
  {id: 'teethwater', hw: 0.35, core: 0.4, keep: 0.2, wig: 120, pts: [[-6.2, 6.3, 60], [-7.0, 7.5, 25], [-7.45, 8.4, 6], [-7.6, 9.0, 0.5]]},
];
/** A knot line [[x, z, attr], ...] (voxels) as a dense polyline pushed off its
 *  spline by a meander of `amp` voxels (1D noise along the arc, tapered at both
 *  ends), the attribute carried along. */
function wiggle(pts, amp, seed, step = 64) {
  const T = SPL.tessellate(pts, {step});
  const N = T.pts.length, L = T.length, out = [];
  for (let i = 0; i < N; i++) {
    const s = T.s[i], a = T.pts[Math.max(0, i - 1)], b = T.pts[Math.min(N - 1, i + 1)];
    let tx = b[0] - a[0], tz = b[1] - a[1];
    const tl = Math.hypot(tx, tz) || 1; tx /= tl; tz /= tl;
    const taper = Math.min(1, s / 4000, (L - s) / 4000);
    const o = amp * taper * (0.7 * TG.noise1(s / 14000, seed) + 0.3 * TG.noise1(s / 4500, seed + 9)) * 2;
    out.push([T.pts[i][0] - tz * o, T.pts[i][1] + tx * o, T.pts[i][2]]);
  }
  return out;
}
const VALLEY_LINES = {};
function carveValley(G, v) {
  const craterFloor = () => (groundAt(V(CRATER.x), V(CRATER.z)) - SEA) / VPM;
  const pts = v.pts.map(p => [V(p[0]), V(p[1]), Y(typeof p[2] === 'string' ? craterFloor() + +p[2].slice(6) : p[2])]);
  const line = wiggle(pts, (v.wig || 0) * VPM, v.id.length * 97 + 5);
  VALLEY_LINES[v.id] = line;
  const hw = V(v.hw), Cc = TG.curveCoords(G, line, hw, 1);
  for (let n = 0; n < G.h.length; n++) {
    const d = Cc.d[n];
    if (!isFinite(d)) continue;
    const u = Math.sqrt(d * d + Cc.beyond[n] * Cc.beyond[n]) / hw;
    if (u >= 1) continue;
    const fl = Cc.attrs[0][n];
    if (G.h[n] <= fl) continue;
    const target = fl + (G.h[n] - fl) * (v.keep || 0) * sstep(0, 0.5, u);
    G.h[n] = lerp(G.h[n], target, 1 - sstep(v.core, 1, u));
  }
}

/* ===================================================================================
 * 5. THE RANGES. [x, z, crest m, half-width km]. Kingsgate Pass is the col at
 * (-1.6, -6.05), cut to 780 m (PASS); Hollowcrown the high knot (0.35, -6.7).
 * The Greyspine's NORTH flank is compressed (northK) into a steep face.
 * =================================================================================== */
const GREYSPINE = [
  [-7.7, -4.9, 650, 1.3], [-6.2, -5.55, 1250, 1.9], [-4.6, -6.15, 1500, 2.3], [-3.1, -6.35, 1420, 2.4],
  [-1.6, -6.05, 950, 2.1], [-0.5, -6.45, 1550, 2.5], [0.35, -6.7, 1950, 2.7], [1.9, -6.8, 1650, 2.6],
  [3.6, -6.7, 1450, 2.4], [5.3, -6.3, 1300, 2.2], [7.0, -5.6, 1050, 1.9], [8.3, -5.1, 520, 1.3],
];
const RANGES = [
  {id: 'range_greyspine', name: 'The Greyspine', points: GREYSPINE, profile: 'alpine', seed: 11, peaks: 0.6, spurs: 0.5, foothills: 0.6, erode: 0.85, wander: 0.7, northK: 1.75},
];
const PASS = {pts: [[-1.68, -6.8, 640], [-1.6, -6.05, 780], [-1.5, -5.3, 610]], hw: 0.42};
const HOLLOWCROWN = {x: 0.35, z: -6.7, top: 1950, r: 0.7};
const RANGE_RECORDS = [];
function bakeRanges(G) {
  for (const r0 of RANGES) {
    const r = {...r0, points: r0.points.map(p => [V(p[0]), V(p[1]), Y(p[2]), V(p[3])])};
    const sg = TG.subGrid(G, TG.rangeBox(r));
    const before = Float32Array.from(sg.h);
    const out = TG.rangeField(r, sg, {seaY: SEA});
    // no range on the sea floor: the relief fades out where the ground before it was under the sea
    for (let n = 0; n < out.h.length; n++) out.h[n] = before[n] + (out.h[n] - before[n]) * sstep(SEA - 60, SEA + 40, before[n]);
    sg.h = out.h;
    if (r0.northK) compressNorth(sg, before, r, r0.northK);
    TG.pasteGrid(G, sg);
    const {northK, ...rec} = {...r0, points: r.points, baked: true};
    RANGE_RECORDS.push(rec);
    log(`range ${r.id}: ${sg.w}x${sg.hh} cells`);
  }
}
/** The north flank of a range made steeper: a column at distance d north of
 *  the crest takes the range's relief from distance d * k(d) (k rising from 1
 *  at the crest to K by 300 m out), so the flank is K times shorter. */
function compressNorth(S, ground, r, K) {
  const T = SPL.tessellate(r.points.map(p => [p[0], p[1]]), {step: 128});
  const reach = TG.rangeReach(r);
  const Cc = TG.curveCoords(S, T.pts, reach, 0);
  const rel = new Float32Array(S.w * S.hh);
  for (let n = 0; n < rel.length; n++) rel[n] = S.h[n] - ground[n];
  for (let j = 0; j < S.hh; j++)
    for (let i = 0; i < S.w; i++) {
      const n = j * S.w + i, d = Cc.d[n];
      if (!isFinite(d) || d >= 0) continue;          // d < 0: left of travel = north
      const k = 1 + (K - 1) * sstep(0, 3000, -d);
      const Pp = SPL.pointAt(T, Cc.s[n]), Q = SPL.pointAt(T, Math.min(T.length, Cc.s[n] + 64));
      let tx = Q[0] - Pp[0], tz = Q[1] - Pp[1];
      const tl = Math.hypot(tx, tz) || 1; tx /= tl; tz /= tl;
      const sx = Pp[0] + tz * (-d * k), sz = Pp[1] - tx * (-d * k);
      const fx = (sx - S.x0) / S.cell, fz = (sz - S.z0) / S.cell;
      const fade = 1 - sstep(0.7 * reach, 0.92 * reach, -d * k);
      const v = fade <= 0 || fx < 0 || fz < 0 || fx > S.w - 1 || fz > S.hh - 1 ? 0 : fade * WL.bil(S, rel, sx, sz);
      S.h[n] = ground[n] + Math.max(0, v) * sstep(SEA - 60, SEA + 40, ground[n]);
    }
}
/** A col carved to a profile (only lowers); a summit raised to its height. */
function passAndSummit(G) {
  const pts = PASS.pts.map(p => [V(p[0]), V(p[1]), Y(p[2])]);
  const hw = V(PASS.hw), Cc = TG.curveCoords(G, pts, hw, 1);
  for (let n = 0; n < G.h.length; n++) {
    const d = Cc.d[n];
    if (!isFinite(d)) continue;
    const u = Math.sqrt(d * d + Cc.beyond[n] * Cc.beyond[n]) / hw;
    if (u >= 1) continue;
    const fl = Cc.attrs[0][n] + 1200 * u * u;       // a saddle: rising to the shoulders
    if (G.h[n] > fl) G.h[n] = lerp(G.h[n], fl, 1 - sstep(0.4, 1, u));
  }
  // Hollowcrown: the range's local summit lifted to its height (the relief scaled up
  // over a radius, the shape the erosion gave it kept)
  const hx = V(HOLLOWCROWN.x), hz = V(HOLLOWCROWN.z), R = V(HOLLOWCROWN.r), top = Y(HOLLOWCROWN.top);
  let peak = -Infinity;
  for (let n = 0; n < G.h.length; n++) if (Math.hypot(cx(n) - hx, cz(n) - hz) < R) peak = Math.max(peak, G.h[n]);
  const base = Y(900), k = (top - base) / Math.max(1, peak - base);
  if (k > 1) for (let n = 0; n < G.h.length; n++) {
    const d = Math.hypot(cx(n) - hx, cz(n) - hz);
    if (d > R * 2 || G.h[n] <= base) continue;
    G.h[n] = base + (G.h[n] - base) * lerp(k, 1, sstep(R * 0.6, R * 2, d));
  }
  log(`Hollowcrown: the range made ${((peak - SEA) / VPM).toFixed(0)} m there, lifted x${k.toFixed(2)}`);
}

/* ===================================================================================
 * 6. CINDER, troughs, fjords, Mirrormere, the flats
 * =================================================================================== */
// Mount Cinder: a STRATOVOLCANO. Concave flanks steepening to the summit
// (profile (1 - r)^p), radial gullies (ridged noise read round a circle, so the
// angle has no seam), a small deep summit crater with a jagged lip, breached to
// the south-east, and parasitic cones [x, z, radius km, height m] on the flanks.
const CINDER = {x: 6.4, z: 6.8, radius: 2.9, height: 1180, crater: 0.085, craterDepth: 150, seed: 61, breach: [[6.45, 6.8], [6.85, 7.25], [7.35, 7.7]],
  cones: [[5.45, 6.05, 0.26, 140], [7.45, 6.2, 0.2, 110], [5.95, 7.95, 0.18, 90], [7.15, 7.55, 0.14, 70], [5.2, 7.2, 0.15, 65]]};
function cinder(G) {
  const sg = TG.subGrid(G, kmBox([CINDER.x - 3.4, CINDER.z - 3.4, CINDER.x + 3.4, CINDER.z + 3.4]));
  const before = Float32Array.from(sg.h);
  const X = V(CINDER.x), Z = V(CINDER.z), R = V(CINDER.radius), H = CINDER.height * VPM, sd = CINDER.seed;
  const cR = V(CINDER.radius * CINDER.crater), cD = CINDER.craterDepth * VPM;
  for (let n = 0; n < sg.h.length; n++) {
    const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
    // a lopsided cone: the radius read at a warped position, the summit off-centre
    const wk = sstep(V(0.2), V(1.4), Math.hypot(x - X, z - Z));
    const qx = x + wk * (2200 * TG.fbm(x / 14000, z / 14000, 71, 3) + 500 * TG.fbm(x / 3500, z / 3500, 73, 3));
    const qz = z + wk * (2200 * TG.fbm(x / 14000 + 3.1, z / 14000 + 1.7, 75, 3) + 500 * TG.fbm(x / 3500 + 2.2, z / 3500 - 1.3, 77, 3));
    const dx = qx - X, dz = qz - Z, d = Math.hypot(dx, dz), rho = d / R;
    if (rho >= 1.2) continue;
    const ca = dx / (d || 1), sa = dz / (d || 1);
    // the profile: steep cone over a broad apron of tephra and old flows
    const f = 0.82 * Math.pow(Math.max(0, 1 - rho), 2.3) + 0.18 * Math.pow(Math.max(0, 1 - rho / 1.2), 1.2);
    // radial gullies: ridged noise round a circle of radius k (no seam at +-pi),
    // drifting with r so they wander; deepest mid-flank
    const g = TG.ridged(6.5 * ca + 1.4 * rho, 6.5 * sa - 1.1 * rho, sd + 3, 3);
    const g2 = TG.ridged(15 * ca - 2.1 * rho, 15 * sa + 1.7 * rho, sd + 5, 2);
    // sectors differ: some deeply gullied (old, eroded), some smooth (young flows over them)
    const sector = sstep(-0.25, 0.35, TG.fbm(1.8 * ca + 0.4 * rho, 1.8 * sa, sd + 13, 3));
    const gully = H * (0.11 * Math.pow(g, 3) * (0.3 + 0.7 * sector) + 0.04 * Math.pow(g2, 3) * sector) * sstep(0.06, 0.25, rho) * (1 - sstep(0.7, 1.05, rho));
    let h = before[n] + H * f - gully;
    // the summit crater: a deep bowl inside a jagged lip
    const rd = Math.hypot(x - X, z - Z), ra = Math.atan2(z - Z, x - X);
    const rimR = cR * (1 + 0.22 * TG.fbm(2.2 * Math.cos(ra), 2.2 * Math.sin(ra), sd + 9, 3));
    if (rd < rimR * 1.7) {
      const top = before[n] + H * 0.82 * Math.pow(1 - rimR / R, 2.3) + 0.18 * H;
      const lip = top + 55 * VPM * TG.ridged(4 * Math.cos(ra), 4 * Math.sin(ra), sd + 11, 3);
      if (rd < rimR) h = Math.min(h, lip - cD * Math.pow(Math.max(0, 1 - (rd / rimR) ** 2), 0.6) - 8 * VPM);
      else h = lerp(lip, h, sstep(rimR, rimR * 1.7, rd));
    }
    if (h > sg.h[n]) sg.h[n] = h;
  }
  // parasitic cones, each with its own small crater
  CINDER.cones.forEach(([cx0, cz0, cr, ch], k) => {
    const CX = V(cx0), CZ = V(cz0), CR = V(cr);
    for (let n = 0; n < sg.h.length; n++) {
      const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
      const [wx, wz] = TG.warp((x - CX) / CR, (z - CZ) / CR, sd + 20 + k, 0.25, 2);
      const q = Math.hypot(wx, wz);
      if (q >= 1) continue;
      const cone = ch * VPM * (Math.pow(1 - q, 1.25) - 0.5 * Math.max(0, 1 - q / 0.28));
      sg.h[n] += cone;
    }
  });
  TG.pasteGrid(G, sg);
  // the breach: the caldera rim broken to the south-east, a ravine the lava left by
  const fl = [Y(CINDER.height - 150), Y(CINDER.height - 330), Y(CINDER.height - 560)];
  const pts = wiggle(CINDER.breach.map((p, k) => [V(p[0]), V(p[1]), fl[k]]), 600, 911, 128);
  const hw = 900, Cc = TG.curveCoords(G, pts, hw, 1);
  for (let n = 0; n < G.h.length; n++) {
    const d = Cc.d[n];
    if (!isFinite(d)) continue;
    const u = Math.sqrt(d * d + Cc.beyond[n] * Cc.beyond[n]) / hw;
    if (u < 1 && G.h[n] > Cc.attrs[0][n]) G.h[n] = lerp(G.h[n], Cc.attrs[0][n] + 600 * u, 1 - sstep(0.3, 1, u));
  }
}
const TROUGHS = [
  // the glacier tongue's trough, the ice reaching the sea
  {id: 'u_tongue', hw: 0.4, depth: 70, pts: [[5.0, -6.2], [5.25, -7.2], [5.45, -8.1], [5.6, -8.8]]},
];
function carveTroughs(G) {
  for (const t of TROUGHS) {
    const pts = P2(t.pts);
    const xs = pts.map(p => p[0]), zs = pts.map(p => p[1]), pad = V(t.hw) * 2;
    const sg = TG.subGrid(G, [Math.min(...xs) - pad, Math.min(...zs) - pad, Math.max(...xs) + pad, Math.max(...zs) + pad]);
    TG.uValley(sg, {points: pts, width: V(t.hw), depth: t.depth * VPM, seaY: SEA, seed: 3, minY: SEA + 20 * VPM});
    TG.pasteGrid(G, sg);
  }
}
/* ---- FJORDS: flooded glacial troughs, carved on the posts into the raised
 * Rimeholt highland. Each has a sinuous TRUNK (head -> mouth, hw [head, mouth]
 * km, its width wandering), side ARMS (tip -> where they leave the trunk) and
 * HANGING valleys (a side valley whose floor stops [m] high on the fjord wall:
 * the waterfalls). U profile: a flat floor, walls steepening to near-vertical,
 * a shoulder above; the floor deepens seaward to `depth` m below the sea and
 * rises to a sill at the mouth. ---- */
const FJORDS = [
  {id: 'fjord_west', hw: [0.13, 0.3], depth: 70, seed: 3, valley: [[-3.9, -6.0, 520], [-4.05, -6.6, 250], [-4.2, -7.25, 3]],
   pts: [[-4.2, -7.2], [-4.5, -7.7], [-4.25, -8.25], [-4.7, -8.8], [-4.5, -9.3], [-4.85, -9.85]],
   arms: [{pts: [[-3.3, -8.35], [-3.75, -8.6], [-4.3, -8.4]], hw: [0.07, 0.13]},
          {pts: [[-5.6, -8.0], [-5.35, -8.5], [-4.65, -8.85]], hw: [0.06, 0.12]}],
   hanging: [[[-3.7, -7.75], [-4.25, -7.85], 260], [[-5.15, -7.6], [-4.55, -7.75], 210], [[-3.6, -9.05], [-4.35, -9.1], 140]]},
  {id: 'fjord_kings', hw: [0.15, 0.32], depth: 60, seed: 5, valley: [[-1.55, -6.25, 600], [-1.62, -6.9, 230], [-1.75, -7.62, 3]],
   pts: [[-1.75, -7.62], [-2.05, -8.05], [-1.8, -8.6], [-2.2, -9.15], [-2.0, -9.85]],
   arms: [{pts: [[-0.85, -8.7], [-1.3, -8.9], [-1.85, -8.65]], hw: [0.07, 0.12]}],
   hanging: [[[-2.75, -8.2], [-2.15, -8.25], 180], [[-1.05, -8.1], [-1.75, -8.2], 230]]},
  {id: 'fjord_east', hw: [0.11, 0.26], depth: 45, seed: 7, valley: [[2.75, -6.1, 560], [2.9, -6.9, 230], [3.0, -7.75, 3]],
   pts: [[3.0, -7.75], [3.3, -8.3], [3.1, -8.85], [3.45, -9.35], [3.3, -9.85]],
   arms: [{pts: [[4.25, -8.4], [3.8, -8.75], [3.2, -8.9]], hw: [0.08, 0.13]}],
   hanging: [[[2.35, -8.45], [2.95, -8.5], 190]]},
  {id: 'fjord_skar', hw: [0.07, 0.18], depth: 30, seed: 9, pts: [[-6.15, -8.15], [-5.95, -8.7], [-6.3, -9.25], [-6.15, -9.85]], arms: [], hanging: []},
  {id: 'fjord_tongue', hw: [0.17, 0.3], depth: 40, seed: 11, pts: [[5.55, -8.55], [5.85, -9.1], [5.7, -9.85]], arms: [], hanging: [[[6.4, -9.0], [5.95, -9.05], 120]]},
  {id: 'fjord_isvik', hw: [0.09, 0.22], depth: 35, seed: 13, pts: [[7.35, -8.2], [7.15, -8.75], [7.55, -9.3], [7.45, -9.9]], arms: [], hanging: []},
];
let FJ_CUT = 0;
function fjordCut(pts, hw0, hw1, depth, seed, mouth) {
  const line = wiggle(P2(pts).map(p => [p[0], p[1], 0]), 700, seed * 31 + 7, 96);
  const T = SPL.tessellate(line.map(p => [p[0], p[1]]), {step: 48}), L = T.length;
  const xs = T.pts.map(p => p[0]), zs = T.pts.map(p => p[1]), reach = V(Math.max(hw0, hw1)) * 2.2;
  const sg = postBox([Math.min(...xs) - reach, Math.min(...zs) - reach, Math.max(...xs) + reach, Math.max(...zs) + reach]);
  const Cc = TG.curveCoords(sg, T.pts, reach, 0);
  for (let n = 0; n < sg.h.length; n++) {
    const d = Cc.d[n];
    if (!isFinite(d)) continue;
    const s = Cc.s[n], t = s / L;
    const hw = V(lerp(hw0, hw1, t)) * (1 + 0.28 * TG.noise1(s / 2600, seed + 1) + 0.12 * TG.noise1(s / 900, seed + 2));
    const u = Math.sqrt(d * d + (Cc.beyond[n] * 1.5) ** 2) / hw;
    if (u >= 2.1) continue;
    // the floor: deeper seaward, a sill at the mouth (for arms: level with the trunk)
    const dep = depth * (0.35 + 0.65 * sstep(0, 0.6, t)) * (mouth ? 1 - 0.55 * sstep(0.88, 1, t) : 1);
    const floor = SEA - dep * VPM;
    const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
    const g = sg.h[n];
    // U profile: flat floor to u 0.4, the wall to u 1 (near-vertical with a little
    // noise in it), a shoulder above that eases into the ground by u 2.1
    const wallN = 0.08 * TG.fbm(x / 900, z / 900, seed + 5, 3);
    let k;
    if (u < 1) k = Math.pow(sstep(0.4 + wallN, 1.02, u), 2.2) * 0.8;
    else k = 0.8 + 0.2 * sstep(1, 2.1, u);
    let target = floor + (g - floor) * k;
    const sd = coastSD(x, z);
    if (sd < 0) target = Math.max(target, seaFloor(sd) - 8 * VPM * Math.exp(sd / 3000));
    if (target < g) { sg.h[n] = target; FJ_CUT++; }
  }
  TG.pasteGrid(P, sg);
}
function carveFjords() {
  for (const F of FJORDS) {
    // the glacial trough the fjord continues: a U valley whose floor falls to the fjord head
    if (F.valley) {
      const v = F.valley, sg = postBox(kmBox([Math.min(...v.map(p => p[0])) - 0.8, Math.min(...v.map(p => p[1])) - 0.8, Math.max(...v.map(p => p[0])) + 0.8, Math.max(...v.map(p => p[1])) + 0.8]));
      carveValley(sg, {id: F.id + '_trough', hw: 0.36, core: 0.3, keep: 0.05, wig: 90, pts: v});
      TG.pasteGrid(P, sg);
    }
    // hanging valleys first (the trunk then cuts their mouths off into a wall)
    F.hanging.forEach(([a, b, fm], k) => {
      const sg = postBox(kmBox([Math.min(a[0], b[0]) - 0.4, Math.min(a[1], b[1]) - 0.4, Math.max(a[0], b[0]) + 0.4, Math.max(a[1], b[1]) + 0.4]));
      carveValley(sg, {id: F.id + '_hv' + k, hw: 0.16, core: 0.35, keep: 0.06, wig: 60,
        pts: [[a[0], a[1], fm + 90], [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, fm + 35], [b[0], b[1], fm]]});
      TG.pasteGrid(P, sg);
    });
    fjordCut(F.pts, F.hw[0], F.hw[1], F.depth, F.seed, true);
    F.arms.forEach((A, k) => fjordCut(A.pts, A.hw[0], A.hw[1], F.depth * 0.7, F.seed * 7 + k, false));
  }
}
// MIRRORMERE: the lake, its level, the Merewall moraine that holds it.
const MERE = {x: 0.6, z: -1.65, a: 1.55, b: 0.68, rot: -12, level: 45, depth: 28, seed: 71, notch: [1.5, -1.12]};
/** Mirrormere's normalised radius at (x, z) (< 1 inside its warped ellipse)
 *  and the rotated minor-axis coordinate (+ = the south shore). */
function mereR(x, z) {
  const c = Math.cos(MERE.rot * Math.PI / 180), s = Math.sin(MERE.rot * Math.PI / 180);
  const wx = 3200 * TG.fbm(x / 7000, z / 7000, MERE.seed, 3), wz = 2600 * TG.fbm(x / 7000 + 3.3, z / 7000 + 1.1, MERE.seed + 2, 3);
  const dx = x + wx - V(MERE.x), dz = z + wz - V(MERE.z);
  const u = (dx * c + dz * s) / V(MERE.a), v = (-dx * s + dz * c) / V(MERE.b);
  return {r: Math.hypot(u, v), u, v};
}
function carveMere(G) {
  const lvl = Y(MERE.level), nx = V(MERE.notch[0]), nz = V(MERE.notch[1]);
  for (let n = 0; n < G.h.length; n++) {
    const x = cx(n), z = cz(n);
    if (Math.abs(x - V(MERE.x)) > 32000 || Math.abs(z - V(MERE.z)) > 22000) continue;
    const {r, v} = mereR(x, z);
    if (r < 1) {
      // the basin: a glacial trough's floor, deeper toward the north (the ice came from there)
      const bowl = Math.pow(Math.max(0, 1 - r * r), 0.5) * (1.15 - 0.3 * clamp(v, -1, 1));
      G.h[n] = Math.min(G.h[n], lvl - 12 - MERE.depth * VPM * bowl);
    } else if (r < 2.2) {
      // the shore: dry land at least 1.5 m above the level right round; on the south
      // side the MEREWALL, a hummocky moraine ridge (broken at the notch)
      const south = sstep(-0.1, 0.7, v / Math.max(r, 1e-3));
      const hum = 1 + 0.35 * TG.fbm(x / 2600, z / 2600, MERE.seed + 5, 3);
      const wall = lvl + 15 + south * 170 * hum * Math.exp(-(((r - 1.35) / 0.3) ** 2));
      const k = sstep(500, 1800, Math.hypot(x - nx, z - nz));
      const target = lerp(lvl + 15, wall, k);
      const w = 1 - sstep(1.25 + 0.6 * south, 2.2, r);
      if (G.h[n] < target) G.h[n] = lerp(G.h[n], target, w);
    }
  }
}
const FLATS = [
  {id: 'glimmerfen', poly: [[-4.0, 2.6], [-1.6, 2.5], [-1.4, 5.5], [-2.4, 6.0], [-4.2, 5.7]], to: 11, keep: 0.15, r: 12, both: true, warp: [5000, 3500]},
  {id: 'delta', poly: [[1.0, 5.6], [3.6, 5.8], [3.6, 6.9], [1.0, 6.9]], to: 4.5, keep: 0.15, r: 10},
  {id: 'eldmouth', poly: [[-8.6, 0.4], [-7.2, 0.5], [-7.2, 1.6], [-8.6, 1.7]], to: 4, keep: 0.2, r: 8},
  {id: 'saltmarsh', poly: [[-9.2, -2.8], [-8.6, -2.8], [-8.6, 0.4], [-9.2, 0.4]], to: 3.5, keep: 0.2, r: 6},
  // Rimeholt's shore at the head of its fjord
  {id: 'rimeholt', poly: [[-2.2, -7.25], [-1.45, -7.25], [-1.4, -7.7], [-2.25, -7.7]], to: 4, keep: 0.15, r: 3, both: true},
  {id: 'teethsound', poly: [[-5.0, 5.9], [-4.0, 5.9], [-4.0, 6.8], [-5.0, 6.8]], to: 3, keep: 0.2, r: 6},
  {id: 'hearthfloor', poly: [[-0.4, -0.3], [1.9, -0.3], [2.3, 5.2], [0.2, 5.0], [-0.9, 2.2]], to: 12.5, keep: 0.5, r: 16, both: true},
  // Harrowby's own plain: the vale eased down to the village's 8.8 m, so it sits on the land, not in a bowl
  {id: 'harrowbyplain', poly: [[-0.9, -0.3], [1.0, -0.4], [1.4, 0.6], [1.0, 1.3], [-0.4, 1.4], [-1.1, 0.6]], to: 9.6, keep: 0.4, r: 14, both: true},
];
function flattenLowlands(G) {
  for (const F of FLATS) {
    const m = WL.softMask(G, P2(F.poly), {warpAmp: F.warp ? F.warp[0] : 3000, warpL: F.warp ? F.warp[1] : 12000, seed: F.to * 11 + 3, r: F.r});
    const to = Y(F.to);
    for (let n = 0; n < G.h.length; n++) {
      if (!(m[n] > 0) || G.h[n] < SEA) continue;
      const target = to + (G.h[n] - to) * F.keep;
      if (G.h[n] > target || F.both) G.h[n] = lerp(G.h[n], target, m[n]);
    }
  }
}

/* ===================================================================================
 * 7. THE POSTS (6.4 m)
 * =================================================================================== */
const P = TG.makeGrid(NP, NP, MIN, MIN, PCELL);
const postBox = b => TG.subGrid(P, b);
/** The ground the river bake samples, as the plane gives it (the engine adds its noise). */
const groundAt = (x, z) => WL.bil(P, P.h, x, z);

const px = n => MIN + (n % NP) * PCELL, pz = n => MIN + ((n / NP) | 0) * PCELL;
/** Visit every post in world box b (voxels): fn(n, x, z). */
function eachPost(b, fn) {
  const i0 = Math.max(0, Math.floor((b[0] - MIN) / PCELL)), i1 = Math.min(NP - 1, Math.ceil((b[2] - MIN) / PCELL));
  const j0 = Math.max(0, Math.floor((b[1] - MIN) / PCELL)), j1 = Math.min(NP - 1, Math.ceil((b[3] - MIN) / PCELL));
  for (let j = j0; j <= j1; j++) for (let i = i0; i <= i1; i++) fn(j * NP + i, MIN + i * PCELL, MIN + j * PCELL);
}
/** Bicubic of a design-grid field at a world column. */
const up = (f, x, z) => HY.bicubicAt(f, NC, NC, (x - MIN) / CC, (z - MIN) / CC);
function upsampleToPosts() {
  // local relief on the design grid (how far a cell stands over the low ground ~230 m round it)
  const neg = new Float32Array(C.h.length);
  for (let n = 0; n < neg.length; n++) neg[n] = -C.h[n];
  const lo = TG.maxFilter(neg, C.w, C.hh, 5), rel = new Float32Array(C.h.length);
  for (let n = 0; n < rel.length; n++) rel[n] = Math.max(0, C.h[n] + lo[n]);
  const relB = TG.blur(rel, C.w, C.hh, 3, 2);
  for (let n = 0; n < P.h.length; n++) {
    const x = px(n), z = pz(n);
    let v = up(C.h, x, z);
    if (v > SEA + 20) {
      const r = WL.bil(C, relB, x, z);
      if (r > 30) v += TG.reliefDetail(x, z, Math.min(r, 9000), 977, 0.9) * sstep(SEA + 20, SEA + 200, v);
    }
    P.h[n] = v;
  }
}

/* ---- tablelands: Heatherhigh's moor top and the steppe keep the ground from
 * before the erosion (smooth, high), except where the erosion cut deeper than
 * cut[0] m (the valleys that dissect them). Evaluated on the posts, so the
 * edge is a smooth curve. ---- */
const TABLELANDS = [
  {id: 'heatherhigh', r: 20, lo: 0.45, hi: 0.75, jitter: 0.2, cut: [35, 90]},
  {id: 'steppe', r: 20, lo: 0.45, hi: 0.75, jitter: 0.2, cut: [15, 45]},
];
function tablelands() {
  for (const T of TABLELANDS) {
    const Z = ZONES.find(z => z.id === T.id);
    const m = TG.blur(zoneMask(T.id, 0), C.w, C.hh, T.r, 3);
    let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
    for (const p of P2(Z.poly)) { x0 = Math.min(x0, p[0]); z0 = Math.min(z0, p[1]); x1 = Math.max(x1, p[0]); z1 = Math.max(z1, p[1]); }
    eachPost([x0 - 8000, z0 - 8000, x1 + 8000, z1 + 8000], (n, x, z) => {
      const mv = WL.bil(C, m, x, z);
      if (!(mv > T.lo - T.jitter)) return;
      const j = T.jitter * (0.65 * TG.fbm(x / 16000, z / 16000, Z.base + 17, 4) + 0.35 * TG.fbm(x / 4500, z / 4500, Z.base + 19, 3));
      const w = sstep(T.lo, T.hi, mv + j);
      if (w <= 0) return;
      const pre = up(PRE, x, z), er = up(ERODED, x, z);
      const keepCut = sstep(T.cut[0] * VPM, T.cut[1] * VPM, pre - er);
      P.h[n] = lerp(P.h[n], Math.max(P.h[n], pre), w * (1 - keepCut));
    });
  }
}

/* ---- CAPROCK: a field of tables, mesas and buttes from a thresholded noise:
 * where e = noise - thr(x) > 0 the ground stands up `rise` m (a cliff ~cw wide
 * in e), a second tier where e > e2; a talus apron below each cliff. thr runs
 * from thrW (west) to thrE (east): solid table in the west breaking into
 * scattered buttes in the east. ---- */
const CAPROCKS = [
  {id: 'redreach', box: [4.2, -2.4, 9.6, 5.8], L: 1.1, thrW: -0.05, thrE: 0.32, xW: 5.6, xE: 8.3, tiers: [[0, 150], [0.14, 110]],
   cw: 0.012, steps: 3, seed: 101, coastMin: 250},
  {id: 'shardspire', box: [3.0, -6.4, 8.8, -1.4], L: 0.9, thrW: 0.02, thrE: 0.12, xW: 4.5, xE: 7.5, tiers: [[0, 120], [0.16, 70]],
   cw: 0.03, steps: 3, seed: 113, coastMin: 300},
];
function caprock() {
  for (const K of CAPROCKS) {
    const m = ZONE_MASK[K.id];
    const L = V(K.L), xW = V(K.xW), xE = V(K.xE);
    eachPost(kmBox(K.box), (n, x, z) => {
      const zw = WL.bil(C, m, x, z);
      if (zw < 0.05) return;
      const sd = coastSD(x, z);
      if (sd < K.coastMin * VPM * 0.3) return;
      const [qx, qz] = TG.warp(x / L, z / L, K.seed, 0.6, 4);
      const nz = TG.fbm(qx, qz, K.seed + 7, 5) + 0.12 * TG.fbm(x / (L * 0.25), z / (L * 0.25), K.seed + 9, 3);
      const e = nz - lerp(K.thrW, K.thrE, sstep(xW, xE, x));
      let rise = 0;
      for (const [e0, r] of K.tiers) {
        // the cliff, banded (steps) and a talus apron beneath it
        let c = sstep(e0, e0 + K.cw, e);
        if (K.steps > 1) c = clamp(TG.terraceH(c, 0, 1 / K.steps, 3), 0, 1);
        const apron = 0.22 * Math.pow(sstep(e0 - 0.09, e0, e), 2);
        rise += r * VPM * Math.max(c, apron);
      }
      // the field fades out at the zone's and the sea's edge
      P.h[n] += rise * sstep(0.05, 0.5, zw) * sstep(K.coastMin * VPM * 0.3, K.coastMin * VPM, sd);
    });
  }
}

/** A channel floor (monotone, by arc s) made a STAIRCASE where it is steep: pools
 *  and vertical falls every 6 m of drop instead of a ramp. The engine's river
 *  bake then lays a few reaches with real waterfalls (each step a weir) instead
 *  of a reach every half metre -- the waterfalls the brief asks for, and far
 *  fewer river pieces in a site cell (kSiteCellMax). Gentle stretches keep their
 *  floor. */
function stairFloor(floor, s) {
  const N = floor.length, H = 60, out = Float32Array.from(floor);
  for (let i = 0; i < N; i++) {
    if (!isFinite(floor[i])) continue;
    let a = i, b = i;
    while (a > 0 && s[i] - s[a - 1] < 200 && isFinite(floor[a - 1])) a--;
    while (b + 1 < N && s[b + 1] - s[i] < 200 && isFinite(floor[b + 1])) b++;
    const grade = b > a ? (floor[a] - floor[b]) / Math.max(1, s[b] - s[a]) : 0;
    const k = sstep(0.06, 0.12, grade);
    if (k <= 0) continue;
    const Hs = Math.max(H, grade * 350), st = SEA + Hs * Math.floor((floor[i] - SEA) / Hs);
    out[i] = lerp(floor[i], Math.min(floor[i], st), k);
  }
  // still monotone (a blend of two non-increasing profiles can wobble by a voxel)
  let run = Infinity;
  for (let i = 0; i < N; i++) { if (!isFinite(out[i])) continue; run = Math.min(run, out[i]); floor[i] = run; }
}
/* ---- INCISED rivers: where the story's river must take a course the eroded
 * land does not (the Amberbrook round Harrowby to the Silverrun; the Redrun
 * from the Greyspine through the Shardspire tableland and Redreach's mesas),
 * the course is drawn and the land made to obey it: the ground along the
 * spline, smoothed, its RUNNING MINIMUM less `cut` m is the channel floor
 * (never uphill, by construction), and a valley (or, from `slotFrom` of the
 * way down, a terraced slot canyon) is cut to it. Their rivers[] records are
 * these courses (step 8). ---- */
const INCISED = [
  // the Glimmer: the slow fen river, looping across Glimmerfen to the Teeth Sound
  {id: 'glimmer', river: 'river_glimmer', course: [[-2.0, 3.7], [-2.45, 4.25], [-2.5, 4.9], [-3.25, 5.15], [-3.5, 5.75], [-4.15, 6.05], [-4.45, 6.55], [-4.75, 6.95]],
   cut: 1.2, hw: 0.16, core: 0.4, keep: 0.1, w: [4, 12], d: [0.6, 1.2], wig: 80},
  // the Goldwater: off the Goldsea plain, down its own vale to the east shore of Gull Bay
  {id: 'goldwater', river: 'river_goldwater', course: [[4.5, 3.2], [4.1, 4.0], [3.75, 4.8], [3.5, 5.6], [3.35, 6.3], [3.25, 7.1]],
   cut: 1.8, hw: 0.22, core: 0.3, keep: 0.12, w: [3, 14], d: [0.5, 1.4], wig: 220},
  // the Chalkbourne: a downland stream cut to the cliff edge, where it falls into the sea
  {id: 'chalkbourne', river: 'river_chalkbourne', course: [[-1.55, 6.2], [-1.75, 6.9], [-1.9, 7.6], [-2.0, 8.25], [-2.08, 9.05]],
   cut: 2, hw: 0.18, core: 0.3, keep: 0.15, w: [2.5, 7], d: [0.5, 0.9], wig: 90},
  {id: 'amberbrook', river: 'river_amberbrook', course: [[-2.75, -1.9], [-2.45, -1.3], [-2.05, -0.65], [-1.6, 0.0], [-1.2, 0.55], [-0.75, 1.1],
    [-0.25, 1.5], [0.3, 1.9], [0.72, 2.28], [0.98, 2.5]], cut: 1.5, hw: 0.2, core: 0.3, keep: 0.12, w: [4, 14], d: [0.8, 1.6], minM: 9.5, wig: 140},
  {id: 'redrun', river: 'river_redrun', course: [[5.65, -5.85], [5.35, -5.15], [5.6, -4.55], [5.2, -3.95], [5.4, -3.35], [4.95, -2.75], [5.25, -2.1], [5.0, -1.5],
    [5.3, -0.85], [5.25, -0.3], [5.5, 0.1], [5.95, 0.45], [6.5, 0.7], [7.0, 0.95], [7.45, 0.95], [7.85, 1.05], [8.2, 1.1]],
   cut: 2.5, hw: 0.3, core: 0.3, keep: 0.1, slotFrom: 0.14, slotFloor: 0.028, meander: [190, 0.95, 0.12], fords: [[5.02, -1.62], [5.12, -2.95]], w: [3, 16], d: [0.6, 1.8], seed: 23, wig: 160},
];
const INCISED_LINE = {};
/** Red-rock STRATA: absolute-height bands, each 6..30 m thick, hard (a cliff:
 *  0.1 m back per m up) or soft (a bench / talus slope: 1.6..3.4 m back per m
 *  up). Shared by every canyon wall, so the ledges line up across a canyon and
 *  from canyon to canyon, as real beds do. */
const STRATA = (() => {
  const R = TG.rng(777), B = [], rate = [];
  let y = SEA - 200 * VPM;
  while (y < SEA + 1600 * VPM) {
    B.push(y);
    const hard = R() < 0.55;
    const t = (hard ? 8 + 22 * R() : 5 + 12 * R()) * VPM;
    rate.push(hard ? 0.06 + 0.08 * R() : 1.6 + 1.8 * R());
    y += t;
  }
  B.push(y);
  return {B, rate};
})();
/** The height of a strata wall `e` voxels out from the foot of a canyon whose floor is `fl`. */
function strataWall(fl, e) {
  if (e <= 0) return fl;
  const {B, rate} = STRATA;
  let lo = 0, hi = B.length - 1;
  while (hi - lo > 1) { const m = (lo + hi) >> 1; if (B[m] <= fl) lo = m; else hi = m; }
  let y = fl;
  for (let k = lo; k < rate.length; k++) {
    const dw = (B[k + 1] - y) * rate[k];
    if (dw >= e) return y + e / rate[k];
    e -= dw; y = B[k + 1];
  }
  return y;
}
/** A line given entrenched MEANDERS: a sine of `amp` m and wavelength `lam`
 *  km whose phase and amplitude wander (noise), fading in from `from` of the
 *  way down and out at the mouth. */
function meanderLine(pts, [amp, lam, from], seed) {
  const T = SPL.tessellate(pts.map(p => [p[0], p[1]]), {step: 64}), N = T.pts.length, L = T.length, out = [];
  const W = V(lam);
  for (let i = 0; i < N; i++) {
    const s = T.s[i], a = T.pts[Math.max(0, i - 1)], b = T.pts[Math.min(N - 1, i + 1)];
    let tx = b[0] - a[0], tz = b[1] - a[1];
    const tl = Math.hypot(tx, tz) || 1; tx /= tl; tz /= tl;
    const on = sstep(from * L - 1500, from * L + 1500, s) * Math.min(1, (L - s) / 1500, s / 1500);
    const ph = 2 * Math.PI * s / W + 2.2 * TG.noise1(s / (W * 2.3), seed + 1);
    const A = amp * VPM * (0.65 + 0.45 * TG.noise1(s / (W * 3.1), seed + 2)) * on;
    const o = A * Math.sin(ph);
    out.push([T.pts[i][0] - tz * o, T.pts[i][1] + tx * o, 0]);
  }
  return out;
}
function incise(R) {
  let base = wiggle(P2(R.course).map(p => [p[0], p[1], 0]), (R.wig || 0) * VPM, R.id.length * 41 + 9, 256);
  if (R.meander) base = meanderLine(base, R.meander, R.seed || 1);
  const T = SPL.tessellate(base.map(p => [p[0], p[1]]), {step: 32});
  const line = T.pts.map(p => [p[0], p[1]]), N = line.length, L = T.length;
  const g = TG.blur(Float32Array.from(line.map(p => groundAt(p[0], p[1]))), N, 1, 3, 2);
  const floor = new Float32Array(N);
  let run = Infinity;
  for (let i = 0; i < N; i++) { run = Math.min(run, g[i] - R.cut * VPM); floor[i] = R.minM != null ? Math.max(Y(R.minM), run) : run; }
  stairFloor(floor, T.s);
  const at = s => { let lo = 0, hi = N - 1; while (hi - lo > 1) { const m = (lo + hi) >> 1; if (T.s[m] <= s) lo = m; else hi = m; } return floor[lo]; };
  const hwMax = Math.max(V(R.hw), R.slotFrom != null ? V(0.75) : 0) * 1.1, xs = line.map(p => p[0]), zs = line.map(p => p[1]);
  const sg = postBox([Math.min(...xs) - hwMax, Math.min(...zs) - hwMax, Math.max(...xs) + hwMax, Math.max(...zs) + hwMax]);
  const Cc = TG.curveCoords(sg, line, hwMax, 0), slot0 = R.slotFrom == null ? Infinity : R.slotFrom * L;
  for (let n = 0; n < sg.h.length; n++) {
    const d = Cc.d[n];
    if (!isFinite(d)) continue;
    const s = Cc.s[n], ad = Math.sqrt(d * d + Cc.beyond[n] * Cc.beyond[n]), fl = at(s);
    if (sg.h[n] <= fl) continue;
    // the valley above the slot, its walls easing to the slot's below
    const vs = isFinite(slot0) ? sstep(slot0 - 2000, slot0, s) : 0;
    const hwV = V(R.hw);
    const uV = ad / hwV;
    let hv = sg.h[n];
    // (the valley is cut only for a canyon river: the others' courses are drawn on the land as it
    // is and the engine's river bake cuts their channels -- which is how the map was first built,
    // and the Silverrun's traced course through the vale depends on it)
    if (uV < 1 && isFinite(slot0)) { const target = fl + (sg.h[n] - fl) * R.keep * sstep(0, 0.5, uV); hv = lerp(sg.h[n], target, (1 - sstep(R.core, 1, uV)) * (1 - vs)); }
    if (s > slot0 - 2000) {
      // the canyon: a floor W0 wide, then walls built from the STRATA (hard bands
      // stand as cliffs, soft bands weather back into benches and talus), side
      // canyons where a ridged field across the wall widens it in tongues
      const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
      const W0 = V(R.slotFloor) * (1 + 0.4 * TG.noise1(s / 1300, R.seed + 3));
      const gr = TG.ridged(s / 1100, (d < 0 ? 37 : 0) + ad / 1400, R.seed + 5, 3);
      const side = 1 + 1.6 * sstep(0.5, 0.92, gr) * sstep(0.2, 1.5, ad / Math.max(1, W0));
      let e = (ad / side - W0) * (1 + 0.18 * TG.fbm(x / 700, z / 700, R.seed + 7, 3));
      // a FORD: the walls break down into a broad ramp of talus a road can take
      if (R.fords) { let fd = Infinity; for (const f of R.fords) fd = Math.min(fd, Math.hypot(x - V(f[0]), z - V(f[1]))); e /= 1 + 2.5 * (1 - sstep(1500, 4500, fd)); }
      const wall = strataWall(fl, e);
      if (wall < hv) hv = lerp(hv, wall, sstep(slot0 - 2000, slot0, s));
    }
    sg.h[n] = Math.min(sg.h[n], hv);
  }
  TG.pasteGrid(P, sg);
  INCISED_LINE[R.river] = {line, s: T.s, floor, L};
}

/* ---- the Shardspire: a sheer needle and its crystal satellites; the plateau's crater ---- */
const SPIRES = [
  {x: 6.0, z: -3.8, r: 75, h: 300, main: true},
  {x: 6.17, z: -3.72, r: 22, h: 70}, {x: 5.88, z: -3.95, r: 18, h: 55}, {x: 6.08, z: -4.02, r: 26, h: 90},
  {x: 5.6, z: -3.5, r: 20, h: 45}, {x: 6.5, z: -3.3, r: 16, h: 40}, {x: 6.35, z: -4.3, r: 18, h: 60},
];
const CRATER = {x: 6.75, z: -4.55, radius: 0.27, depth: 34, rim: 12, seed: 33};
function spires() {
  for (const S of SPIRES) {
    const X = V(S.x), Z = V(S.z), R = S.r * VPM, base = WL.bil(P, P.h, X, Z);
    eachPost([X - R * 1.6, Z - R * 1.6, X + R * 1.6, Z + R * 1.6], (n, x, z) => {
      const [wx, wz] = TG.warp((x - X) / R, (z - Z) / R, 7 + S.r, 0.18, 2);
      const q = Math.hypot(wx, wz);
      if (q >= 1.6) return;
      const f = q < 1 ? Math.pow(1 - q, S.main ? 0.35 : 0.6) : 0;
      const v = base + S.h * VPM * Math.max(f, 0.08 * (1 - sstep(0.8, 1.6, q)));
      if (v > P.h[n]) P.h[n] = v;
    });
  }
  const sg = postBox(kmBox([CRATER.x - 1, CRATER.z - 1, CRATER.x + 1, CRATER.z + 1]));
  const before = Float32Array.from(sg.h);
  CRATER.g0 = WL.bil(P, P.h, V(CRATER.x), V(CRATER.z));
  TG.crater(sg, {x: V(CRATER.x), z: V(CRATER.z), radius: V(CRATER.radius), depth: CRATER.depth * VPM, rim: CRATER.rim * VPM, peak: 0, seed: CRATER.seed});
  warpRelief(sg, before, V(CRATER.x), V(CRATER.z), 0, V(CRATER.radius) * 2.4, 900, 3000, 81);
  TG.pasteGrid(P, sg);
}

/** A landform's relief (sg.h - before) re-read at a domain-warped position:
 *  a circular crater or cone becomes a lopsided natural one. amp voxels at
 *  wavelength L, fading in from r0 to r1 around (X, Z). */
function warpRelief(sg, before, X, Z, r0, r1, amp, L, seed) {
  const rel = new Float32Array(sg.h.length);
  for (let n = 0; n < rel.length; n++) rel[n] = sg.h[n] - before[n];
  for (let n = 0; n < rel.length; n++) {
    const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
    const wk = 0.35 + 0.65 * sstep(r0, Math.max(r0 + 1, r1 * 0.5), Math.hypot(x - X, z - Z));
    const wx = wk * amp * TG.fbm(x / L, z / L, seed, 3), wz = wk * amp * TG.fbm(x / L + 3.1, z / L + 1.7, seed + 2, 3);
    sg.h[n] = before[n] + WL.bil(sg, rel, x + wx, z + wz);
  }
}
/* ---- the white cliffs (and the Teeth's): the land within a cliff's width of
 * the waterline goes straight up to the ground behind it ---- */
const CLIFF_COASTS = [
  {box: [-4.0, 7.4, 0.8, 9.6], top: 30, width: 14, seed: 3},
  {box: [-9.6, 5.8, -5.0, 9.6], top: 12, width: 10, seed: 5},
];
function cliffs() {
  for (const K of CLIFF_COASTS)
    eachPost(kmBox(K.box), (n, x, z) => {
      const sd = coastSD(x, z);
      if (sd < -1500 || sd > 1500) return;
      const gx = coastSD(x + 128, z) - coastSD(x - 128, z), gz = coastSD(x, z + 128) - coastSD(x, z - 128);
      const gl = Math.hypot(gx, gz) || 1, reach = Math.max(0, 1200 - sd);
      const top = WL.bil(C, C.h, x + gx / gl * reach, z + gz / gl * reach);
      if (top < Y(K.top)) return;
      const s2 = sd + 6 * VPM * TG.fbm(x / 900, z / 900, K.seed, 3);
      // faded in from the box's edge (a hard edge left a straight wall inland)
      const [b0, b1, b2, b3] = kmBox(K.box), fade = sstep(0, 6000, Math.min(x - b0, b2 - x, z - b1, b3 - z));
      if (s2 > 0) P.h[n] = lerp(P.h[n], Math.max(P.h[n], SEA + 6 + (top - SEA - 6) * Math.pow(sstep(0, K.width * VPM, s2), 0.7) * (1 - 0.15 * sstep(500, 1500, s2))), fade);
      else P.h[n] = lerp(P.h[n], Math.min(P.h[n], SEA - 20 - 60 * sstep(0, 600, -s2)), fade);
    });
}

/* ---- karst towers in clusters (fengcong) and sea stacks (the Verdant Teeth) ---- */
const KARST = {box: [-9.7, 5.3, -4.4, 9.7], spacing: 0.15, seed: 91};
let KARST_N = 0;
function distToLine(line, x, z) {
  let b = Infinity;
  for (let k = 1; k < line.length; k++) {
    const a = line[k - 1], c = line[k], ex = c[0] - a[0], ez = c[1] - a[1], L2 = ex * ex + ez * ez;
    const t = clamp(L2 > 0 ? ((x - a[0]) * ex + (z - a[1]) * ez) / L2 : 0, 0, 1);
    b = Math.min(b, Math.hypot(x - a[0] - ex * t, z - a[1] - ez * t));
  }
  return b;
}
function karstTowers() {
  const R = TG.rng(KARST.seed), sp = V(KARST.spacing);
  const [bx0, bz0, bx1, bz1] = kmBox(KARST.box);
  const towers = [];
  for (let gz = bz0; gz < bz1; gz += sp)
    for (let gx = bx0; gx < bx1; gx += sp) {
      const x = gx + sp * (0.1 + 0.8 * R()), z = gz + sp * (0.1 + 0.8 * R());
      const keep = R(), hr = R(), rr = R(), ar = R(), an = R(), ty = R();
      const sd = coastSD(x, z);
      if (sd < -1500) continue;
      if (!(WL.bil(C, ZONE_MASK.teeth, x, z) > 0.35 || sd < 0)) continue;
      // the clusters: dense where the cluster field is high, open jungle plain between
      const cl = sstep(-0.05, 0.3, TG.fbm(x / 16000, z / 16000, KARST.seed + 3, 4) + 0.15 * TG.fbm(x / 5000, z / 5000, KARST.seed + 5, 3));
      if (keep > (sd < 0 ? 0.1 : 0.02 + 0.85 * cl * cl)) continue;
      if (distToLine(VALLEY_LINES.teethwater, x, z) < 1500) continue;
      // sizes spread wide (a few giants, many small), the tallest in the cluster cores
      const H = (sd < 0 ? 22 + 70 * Math.pow(hr, 1.6) : (30 + 280 * Math.pow(hr, 2.4)) * (0.35 + 0.85 * cl)) * VPM;
      const rad = (sd < 0 ? 16 + 40 * Math.pow(rr, 1.5) : 22 + 120 * Math.pow(rr, 2)) * VPM * (0.7 + 0.5 * Math.sqrt(H / (300 * VPM)));
      // three forms: the tower (sheer, a domed top), the cone (a fenglin peak), the ridge (long, sawtoothed)
      const form = ty < 0.5 ? 'tower' : ty < 0.82 ? 'cone' : 'ridge';
      towers.push({x, z, H, rad, form, asp: form === 'ridge' ? 1.8 + 1.6 * ar : 1 + 0.7 * ar, ang: an * Math.PI, sd: KARST.seed + towers.length * 13});
    }
  // the fengcong: in the cluster cores the towers stand on a shared rocky base
  eachPost(kmBox(KARST.box), (n, x, z) => {
    if (coastSD(x, z) < 300 || !(WL.bil(C, ZONE_MASK.teeth, x, z) > 0.3)) return;
    const cl = sstep(-0.05, 0.3, TG.fbm(x / 16000, z / 16000, KARST.seed + 3, 4) + 0.15 * TG.fbm(x / 5000, z / 5000, KARST.seed + 5, 3));
    const ped = 40 * VPM * sstep(0.45, 0.95, cl) * (0.5 + 0.5 * TG.ridged(x / 1800, z / 1800, KARST.seed + 9, 3)) * sstep(300, 1200, coastSD(x, z));
    if (ped > 0) P.h[n] += ped * sstep(600, 1500, distToLine(VALLEY_LINES.teethwater, x, z));
  });
  const P0 = Float32Array.from(P.h);       // the ground under the towers (overlaps take the max, never stack)
  for (const t of towers) {
    const reach = t.rad * t.asp * 1.5, base = Math.max(SEA - 40, WL.bil(P, P0, t.x, t.z));
    const ca = Math.cos(t.ang), sa = Math.sin(t.ang);
    eachPost([t.x - reach, t.z - reach, t.x + reach, t.z + reach], (n, x, z) => {
      const dx = x - t.x, dz = z - t.z;
      const u = (dx * ca + dz * sa) / (t.rad * t.asp), v = (-dx * sa + dz * ca) / t.rad;
      const [wx, wz] = TG.warp(u, v, t.sd, 0.35, 3);
      const q = Math.hypot(wx, wz);
      if (q >= 1.45) return;
      let f;
      if (t.form === 'tower') f = q < 0.7 ? 1 - 0.3 * (q / 0.7) ** 2 : q < 1 ? 0.7 * (1 - sstep(0.7, 1, q)) + 0.06 : 0.06 * (1 - sstep(1, 1.45, q));
      else if (t.form === 'cone') f = q < 1 ? Math.pow(1 - q, 0.75) * (1 - 0.06 * q) + 0.04 : 0.04 * (1 - sstep(1, 1.45, q));
      else f = q < 1 ? (1 - Math.pow(q, 2.5)) * (0.65 + 0.35 * TG.ridged1(u * 2.2, t.sd, 2)) : 0;
      // the tower stands on its own footing; its skirt follows the ground round it
      const foot = q < 1 ? Math.max(P0[n], lerp(base, P0[n], sstep(0.6, 1, q))) : P0[n];
      const val = foot + t.H * f * (0.85 + 0.15 * TG.ridged(x / 250, z / 250, t.sd, 2));
      if (val > P.h[n]) P.h[n] = val;
    });
  }
  KARST_N = towers.length;
}

/* ---- lava flows down Cinder's flanks: lobate tongues with steep edges ---- */
const LAVA_FLOWS = [
  {pts: [[6.9, 7.3], [7.5, 7.9], [8.0, 8.35], [8.45, 8.8]], hw: 0.13, thick: 18},
  {pts: [[6.0, 7.5], [5.6, 8.1], [5.3, 8.6]], hw: 0.11, thick: 14},
  {pts: [[6.9, 6.2], [7.6, 5.7], [8.2, 5.4], [8.75, 5.35]], hw: 0.14, thick: 20},
  {pts: [[5.9, 6.2], [5.3, 5.6], [4.95, 5.0]], hw: 0.12, thick: 16},
  {pts: [[6.4, 7.6], [6.5, 8.3], [6.6, 8.9]], hw: 0.1, thick: 12},
  {pts: [[7.1, 6.85], [7.8, 7.05], [8.55, 7.3]], hw: 0.08, thick: 9},
  {pts: [[5.75, 6.9], [5.1, 6.95], [4.75, 7.4]], hw: 0.09, thick: 11},
  {pts: [[7.6, 7.9], [7.6, 8.5], [7.9, 9.0]], hw: 0.07, thick: 8},
];
function lavaFlows() {
  LAVA_FLOWS.forEach((F, k) => {
    const line = wiggle(P2(F.pts).map(p => [p[0], p[1], 0]), 900, 300 + k);
    const hw = V(F.hw), xs = line.map(p => p[0]), zs = line.map(p => p[1]);
    const sg = postBox([Math.min(...xs) - hw * 2, Math.min(...zs) - hw * 2, Math.max(...xs) + hw * 2, Math.max(...zs) + hw * 2]);
    const Cc = TG.curveCoords(sg, line, hw * 1.6, 0), L = WL.polyLength(line);
    for (let n = 0; n < sg.h.length; n++) {
      const d = Cc.d[n];
      if (!isFinite(d)) continue;
      const s = Cc.s[n];
      const wl = hw * (1 + 0.35 * TG.noise1(s / 2400, 400 + k)) * (0.6 + 0.4 * sstep(0, L * 0.3, s)) * (1 + 0.5 * sstep(L * 0.75, L, s));
      const u = Math.sqrt(d * d + (Cc.beyond[n] * 1.6) ** 2) / wl;
      if (u >= 1) continue;
      const k2 = 1 - Math.pow(sstep(0.6, 1, u), 2);
      const ridge = 0.25 * (1 - Math.abs(d) / wl) * TG.ridged(s / 600, d / 300, 500 + k, 2);
      sg.h[n] += 1.8 * F.thick * VPM * k2 * (1 + ridge);
    }
    TG.pasteGrid(P, sg);
  });
}

/* ---- dunes: Saltreach on the west coast, the Sunscour erg (two crossing sets: star dunes) ---- */
const DUNEFIELDS = [
  {box: [-9.5, -3.2, -7.6, 5.1], sets: [[9, 0.16, 8]], seed: 5, maxM: 30, coastMin: 40, maxCoast: 650},
  {box: [7.4, -4.6, 9.3, -1.1], sets: [[13, 0.24, 0], [9, 0.18, 70]], seed: 9, maxM: 75, coastMin: 60},
];
function duneFields() {
  for (const D of DUNEFIELDS) {
    const b = kmBox(D.box);
    const sg = postBox(b), before = Float32Array.from(sg.h), best = new Float32Array(sg.h.length);
    D.sets.forEach(([H, wl, wind], k) => {
      const t = TG.makeGrid(sg.w, sg.hh, sg.x0, sg.z0, sg.cell);
      TG.dunes(t, {box: b, height: H * VPM, wavelength: V(wl), wind, seed: D.seed + k * 7});
      for (let n = 0; n < t.h.length; n++) best[n] = Math.max(best[n], t.h[n]);
    });
    for (let n = 0; n < sg.h.length; n++) {
      const x = sg.x0 + (n % sg.w) * sg.cell, z = sg.z0 + ((n / sg.w) | 0) * sg.cell;
      const sd = coastSD(x, z);
      // in patches (an erg's dunes come and go), off the beach, not up the hills
      const patch = sstep(-0.2, 0.25, TG.fbm(x / 9000, z / 9000, D.seed + 11, 3));
      let w = patch * sstep(D.coastMin * VPM, D.coastMin * VPM + 600, sd) * (1 - sstep(Y(D.maxM - 15), Y(D.maxM), before[n]));
      // a coastal field stays behind its beach (its inland edge wavers); every field fades out from its box's edge
      if (D.maxCoast) w *= 1 - sstep(D.maxCoast * VPM * (0.45 + 0.25 * TG.fbm(x / 2500, z / 2500, D.seed + 13, 3)), D.maxCoast * VPM, sd);
      w *= sstep(0, 4000, Math.min(x - b[0], b[2] - x, z - b[1], b[3] - z));
      sg.h[n] = before[n] + best[n] * w;
    }
    TG.pasteGrid(P, sg);
  }
}

/* ---- small basins: Heatherhigh's tarns, Glimmerfen's pools, the oasis ---- */
const TARNS = [
  [-7.0, -3.9, 0.15, 9], [-6.35, -3.25, 0.2, 11], [-5.75, -4.1, 0.13, 8], [-5.1, -3.45, 0.18, 10], [-4.4, -4.05, 0.12, 8],
  [-4.05, -2.95, 0.11, 7], [-6.6, -2.45, 0.14, 8], [-4.95, -2.55, 0.12, 8], [-5.6, -2.05, 0.17, 9],
];
const FENPOOLS = [];
{
  const R = TG.rng(57);
  for (let k = 0; k < 22; k++) FENPOOLS.push([-3.9 + 2.4 * R(), 2.7 + 2.9 * R(), 0.04 + 0.08 * R(), 2 + 1.5 * R()]);
}
const OASIS = [8.15, -2.0, 0.12, 8];
const TWINMERE = [1.0, 2.55, 0.2, 7];   // placed on the Silverrun's line at run time
const BASIN_LEVEL = {}, BASIN_R = {};
function basins(list) {
  for (const [kx, kz, kr, dm] of list) {
    const X = V(kx), Z = V(kz), R = V(kr);
    // the floor below the lowest ground round the rim, so the bowl holds water
    let rimLo = Infinity;
    for (let a = 0; a < 24; a++) rimLo = Math.min(rimLo, WL.bil(P, P.h, X + R * 1.15 * Math.cos(a * Math.PI / 12), Z + R * 1.15 * Math.sin(a * Math.PI / 12)));
    const floor = Math.max(SEA + 30, rimLo - dm * VPM), seed = Math.round(kx * 100) + 3, ang = (((seed % 31) + 31) % 31) / 31 * Math.PI;
    if (rimLo - floor >= 6) { BASIN_LEVEL[kx + ',' + kz] = Math.round(rimLo - 2); BASIN_R[kx + ',' + kz] = R; }
    eachPost([X - R * 2.2, Z - R * 2.2, X + R * 2.2, Z + R * 2.2], (n, x, z) => {
      const dx = (x - X) / R, dz = (z - Z) / R;
      const u = (dx * Math.cos(ang) + dz * Math.sin(ang)) / 1.4, v = (-dx * Math.sin(ang) + dz * Math.cos(ang)) * 1.15;
      const [wx, wz] = TG.warp(u, v, seed, 0.5, 3);
      const q = Math.hypot(wx, wz);
      if (q >= 1.8) return;
      if (q >= 1 && q < 1.5) {
        // a low sill so the water stays (the ground round a pit on a slope)
        const sill = rimLo + 0.6 * VPM * (1 - sstep(1, 1.5, q));
        if (P.h[n] < sill) P.h[n] = sill;
      }
      const target = q < 1 ? floor + (rimLo - floor) * Math.pow(q, 1.6) : P.h[n];
      if (target < P.h[n]) P.h[n] = target;
    });
  }
}

/* ---- Harrowby: the posts under the village as they were ---- */
const HB = {file: join(ROOT, 'scripts', 'design_world_harrowby.json'), win: [-2048, 1024, 3200, 5760],
            keep: [30, 2790, 1090, 3950], feather: 1600};
function harrowbySnapshot() {
  if (existsSync(HB.file)) return JSON.parse(readFileSync(HB.file, 'utf8'));
  // first run: read the posts from the plane on disk (the map as it was before this script)
  const J = JSON.parse(readFileSync(join(MAPDIR, 'map.json'), 'utf8'));
  const PL = MT.makePlane('height', J);
  for (const f of readdirSync(join(MAPDIR, 'planes', 'height'))) {
    const [tx, tz] = f.replace('.svt', '').split('_').map(Number);
    PL.tiles.set(MT.tileKey(tx, tz), MT.decodeTile(PL, readFileSync(join(MAPDIR, 'planes', 'height', f)), tx, tz));
  }
  const i0 = Math.floor((HB.win[0] - MIN) / PCELL), j0 = Math.floor((HB.win[1] - MIN) / PCELL);
  const i1 = Math.ceil((HB.win[2] - MIN) / PCELL), j1 = Math.ceil((HB.win[3] - MIN) / PCELL);
  const w = i1 - i0 + 1, h = j1 - j0 + 1, vals = [];
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) vals.push(Math.round(MT.sample(PL, i0 + i, j0 + j)));
  const snap = {about: 'The height-plane posts under Harrowby as they were before scripts/design_world.mjs first ran (2026-10-09); the script keeps them so the village gates hold.', i0, j0, w, h, vals};
  writeFileSync(HB.file, JSON.stringify(snap) + '\n');
  return snap;
}
function harrowbyGround() {
  const S = harrowbySnapshot();
  const [kx0, kz0, kx1, kz1] = HB.keep, F = HB.feather;
  for (let j = 0; j < S.h; j++)
    for (let i = 0; i < S.w; i++) {
      const pi = S.i0 + i, pj = S.j0 + j, x = MIN + pi * PCELL, z = MIN + pj * PCELL, n = pj * NP + pi;
      const d = Math.max(kx0 - x, x - kx1, kz0 - z, z - kz1, 0);
      const w = 1 - sstep(0, F, d);
      if (w > 0) P.h[n] = lerp(P.h[n], S.vals[j * S.w + i], w);
    }
}

/* ---- render helpers ---- */
function renderGrid(name, G, extra = {}) {
  const view = extra.view || {x0: MIN, z0: MIN, x1: MIN + SIZE, z1: MIN + SIZE, res: extra.res || 256};
  const R = WL.renderView(view, {heightAt: (x, z) => WL.bil(G, G.h, x, z), seaY: SEA, lo: SEA, hi: Y(2000), ...extra});
  const p = writePNG(join(PNG, name + '.png'), R.img, R.W, R.H);
  log(`-> ${p} (${R.W}x${R.H})`);
}
const ZOOMS = {
  harrowby: [-1.5, -3.5, 3.5, 1.5], hearth: [-1.0, -1.0, 3.5, 4.0], teeth: [-9.6, 5.2, -4.4, 9.6], redreach: [4.8, -1.5, 9.2, 4.6],
  shardspire: [3.8, -5.6, 8.0, -2.2], pass: [-3.5, -8.5, 0.5, -4.5], cinder: [4.4, 4.6, 9.2, 9.4], downs: [-4.2, 5.2, 1.2, 9.4],
  saltreach: [-9.6, -3.5, -6.5, 5.0], fjords: [-5.5, -9.6, 4.0, -5.5], heather: [-8.0, -5.0, -3.0, -1.0], sunscour: [6.8, -5.0, 9.4, -0.8],
};
function renderZooms(G, extra = {}) {
  for (const [k, b] of Object.entries(ZOOMS)) {
    const vb = kmBox(b), span = Math.max(vb[2] - vb[0], vb[3] - vb[1]);
    renderGrid('zoom_' + k, G, {view: {x0: vb[0], z0: vb[1], x1: vb[2], z1: vb[3], res: Math.max(16, Math.round(span / 900))}, contour: 100, ...extra});
  }
}

/* ===================================================================================
 * RUN: the land
 * =================================================================================== */
mkdirSync(PNG, {recursive: true});
log('1-2 coast and zones');
const SD = new Float32Array(C.h.length);
for (let n = 0; n < SD.length; n++) SD[n] = coastSD(cx(n), cz(n));
buildBase(C);
log('3 valleys');
for (const v of VALLEYS) if (!v.late) carveValley(C, v);
const PRE = Float32Array.from(C.h);
log('4 erosion');
C.h = HY.viaProxy(C.h, C.w, C.hh, 2, (h, w, hh) => {
  const G2 = {w, hh, x0: MIN + CC / 2, z0: MIN + CC / 2, cell: CC * 2, h};
  TG.erodeLand(G2, {strength: 0.45, seaY: SEA, seed: 7});
});
for (let n = 0; n < C.h.length; n++) if (SD[n] < 0) C.h[n] = PRE[n];     // the sea floor stays as designed
const ERODED = Float32Array.from(C.h);
liftZones(C);
smoothZones(C);
renderGrid('01_base', C);
log('5 ranges');
bakeRanges(C);
passAndSummit(C);
log('6 Cinder, troughs, fjords, Mirrormere, flats');
cinder(C);
carveTroughs(C);
carveMere(C);
flattenLowlands(C);
for (const v of VALLEYS) if (v.late === true) carveValley(C, v);
renderGrid('02_land', C);
log('7 posts');
upsampleToPosts();
tablelands();
caprock();
spires();
carveFjords();
{
  let nan = 0, lo = Infinity;
  for (let n = 0; n < P.h.length; n++) if (!isFinite(P.h[n])) nan++;
  eachPost(kmBox([-2.3, -8.8, -1.6, -8.0]), n => { lo = Math.min(lo, P.h[n]); });
  log(`fjords: ${FJORDS.length}, ${FJ_CUT} posts cut; Kings fjord floor ${((lo - SEA) / VPM).toFixed(0)} m; non-finite posts ${nan}`);
}
for (const v of VALLEYS) if (v.late === 'posts') carveValley(P, v);
// the Twinmere: the confluence pool, on the Silverrun's valley line where the Amberbrook comes in
{
  const ab = INCISED.find(r => r.id === 'amberbrook'), e = ab.course[ab.course.length - 1];
  let best = null, bd = Infinity;
  for (const p of VALLEY_LINES.silverrun) { const d = Math.hypot(p[0] - V(e[0]), p[1] - V(e[1])); if (d < bd) { bd = d; best = p; } }
  TWINMERE[0] = best[0] / KM; TWINMERE[1] = best[1] / KM;
  ab.course[ab.course.length - 1] = [TWINMERE[0], TWINMERE[1]];
}
for (const R of INCISED) incise(R);
cliffs();
karstTowers();
log(`karst: ${KARST_N} towers and stacks`);
lavaFlows();
duneFields();
basins(TARNS);
basins(FENPOOLS);
basins([OASIS]);
harrowbyGround();
for (let n = 0; n < P.h.length; n++) {
  if (!isFinite(P.h[n])) throw new Error(`non-finite post at (${(px(n) / KM).toFixed(2)}, ${(pz(n) / KM).toFixed(2)}) km`);
  P.h[n] = clamp(Math.round(P.h[n]), -MT.HEIGHT_LIMIT, MT.HEIGHT_LIMIT);
  if (P.h[n] < SEA - 300 && P.h[n] < H_DEF + 6) P.h[n] = H_DEF;
}
renderGrid('03_posts', P, {res: 128});
renderZooms(P);
{
  let hi = -Infinity, at = 0;
  for (let n = 0; n < P.h.length; n++) if (P.h[n] > hi) { hi = P.h[n]; at = n; }
  log(`highest post: ${((hi - SEA) / VPM).toFixed(0)} m at (${(px(at) / KM).toFixed(2)}, ${(pz(at) / KM).toFixed(2)}) km`);
}
if (STAGE === 'terrain') process.exit(0);

/* ===================================================================================
 * 8. WATER: the flow grid (12.8 m, the posts box-averaged), Priority-Flood
 * lakes (the designed basins, then the deeper natural ones nothing flows
 * through), and the NAMED RIVERS traced down the D8 flow of the final ground:
 * every river ends in the sea, a lake (its last point inside the outline) or
 * another river (`joins`, its last point snapped onto the parent's
 * centreline). A trace that runs into one of the KEPT lakes it was not aimed
 * at ends there and the lake's outflow carries on as a new river; a small
 * lake or a hollow a river crosses is filled to its spill level on the posts
 * (the engine's river bake keeps the running minimum of the ground, so a
 * hollow left on a river's line would be cut into a trench downstream).
 * =================================================================================== */
const F = (() => {
  const d = HY.downsample(P.h, NP, NP, 2);
  return {w: d.w, hh: d.hh, x0: MIN + PCELL / 2, z0: MIN + PCELL / 2, cell: PCELL * 2, h: d.h};
})();
const fIJ = (x, z) => [clamp(Math.round((x - F.x0) / F.cell), 0, F.w - 1), clamp(Math.round((z - F.z0) / F.cell), 0, F.hh - 1)];
const fXZ = n => [F.x0 + (n % F.w) * F.cell, F.z0 + ((n / F.w) | 0) * F.cell];

// THE LAKES: [id, name, x, z (km: a point in the basin), bed, shore, keep]
// keep: a river that meets it ends in it (its outflow carries on); otherwise a
// river crossing it fills it (the fen's pools)
const LAKE_SPECS = [
  ['lake_mirrormere', 'Mirrormere', 0.6, -1.7, 'shore_mud', 'gravel', true],
  ['lake_shardcrater', 'the Shardwell', CRATER.x, CRATER.z, 'gravel', 'gravel', true],
  ['lake_oasis', 'the Oasis of Sunscour', OASIS[0], OASIS[1], 'sand', 'sand', true],
  ...TARNS.map((t, k) => [`lake_tarn_${k + 1}`, ['Blackwater Tarn', 'the Hag\'s Eye', 'Cold Tarn', 'Heron Tarn', 'Moss Tarn', 'Linnet Tarn',
    'Peat Tarn', 'Cairn Tarn', 'Eldwell'][k], t[0], t[1], 'shore_mud', 'gravel', true]),
  ...FENPOOLS.map((t, k) => [`lake_fen_${k + 1}`, null, t[0], t[1], 'shore_mud', 'shore_mud', false]),
];
let LAKES = [], LAKE_POLY = [], LAKE_KEEP = [], LAKE_CELLS = [];
function addLake(rec, keep, cells) {
  if (rec.smooth && cells && cells.length * cellKm2 < 0.04) rec.smooth = false;
  const p = RB.parseLake(rec);
  if (p.error) { log('lake ' + rec.id + ': ' + p.error); return null; }
  const L = RB.lakePoly(p);
  LAKES.push(rec); LAKE_POLY.push(L); LAKE_KEEP.push(keep); LAKE_CELLS.push(cells);
  return L;
}
const lakeAtXZ = (x, z, skip = -1) => {
  for (let k = 0; k < LAKE_POLY.length; k++) {
    if (k === skip || !LAKE_POLY[k]) continue;
    const L = LAKE_POLY[k];
    if (x >= L.x0 && x <= L.x1 && z >= L.z0 && z <= L.z1 && RB.lakeInside(L.poly, Math.round(x), Math.round(z))) return k;
  }
  return -1;
};
let FILLED = null, NATURAL = [];
const USED = new Uint8Array(F.w * F.hh);
/** The F cells below `level` 4-connected to (x, z). */
function basinCells(level, x, z) {
  const [i, j] = fIJ(x, z), s = j * F.w + i, out = [s], seen = new Set([s]);
  for (let q = 0; q < out.length; q++) {
    const c = out[q], ci = c % F.w, cj = (c / F.w) | 0;
    for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const ii = ci + di, jj = cj + dj;
      if (ii < 0 || jj < 0 || ii >= F.w || jj >= F.hh) continue;
      const nb = jj * F.w + ii;
      if (!seen.has(nb) && F.h[nb] < level) { seen.add(nb); out.push(nb); }
    }
  }
  return out;
}
/** The lake in a dug basin: the flow-grid cells under `level` 4-connected to (x, z)
 *  and within `maxR` voxels of it, outlined -> {level, points, cells} or null. */
function basinLake(x, z, level, maxR) {
  const [i0, j0] = fIJ(x, z), s0 = j0 * F.w + i0;
  if (!(F.h[s0] < level)) return null;
  const R = Math.ceil(maxR / F.cell), seen = new Set([s0]), q = [s0];
  for (let h = 0; h < q.length; h++) {
    const c = q[h], ci = c % F.w, cj = (c / F.w) | 0;
    for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const ii = ci + di, jj = cj + dj;
      if (Math.abs(ii - i0) > R || Math.abs(jj - j0) > R || (ii - i0) ** 2 + (jj - j0) ** 2 > R * R) continue;
      const nb = jj * F.w + ii;
      if (!seen.has(nb) && F.h[nb] < level) { seen.add(nb); q.push(nb); }
    }
  }
  if (q.length < 3) return null;
  let a0 = Infinity, b0 = Infinity, a1 = -Infinity, b1 = -Infinity;
  for (const c of q) { const ci = c % F.w, cj = (c / F.w) | 0; a0 = Math.min(a0, ci); b0 = Math.min(b0, cj); a1 = Math.max(a1, ci); b1 = Math.max(b1, cj); }
  const bw = a1 - a0 + 1, bh = b1 - b0 + 1, m = new Uint8Array(bw * bh);
  for (const c of q) m[(((c / F.w) | 0) - b0) * bw + (c % F.w) - a0] = 1;
  const ring = HY.outline(m, bw, bh).map(([a, b]) => [a + a0, b + b0]);
  const simp = HY.simplify([...ring, ring[0]], 1).slice(0, -1);
  const pts = (simp.length >= 3 ? simp : ring).map(([a, b]) => [Math.round(F.x0 + (a - 0.5) * F.cell), Math.round(F.z0 + (b - 0.5) * F.cell)]);
  return {level: Math.round(level), points: pts, cells: q};
}
function buildLakes() {
  FILLED = HY.priorityFlood(F.h, F.w, F.hh, {seaY: SEA, epsilon: 0});
  for (const [id, name, kx, kz, bed, shore, keep] of LAKE_SPECS) {
    const [si, sj] = fIJ(V(kx), V(kz));
    if (USED[sj * F.w + si]) continue;                       // the same basin as a lake already made
    if (id === 'lake_shardcrater') BASIN_LEVEL[kx + ',' + kz] = Math.round(CRATER.g0 + 3 * VPM);
    if (id !== 'lake_mirrormere' && BASIN_LEVEL[kx + ',' + kz] == null) { log(`lake ${id}: its basin is too shallow`); continue; }
    const lv = BASIN_LEVEL[kx + ',' + kz];
    const basin = BASIN_R[kx + ',' + kz];
    const r = lv != null && basin ? basinLake(V(kx), V(kz), lv, basin * 2.2) : RB.lakeFromGrid(F, V(kx), V(kz), {seaY: SEA, filled: FILLED, tolerance: 1, id, ...(lv != null ? {level: lv} : {})});
    if (!r || r.cells < 6 || r.level <= SEA + 2) { log(`lake ${id}: no basin holds water at (${kx}, ${kz})`); continue; }
    const cells = r.cells && Array.isArray(r.cells) ? r.cells : basinCells(r.level, V(kx), V(kz));
    if (cells.some(c => USED[c])) continue;
    for (const c of cells) USED[c] = 1;
    addLake({id, ...(name ? {name} : {}), level: r.level, points: r.points, smooth: true, bed, shore}, keep, cells);
  }
  // the natural basins the land made (cirques, hollows on the moors): candidates, decided after the rivers
  NATURAL = HY.lakes(F.h, F.w, F.hh, {filled: FILLED, minDepth: 8, minCells: 4, outlines: false}).lakes
    .filter(l => l.level > SEA + 20 && !l.cells.some(c => USED[c]));
  log(`lakes: ${LAKES.length} designed, ${NATURAL.length} natural hollows`);
}

// THE NAMED RIVERS. src: [x, z] km (the trace starts at the largest flow within
// `pick` m of it) or lake: the lake it flows out of; to: 'sea' | lake id |
// river id (joins); w: [min, max] metres, k: width per sqrt(km2) of catchment;
// step: stepVox; meander: the RB.meander amount on the flat reaches.
const RIVER_SPECS = [
  {id: 'river_silverrun_head', name: 'Hollowcrown Beck', src: [0.45, -5.2], pick: 400, to: 'lake_mirrormere', w: [3, 10], k: 5, step: 32,
   about: 'The Silverrun\'s head: off Hollowcrown\'s south cirques, down a staircase of falls into Mirrormere.'},
  {id: 'river_silverrun', name: 'The Silverrun', lake: 'lake_mirrormere', to: 'sea', w: [10, 45], k: 5.5, step: 6, meander: 0.35,
   about: 'Out of Mirrormere through the Merewall notch, down the Mere Stair, south through Hearthvale past Harrowby to its delta in Gull Bay.'},
  {id: 'river_merebeck', name: 'Merebeck', src: [-0.4, -4.4], pick: 450, to: 'lake_mirrormere', w: [2, 8], k: 4, step: 32},
  {id: 'river_amberbrook', name: 'The Amberbrook', to: 'pool', step: 6,
   about: 'Out of Amberwood, round the west and south of Harrowby, into the Twinmere, the pool where it meets the Silverrun below the village.'},
  {id: 'river_eldwater', name: 'The Eldwater', lake: 'lake_tarn_9', to: 'sea', w: [6, 40], k: 5, step: 32, meander: 0.3,
   about: 'From the Eldwell tarn on Heatherhigh, off the moor\'s edge and west down the Eldwood to its swampy firth.'},
  {id: 'river_mossbeck', name: 'Mossbeck', src: [-6.6, 3.1], pick: 500, to: 'sea', w: [3, 12], k: 4, step: 8},
  {id: 'river_redrun', name: 'The Redrun', to: 'sea', step: 24,
   about: 'Off the Greyspine\'s east end, through a gorge across the Shardspire tableland, then down its slot canyon through Redreach to the Redmouth.'},
  {id: 'river_glimmer', name: 'The Glimmer', to: 'sea', step: 4,
   about: 'The slow, looping fen river of Glimmerfen, out to the Teeth Sound.'},
  {id: 'river_teethwater', name: 'The Teethwater', src: [-6.1, 6.2], pick: 400, to: 'sea', w: [3, 14], k: 4.5, step: 8, meander: 0.5},
  {id: 'river_chalkbourne', name: 'The Chalkbourne', to: 'sea', step: 8,
   about: 'A downland stream that ends over the white cliffs: a waterfall straight into the sea.'},
  {id: 'river_goldwater', name: 'The Goldwater', to: 'sea', step: 6,
   about: 'Off the Goldsea plain, down its own shallow vale into the east side of Gull Bay.'},
  {id: 'river_rime_west', name: 'Skarbeck', src: [-3.95, -6.05], pick: 350, to: 'sea', w: [3, 10], k: 4.5, step: 32},
  {id: 'river_rime_kings', name: 'Kingsbeck', src: [-1.6, -6.4], pick: 250, to: 'sea', w: [3, 10], k: 4.5, step: 32},
  {id: 'river_rime_east', name: 'Isbeck', src: [2.75, -6.15], pick: 350, to: 'sea', w: [3, 10], k: 4.5, step: 32},
];
let FLOW = null;
function buildFlow() {
  FLOW = HY.flow(F.h, F.w, F.hh, F.cell, {seaY: SEA});
  // DW_DUMP="x0,z0,x1,z1,step" (km): print a table of the ground (m) (a design aid)
  if (process.env.DW_DUMP) {
    const [x0, z0, x1, z1, st] = process.env.DW_DUMP.split(',').map(Number);
    let hdr = '      ';
    for (let x = x0; x <= x1 + 1e-9; x += st) hdr += x.toFixed(1).padStart(6);
    console.log(hdr);
    for (let z = z0; z <= z1 + 1e-9; z += st) {
      let row = z.toFixed(1).padStart(6);
      for (let x = x0; x <= x1 + 1e-9; x += st) row += String(Math.round((groundAt(V(x), V(z)) - SEA) / VPM)).padStart(6);
      console.log(row);
    }
  }
  // DW_TRACE="x,z;x,z" (km): print where the flow from each point goes (a design aid)
  for (const q of (process.env.DW_TRACE || '').split(';').filter(Boolean)) {
    const [kx, kz] = q.split(',').map(Number), [i, j] = fIJ(V(kx), V(kz));
    const t = traceFrom(j * F.w + i, -1, null);
    const pts = t.cells.filter((_, k) => k % 25 === 0 || k === t.cells.length - 1).map(c => { const [x, z] = fXZ(c); return `(${(x / KM).toFixed(2)},${(z / KM).toFixed(2)} ${((F.h[c] - SEA) / VPM).toFixed(0)}m)`; });
    log(`trace from (${q}): ends ${t.end}${t.end === 'lake' ? ' ' + LAKES[t.lake].id : ''}: ${pts.join(' ')}`);
  }
}
/** Every closed hollow that is not one of the lakes is FILLED to its spill
 *  level on the posts (+ a hair of gradient, so it drains): the canyon floors
 *  between mesas become playas, the moor's hollows bogs, and nothing on a
 *  river's line is left for the engine's running-minimum bake to trench
 *  through. Before that, the cirque-sized hollows high in the mountains are
 *  kept as tarns (lake_hollow_*). */
function fillHollows() {
  let tarns = 0;
  const nat = NATURAL.slice().sort((a, b) => b.maxDepth - a.maxDepth || b.area - a.area);
  for (const l of nat) {
    if (tarns >= 14) break;
    if (!(l.level > Y(250) && l.area >= 12 && l.area <= 500 && l.maxDepth >= 20) || l.cells.some(c => USED[c])) continue;
    const [x, z] = fXZ(l.cells[0]);
    // only in the mountains (the Greyspine and the moor), not on the mesas' tops
    if (!(WL.bil(C, ZONE_MASK.heatherhigh, x, z) > 0.5 || distToLine(P2(GREYSPINE), x, z) < 22000) || WL.bil(C, ZONE_MASK.redreach, x, z) > 0.1 || WL.bil(C, ZONE_MASK.shardspire, x, z) > 0.3) continue;
    const r = RB.lakeFromGrid(F, x, z, {seaY: SEA, filled: FILLED, tolerance: 1.6});
    if (!r || lakeAtXZ(x, z) >= 0) continue;
    for (const c of l.cells) USED[c] = 1;
    addLake({id: `lake_hollow_${++tarns}`, level: r.level, points: r.points, smooth: false, bed: 'shore_mud', shore: 'gravel'}, true, l.cells);
  }
  const FE = HY.priorityFlood(F.h, F.w, F.hh, {seaY: SEA, epsilon: 0.05});
  let n = 0;
  for (let c = 0; c < F.h.length; c++) {
    if (USED[c] || !(FE[c] > F.h[c] + 2) || F.h[c] < SEA) continue;
    const ci = c % F.w, cj = (c / F.w) | 0;
    for (let dj = 0; dj < 2; dj++) for (let di = 0; di < 2; di++) {
      const pi = ci * 2 + di, pj = cj * 2 + dj;
      if (pi < NP && pj < NP && P.h[pj * NP + pi] < FE[c]) P.h[pj * NP + pi] = FE[c];
    }
    F.h[c] = FE[c];
    n++;
  }
  log(`hollows: ${tarns} mountain tarns kept, ${n} cells (${(n * cellKm2).toFixed(1)} km2) of hollow filled`);
}
const cellKm2 = (F.cell / VPM / 1000) ** 2;
const RIVERS = [], RIVER_LINE = {}, RIVER_CELLS = {}, CROSSED = new Set();
// cell -> id of the river already built through it (any river, not only a
// spec's `to` parent): a later trace that reaches one JOINS it, and a source
// is never picked on one. Without it Merebeck's source landed on Hollowcrown
// Beck's own channel and the two traced one valley 36 voxels apart for 20 km
// (ticket 2026-10-16_11-13-31: 66-voxel slabs of water where they overlapped).
const RIVER_OWNER = new Map();
// river id -> where its water ends up ('sea' or a lake id), through joins: a
// trace may join a river on its way only if that river goes where it was sent.
const RIVER_DEST = {};
/** Follow the D8 receivers from cell c until the sea, a kept lake (other than
 *  `fromLake`; `target` always stops it), or a cell of `joinSet`.
 *  -> {cells, end: 'sea'|'lake'|'join'|'pit', lake} (crossed droppable lakes noted) */
function traceFrom(c, fromLake, joinSet, target = -1) {
  const cells = [c], seen = new Set([c]), crossed = new Set();
  for (let g = 0; g < 200000; g++) {
    const [x, z] = fXZ(c);
    const lk = lakeAtXZ(x, z, fromLake);
    if (F.h[c] < SEA && lk < 0) return {cells, end: 'sea', crossed};
    if (lk >= 0 && g > 0) {
      if (LAKE_KEEP[lk] || lk === target) return {cells, end: 'lake', lake: lk, crossed};
      crossed.add(lk);
    }
    if (joinSet && joinSet.has(c) && g > 2) return {cells, end: 'join', crossed};
    if (g > 2 && RIVER_OWNER.has(c)) return {cells, end: 'join', joinTo: RIVER_OWNER.get(c), crossed};
    const r = FLOW.rcv[c];
    if (r === c || seen.has(r)) return {cells, end: 'pit', crossed};
    seen.add(r); cells.push(r); c = r;
  }
  return {cells, end: 'pit', crossed};
}
/** The trace's world polyline, simplified and smoothed, with the catchment at each knot. */
function traceLine(cells, tol = 0.6) {
  const pts = cells.map(c => { const [x, z] = fXZ(c); return [x, z, FLOW.A[c]]; });
  const simp = HY.simplify(pts, F.cell * tol);
  const sm = HY.chaikin(simp.map(p => [p[0], p[1]]), 2, false);
  const out = WL.resample(sm, F.cell * 12);
  let k = 0;
  return out.map(p => {
    while (k + 1 < pts.length && Math.hypot(pts[k + 1][0] - p[0], pts[k + 1][1] - p[1]) <= Math.hypot(pts[k][0] - p[0], pts[k][1] - p[1])) k++;
    return [p[0], p[1], pts[k][2]];
  });
}
function buildRivers() {
  const queue = RIVER_SPECS.map(s => ({...s, depth: 0}));
  while (queue.length) {
    const S = queue.shift();
    const parent = typeof S.to === 'string' && S.to.startsWith('river_') ? S.to : null;
    const target = typeof S.to === 'string' && S.to.startsWith('lake_') ? LAKES.findIndex(l => l.id === S.to) : -1;
    const joinSet = parent ? RIVER_CELLS[parent] : null;
    if (parent && !joinSet) { log(`river ${S.id}: its parent ${parent} is not built (order)`); continue; }
    if (INCISED_LINE[S.id]) { authoredRiver(S, parent); continue; }
    let tr = null, fromLake = -1;
    if (S.lake) {
      // a lake's outflow: from the basin's deepest cell the flow crosses the (filled) lake to its spill point
      fromLake = LAKES.findIndex(l => l.id === S.lake);
      if (fromLake < 0) { log(`river ${S.id}: no lake ${S.lake}`); continue; }
      let best = LAKE_CELLS[fromLake][0], bd = -Infinity;
      for (const c of LAKE_CELLS[fromLake]) { const dep = LAKES[fromLake].level - F.h[c]; if (dep > bd) { bd = dep; best = c; } }
      tr = traceFrom(best, fromLake, joinSet, target);
      let last = 0;
      for (let k = 0; k < tr.cells.length; k++) { const [x, z] = fXZ(tr.cells[k]); if (lakeAtXZ(x, z) === fromLake) last = k; }
      tr.cells = tr.cells.slice(Math.max(0, last - 3));
    } else {
      // the source: the cell of largest flow near the given point whose trace ends where it should
      const [si, sj] = fIJ(V(S.src[0]), V(S.src[1])), R = Math.round(S.pick * VPM / F.cell);
      const cand = [];
      for (let j = sj - R; j <= sj + R; j++) for (let i = si - R; i <= si + R; i++) {
        if (i < 0 || j < 0 || i >= F.w || j >= F.hh || (i - si) ** 2 + (j - sj) ** 2 > R * R) continue;
        const n = j * F.w + i;
        if (F.h[n] > SEA && lakeAtXZ(...fXZ(n)) < 0 && !RIVER_OWNER.has(n)) cand.push(n);
      }
      cand.sort((a, b) => FLOW.A[b] - FLOW.A[a] || a - b);
      let fallback = null;
      for (const c of cand.slice(0, 300)) {
        const t = traceFrom(c, -1, joinSet, target);
        // Running into another river first makes a tributary of it -- if that
        // river takes the water where this one was sent.
        const ok = S.to === 'sea' ? t.end === 'sea' || (t.end === 'join' && !parent && RIVER_DEST[t.joinTo] === 'sea')
                 : parent ? t.end === 'join'
                 : (t.end === 'lake' && t.lake === target) || (t.end === 'join' && RIVER_DEST[t.joinTo] === S.to);
        if (ok && t.cells.length > 8) { tr = t; break; }
        if (!fallback && t.cells.length > 8 && t.end !== 'pit') fallback = t;
      }
      if (!tr) { tr = fallback; if (tr) log(`river ${S.id}: no source near (${S.src}) reaches ${S.to}; it ends ${tr.end}`); }
      if (!tr) { log(`river ${S.id}: no trace from (${S.src}): ${cand.length} candidates, ground ${((F.h[sj * F.w + si] - SEA) / VPM).toFixed(1)} m, first ends ${cand.slice(0, 5).map(c => traceFrom(c, -1, joinSet, target).end).join(' ')}`); continue; }
    }
    if (tr.end === 'pit') { log(`river ${S.id}: ends in a pit (dropped)`); continue; }
    for (const k of tr.crossed) CROSSED.add(k);
    // a kept lake it was not aimed at: end there, and the lake's outflow continues it
    if (tr.end === 'lake' && LAKES[tr.lake].id !== S.to) {
      if (S.depth < 3) queue.unshift({...S, id: S.id + '_' + (S.depth + 2), lake: LAKES[tr.lake].id, src: null, about: null, meander: S.meander, depth: S.depth + 1});
      log(`river ${S.id}: runs into ${LAKES[tr.lake].id}; its outflow carries on`);
    }
    const line = traceLine(tr.cells, S.step <= 8 ? 2.2 : 0.6);
    if (line.length < 2) continue;
    const wOf = A => clamp(S.k * Math.sqrt(A * cellKm2), S.w[0], S.w[1]) * VPM;
    let pts = line.map(p => { const w = wOf(p[2]); return [Math.round(p[0]), Math.round(p[1]), Math.round(w), Math.round(clamp(w / 8, 4, 30))]; });
    if (tr.end === 'sea') {
      // the mouth: on out into the sea
      const a = pts[pts.length - 2], b = pts[pts.length - 1], l = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1;
      pts.push([Math.round(b[0] + (b[0] - a[0]) / l * 600), Math.round(b[1] + (b[1] - a[1]) / l * 600), b[2], b[3]]);
    } else if (tr.end === 'join') {
      const T = SPL.tessellate(RIVER_LINE[tr.joinTo || parent].map(p => [p[0], p[1]]), {step: 16});
      const e = pts[pts.length - 1], c = SPL.closest(T, e[0], e[1]);
      pts[pts.length - 1] = [Math.round(c.x), Math.round(c.z), e[2], e[3]];
    }
    if (S.meander && pts.length > 3) pts = RB.meander(pts, {amount: S.meander, seed: S.id.length * 31 + 7, ground: groundAt});
    {
      let lastS = 0, acc = 0;
      for (let k = 1; k < pts.length - 1; k++) {
        acc += Math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]);
        if (acc - lastS >= 12000 && acc < WL.polyLength(pts) - 6000) lastS = acc; else pts[k] = [pts[k][0], pts[k][1]];
      }
    }
    const rec = {id: S.id, ...(S.name ? {name: S.name} : {}), points: pts, smooth: true, bed: S.bed || 'gravel',
                 ...(tr.end === 'join' ? {joins: tr.joinTo || parent} : {}), stepVox: S.step, ...(S.about ? {about: S.about} : {})};
    RIVERS.push(rec);
    RIVER_LINE[S.id] = pts;
    const set = new Set();
    for (const c of tr.cells) { const ci = c % F.w, cj = (c / F.w) | 0; for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) set.add((cj + dj) * F.w + ci + di); }
    RIVER_CELLS[S.id] = set;
    for (const c of set) if (!RIVER_OWNER.has(c)) RIVER_OWNER.set(c, S.id);
    RIVER_DEST[S.id] = tr.end === 'join' ? RIVER_DEST[tr.joinTo || parent] : tr.end === 'lake' ? LAKES[tr.lake].id : tr.end;
    log(`river ${S.id}: ${pts.length} knots, ${(WL.polyLength(pts) / KM).toFixed(2)} km, ends ${tr.end}${tr.end === 'lake' ? ' ' + LAKES[tr.lake].id : tr.end === 'join' ? ' ' + (tr.joinTo || parent) : ''}`);
  }
}
/** An INCISED river's record: its drawn course, knots every ~150 m, width and
 *  depth from the INCISED entry (source -> mouth) at the ends and every ~1.2 km. */
function authoredRiver(S, parent) {
  const I = INCISED_LINE[S.id], R = INCISED.find(r => r.river === S.id);
  const pts = WL.resample(I.line, 1500).map(p => [Math.round(p[0]), Math.round(p[1])]);
  const L = WL.polyLength(pts);
  let acc = 0, last = -Infinity;
  for (let k = 0; k < pts.length; k++) {
    if (k) acc += Math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]);
    if (k === 0 || k === pts.length - 1 || (acc - last >= 12000 && acc < L - 6000)) {
      const t = acc / L;
      pts[k].push(Math.round(lerp(R.w[0], R.w[1], Math.sqrt(t)) * VPM), Math.round(lerp(R.d[0], R.d[1], t) * VPM));
      last = acc;
    }
  }
  if (typeof S.to === 'string' && S.to.startsWith('lake_')) {
    // into the lake: the last knot well inside its outline (at the deepest point of the basin)
    const k = LAKES.findIndex(l => l.id === S.to);
    if (k >= 0) {
      let best = LAKE_CELLS[k][0], bd = -Infinity;
      for (const c of LAKE_CELLS[k]) { const dep = LAKES[k].level - F.h[c]; if (dep > bd) { bd = dep; best = c; } }
      const [lx, lz] = fXZ(best), e = pts[pts.length - 1];
      pts[pts.length - 1] = e.slice(0, 2);
      pts.push([Math.round(lx), Math.round(lz), e[2], e[3]]);
    }
  } else if (S.to === 'sea') {
    const a = pts[pts.length - 2], b = pts[pts.length - 1], l = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1;
    pts.push([Math.round(b[0] + (b[0] - a[0]) / l * 500), Math.round(b[1] + (b[1] - a[1]) / l * 500), b[2], b[3]]);
    pts[pts.length - 2] = pts[pts.length - 2].slice(0, 2);
  } else if (parent) {
    const T = SPL.tessellate(RIVER_LINE[parent].map(p => [p[0], p[1]]), {step: 16});
    const e = pts[pts.length - 1], c = SPL.closest(T, e[0], e[1]);
    pts[pts.length - 1] = [Math.round(c.x), Math.round(c.z), e[2], e[3]];
  }
  RIVERS.push({id: S.id, name: S.name, points: pts, smooth: true, bed: S.bed || 'gravel', ...(parent ? {joins: parent} : {}), stepVox: S.step,
               ...(S.about ? {about: S.about} : {})});
  RIVER_LINE[S.id] = pts;
  const set = new Set();
  for (const p of I.line) { const [i, j] = fIJ(p[0], p[1]); set.add(j * F.w + i); }
  RIVER_CELLS[S.id] = set;
  for (const c of set) if (!RIVER_OWNER.has(c)) RIVER_OWNER.set(c, S.id);
  RIVER_DEST[S.id] = parent ? RIVER_DEST[parent] : S.to;
  log(`river ${S.id} (incised): ${pts.length} knots, ${(L / KM).toFixed(2)} km${parent ? ', joins ' + parent : ''}`);
}
/** THE TWINMERE: the Amberbrook meets the Silverrun in a pool. A river-to-
 *  river junction is at the mercy of the engine's noise under both running
 *  minimums (the loader warns when the tributary's water would sit below the
 *  parent's); a lake is exact. So the pool is dug where the traced Silverrun
 *  passes nearest the Amberbrook's end: the Silverrun ends in it, the
 *  Amberbrook ends in it, and the lower Silverrun flows out of it. */
function makeConfluence() {
  const sr = RIVERS.find(r => r.id === 'river_silverrun'), ab = RIVERS.find(r => r.id === 'river_amberbrook');
  if (!sr || !ab) return;
  const T = SPL.tessellate(sr.points.map(p => [p[0], p[1]]), {step: 32});
  const e = ab.points[ab.points.length - 1], c = SPL.closest(T, e[0], e[1]);
  TWINMERE[0] = c.x / KM; TWINMERE[1] = c.z / KM;
  basins([TWINMERE]);
  // the flow grid over the pool, again from the posts
  const R = V(TWINMERE[2]) * 2.5;
  for (let j = fIJ(0, c.z - R)[1]; j <= fIJ(0, c.z + R)[1]; j++)
    for (let i = fIJ(c.x - R, 0)[0]; i <= fIJ(c.x + R, 0)[0]; i++) {
      const pi = i * 2, pj = j * 2;
      F.h[j * F.w + i] = (P.h[pj * NP + pi] + P.h[pj * NP + pi + 1] + P.h[(pj + 1) * NP + pi] + P.h[(pj + 1) * NP + pi + 1]) / 4;
    }
  const lv = BASIN_LEVEL[TWINMERE[0] + ',' + TWINMERE[1]];
  const r = lv == null ? null : basinLake(c.x, c.z, lv, V(TWINMERE[2]) * 2.2);
  if (!r) { log('the Twinmere: no pool could be dug'); return; }
  const k = LAKES.length;
  addLake({id: 'lake_twinmere', name: 'the Twinmere', level: r.level, points: r.points, smooth: true, bed: 'shore_mud', shore: 'gravel'}, true, r.cells);
  // split the Silverrun at the pool: its knots before the pool + the centre; the centre + its knots after
  const P0 = sr.points, inside = p => RB.lakeInside(LAKE_POLY[k].poly, p[0], p[1]);
  let a = P0.findIndex(inside);
  if (a < 0) a = P0.findIndex((p, q) => q > 0 && Math.hypot(p[0] - c.x, p[1] - c.z) < Math.hypot(P0[q - 1][0] - c.x, P0[q - 1][1] - c.z) && q + 1 < P0.length && Math.hypot(P0[q + 1][0] - c.x, P0[q + 1][1] - c.z) > Math.hypot(p[0] - c.x, p[1] - c.z));
  let b = a;
  while (b + 1 < P0.length && inside(P0[b + 1])) b++;
  // w / d at the split: the nearest knots that carry them
  const wd = q => { for (let d = 0; d < P0.length; d++) { for (const t of [q - d, q + d]) if (t >= 0 && t < P0.length && P0[t].length >= 4) return [P0[t][2], P0[t][3]]; } return [200, 20]; };
  const centre = [Math.round(c.x), Math.round(c.z), ...wd(a)];
  const up = [...P0.slice(0, Math.max(1, a)).map(p => p.slice()), centre.slice()];
  const down = [centre.slice(), ...P0.slice(Math.min(P0.length - 1, b + 1)).map(p => p.slice())];
  if (up[up.length - 2].length < 4) up[up.length - 2] = up[up.length - 2].slice(0, 2);
  sr.points = up;
  RIVERS.splice(RIVERS.indexOf(sr) + 1, 0, {...sr, id: 'river_silverrun_lower', name: 'The Silverrun', points: down,
    about: 'Out of the Twinmere, south through Hearthvale to its delta in Gull Bay.'});
  RIVER_LINE.river_silverrun_lower = down;
  ab.points[ab.points.length - 1] = [centre[0], centre[1], ...ab.points[ab.points.length - 1].slice(2)];
  for (const rv of RIVERS) {
    if (rv === sr || rv.id === 'river_silverrun_lower' || rv === ab) continue;
    const q = rv.points.findIndex(p => RB.lakeInside(LAKE_POLY[k].poly, p[0], p[1]));
    if (q > 0) {
      const last = rv.points[q], wdq = rv.points.slice(0, q).reverse().find(p => p.length >= 4) || [0, 0, 80, 8];
      rv.points = [...rv.points.slice(0, q).map((p, i) => (i === 0 ? p : p.slice(0, 2))), [last[0], last[1], wdq[2], wdq[3]]];
      rv.points[0] = rv.points[0].length >= 4 ? rv.points[0] : [...rv.points[0], wdq[2], wdq[3]];
      log(`river ${rv.id}: ends in the Twinmere`);
    }
  }
  log(`the Twinmere at (${TWINMERE[0].toFixed(2)}, ${TWINMERE[1].toFixed(2)}) km, level ${((r.level - SEA) / VPM).toFixed(1)} m: the Silverrun split at knots ${a}..${b}`);
}
/** After the rivers: the lakes a river crossed and the hollows on a river's
 *  line are FILLED to their spill level on the posts (no lake, no trench); the
 *  larger natural hollows nothing flows through become tarns. */
function settleWater() {
  const onRiver = new Uint8Array(F.w * F.hh);
  for (const set of Object.values(RIVER_CELLS)) for (const c of set) if (c >= 0 && c < onRiver.length) onRiver[c] = 1;
  const fill = (cells, level) => {
    for (const c of cells) {
      const ci = c % F.w, cj = (c / F.w) | 0;
      for (let dj = 0; dj < 2; dj++) for (let di = 0; di < 2; di++) {
        const pi = ci * 2 + di, pj = cj * 2 + dj;
        if (pi < NP && pj < NP && P.h[pj * NP + pi] < level) P.h[pj * NP + pi] = level;
      }
    }
  };
  let dropped = 0;
  for (const k of [...CROSSED].sort((a, b) => b - a)) {
    fill(LAKE_CELLS[k], LAKES[k].level + 1);
    LAKES.splice(k, 1); LAKE_POLY.splice(k, 1); LAKE_KEEP.splice(k, 1); LAKE_CELLS.splice(k, 1);
    dropped++;
  }
  log(`water settled: ${dropped} small lakes a river crossed filled, ${LAKES.length} lakes`);
}
/** Every traced river's channel cut into the posts: along its spline the
 *  running minimum of the ground less `cut` m, over its width and a little.
 *  The engine bakes the water from the ground's running minimum too, so a
 *  river whose plane already holds a monotone channel gets reaches that sit
 *  in it, and a tributary arriving over the floodplain is always above its
 *  parent's water (the junction rule). Lakes on the line are skipped (an
 *  outflow starts from the lake's level). */
function channelRivers() {
  for (const r of RIVERS) {
    if (INCISED_LINE[r.id]) continue;
    const knots = r.points.map(p => [p[0], p[1], p.length >= 4 ? p[2] : undefined]);
    const T = SPL.tessellate(knots, {step: 32});
    const N = T.pts.length;
    const g = TG.blur(Float32Array.from(T.pts.map(p => groundAt(p[0], p[1]))), N, 1, 2, 2);
    const floor = new Float32Array(N).fill(NaN);
    let run = Infinity;
    const cut = (r.stepVox || 4) >= 24 ? 0.6 : 1.5;
    for (let i = 0; i < N; i++) {
      const lk = lakeAtXZ(T.pts[i][0], T.pts[i][1]);
      if (lk >= 0) { run = Math.min(run, LAKES[lk].level - 5); continue; }
      run = Math.min(run, g[i] - cut * VPM);
      floor[i] = Math.max(run, SEA - 30);
    }
    stairFloor(floor, T.s);
    const hwMax = Math.max(...T.pts.map(p => p[2] || 80)) / 2 + 300, xs = T.pts.map(p => p[0]), zs = T.pts.map(p => p[1]);
    const sg = postBox([Math.min(...xs) - hwMax, Math.min(...zs) - hwMax, Math.max(...xs) + hwMax, Math.max(...zs) + hwMax]);
    const Cc = TG.curveCoords(sg, T.pts.map(p => [p[0], p[1], p[2] || 80]), hwMax, 1);
    for (let n = 0; n < sg.h.length; n++) {
      const d = Cc.d[n];
      if (!isFinite(d)) continue;
      let lo = 0, hi = N - 1;
      while (hi - lo > 1) { const m = (lo + hi) >> 1; if (T.s[m] <= Cc.s[n]) lo = m; else hi = m; }
      const fl = floor[lo];
      if (!isFinite(fl) || sg.h[n] <= fl) continue;
      const hw = Cc.attrs[0][n] / 2 + 60, u = Math.sqrt(d * d + Cc.beyond[n] * Cc.beyond[n]) / hw;
      if (u < 1.6) sg.h[n] = lerp(sg.h[n], Math.max(fl, Math.min(sg.h[n], fl + (sg.h[n] - fl) * sstep(0.6, 1.6, u))), 1 - sstep(1, 1.6, u));
    }
    TG.pasteGrid(P, sg);
  }
}
/** The engine's bake of every river on the plane's ground: reaches, falls, warnings. */
let RIVER_BAKE = null;
function checkRivers() {
  const res = RB.bakeAll({seaLevelY: SEA, rivers: RIVERS, lakes: LAKES}, {ground: groundAt});
  for (const e of res.errors) log('RIVER ERROR ' + e);
  for (const w of res.warnings) log('river warning: ' + w);
  for (const b of res.rivers) log(`  ${b.id}: ${b.reaches} reaches, ${b.pieces} pieces, ${b.falls.length} falls (max ${Math.max(0, ...b.falls.map(f => f[2])) / VPM} m), drains to ${b.drainsTo || 'sea'} at ${b.endLevel}`);
  RIVER_BAKE = res;
  // DW_RIVER=<id>: print that river's bake profile (a design aid)
  for (const b of res.rivers) if (b.id === process.env.DW_RIVER) {
    const sm = b.samples, k = Math.max(1, Math.floor(sm.x.length / 80));
    for (let i = 0; i < sm.x.length; i += k)
      console.log(`  s ${sm.s[i]} at (${(sm.x[i] / KM).toFixed(3)}, ${(sm.z[i] / KM).toFixed(3)}) ground ${sm.g[i]} water ${sm.S[i]}`);
  }
  return res;
}
/** An ESTIMATE of the engine's per-site-cell lists (kSiteCellMax 128 per 102.4 m
 *  cell): river pieces split as riverbake.js / worldwater.cpp split them, lake
 *  edges, road pieces, each counted in every cell its reach box touches, plus
 *  Harrowby's ~44 (houses, waynode routes, its paths). The engine's --mapcheck
 *  siteCellWorst is the truth; this keeps the design honest before a run. */
function siteBudget() {
  const S = 1024, cnt = new Map(), byWho = new Map();
  let who = '';
  const add = (x0, z0, x1, z1) => {
    for (let cz2 = Math.floor(z0 / S); cz2 <= Math.floor(z1 / S); cz2++)
      for (let cx2 = Math.floor(x0 / S); cx2 <= Math.floor(x1 / S); cx2++) {
        const k = (cz2 + 1000) * 10000 + cx2 + 1000;
        cnt.set(k, (cnt.get(k) || 0) + 1);
        let m = byWho.get(k); if (!m) byWho.set(k, m = new Map()); m.set(who, (m.get(who) || 0) + 1);
      }
  };
  for (const b of RIVER_BAKE.rivers) {
    who = b.id;
    const sm = b.samples, N = sm.x.length;
    const starts = b.starts;
    for (let r = 0; r < starts.length; r++) {
      const a0 = starts[r], b0 = r + 1 < starts.length ? starts[r + 1] : N - 1;
      if (b0 <= a0) continue;
      const stack = [[a0, b0]];
      while (stack.length) {
        const [a, c] = stack.pop();
        const vx = sm.x[c] - sm.x[a], vz = sm.z[c] - sm.z[a], vl = Math.max(Math.hypot(vx, vz), 1e-9);
        let dev = 0, farK = -1;
        for (let k = a + 1; k < c; k++) {
          let d = Math.abs((sm.x[k] - sm.x[a]) * vz - (sm.z[k] - sm.z[a]) * vx) / vl;
          const f = sm.s[c] > sm.s[a] ? (sm.s[k] - sm.s[a]) / (sm.s[c] - sm.s[a]) : 0;
          d = Math.max(d, Math.abs(sm.hw[k] - (sm.hw[a] + (sm.hw[c] - sm.hw[a]) * f)), Math.abs(sm.d[k] - (sm.d[a] + (sm.d[c] - sm.d[a]) * f)));
          if (d > dev) { dev = d; farK = k; }
        }
        if (c - a > 1 && (Math.max(Math.abs(vx), Math.abs(vz)) > 256 || dev > 1)) {
          const mid = dev <= 1 || farK < 0 ? Math.trunc((a + c) / 2) : farK;
          stack.push([mid, c], [a, mid]);
          continue;
        }
        const hw = Math.max(sm.hw[a], sm.hw[c]), reach = hw + Math.max(b.bankAt(hw), 80, 32) + 5;
        add(Math.min(sm.x[a], sm.x[c]) - reach, Math.min(sm.z[a], sm.z[c]) - reach, Math.max(sm.x[a], sm.x[c]) + reach, Math.max(sm.z[a], sm.z[c]) + reach);
      }
    }
  }
  for (const [li, L] of LAKE_POLY.entries()) {
    who = LAKES[li].id;
    const p = L.poly;
    for (let k = 0; k < p.length; k++) {
      const a = p[k], c = p[(k + 1) % p.length], n = Math.max(1, Math.ceil(Math.hypot(c[0] - a[0], c[1] - a[1]) / 256));
      for (let q = 0; q < n; q++) {
        const ax = a[0] + (c[0] - a[0]) * q / n, az = a[1] + (c[1] - a[1]) * q / n, bx = a[0] + (c[0] - a[0]) * (q + 1) / n, bz = a[1] + (c[1] - a[1]) * (q + 1) / n;
        add(Math.min(ax, bx) - 110, Math.min(az, bz) - 110, Math.max(ax, bx) + 110, Math.max(az, bz) + 110);
      }
    }
  }
  for (const r of ROADS) {
    who = r.id;
    const pts = SPL.smoothRoadPoints(r.points), reach = r.width / 2 + r.shoulder + 64;
    for (let k = 1; k < pts.length; k++) {
      const a = pts[k - 1], c = pts[k], n = Math.max(1, Math.ceil(Math.max(Math.abs(c[0] - a[0]), Math.abs(c[1] - a[1])) / 64));
      for (let q = 0; q < n; q++) {
        const ax = a[0] + (c[0] - a[0]) * q / n, az = a[1] + (c[1] - a[1]) * q / n, bx = a[0] + (c[0] - a[0]) * (q + 1) / n, bz = a[1] + (c[1] - a[1]) * (q + 1) / n;
        add(Math.min(ax, bx) - reach, Math.min(az, bz) - reach, Math.max(ax, bx) + reach, Math.max(az, bz) + reach);
      }
    }
  }
  who = 'harrowby';
  add(330, 3090, 790, 3650); for (let k = 0; k < 43; k++) add(330, 3090, 790, 3650);
  const top = [...cnt.entries()].sort((a, b) => b[1] - a[1]).slice(0, 5);
  for (const [k, v] of top) {
    const cz3 = Math.floor(k / 10000) - 1000, cx3 = (k % 10000) - 1000;
    log(`  cell (${(cx3 * S / KM).toFixed(2)}, ${(cz3 * S / KM).toFixed(2)}) km: ${v} = ${[...byWho.get(k).entries()].sort((a, b) => b[1] - a[1]).map(([w, c]) => w + ' ' + c).join(', ')}`);
  }
  let worst = 0, at = 0;
  for (const [k, v] of cnt) if (v > worst) { worst = v; at = k; }
  const wz = Math.floor(at / 10000) - 1000, wx = (at % 10000) - 1000;
  log(`site cells (estimate, road pieces counted every 6.4 m: an over-count): worst ${worst} at cell (${wx}, ${wz}) = (${(wx * S / KM).toFixed(2)}, ${(wz * S / KM).toFixed(2)}) km`);
  return worst;
}

/* ===================================================================================
 * 9. BIOMES: the climate (climate.js: westerlies, a cold north, lapse rate,
 * the rain shadow) gives every 25.6 m cell its nearest biome; then the brief's
 * zones are ENFORCED where the climate alone would not put the right biome
 * (each zone's rule below), and those cells are LOCKED (map.json
 * climate.locks), so the tuner's Generate keeps them and re-derives the rest.
 * =================================================================================== */
const PALETTE = ['forest', 'meadow', 'pine', 'desert', 'tundra', 'swamp', 'alpine', 'ocean',
                 'ashlands', 'mesa', 'moor', 'glowfen', 'glacier', 'savanna', 'dunes', 'amberwood', 'eldwood',
                 'crystal_barrens', 'steppe', 'jungle', 'downs', 'karst'];
const BI = Object.fromEntries(PALETTE.map((b, k) => [b, k]));
const CLIMATE = {windFrom: 265, seaTemp: 13, latGrad: 1.1, coldDir: 0, lapse: 6.5, aspect: 2, coastal: 0.4, coastReachKm: 3,
                 rain: 1.1, orographic: 1.3, wetness: 1, wT: 1, wM: 1, jitter: 0.55, minPatch: 16, seed: 3, glacierT: -4, glacierAltM: 900,
                 never: ['ashlands', 'crystal_barrens', 'mesa', 'karst', 'glowfen']};
const NB = SIZE / 256 + 1;        // biome samples a side (the last row lies past the extent)
const B = {w: NB, hh: NB, x0: MIN + 128, z0: MIN + 128, cell: 256, h: new Float32Array(NB * NB)};
let BIOME = null, LOCKS = null, CLIM = null;
const ZONE_OUTLINE = {};
/** Patches (4-connected) under minN cells take the biome of the neighbour they
 *  share the most border with (ocean never merges, nor is merged into). Two passes. */
function mergeSmallPatches(out, w, minN) {
  const lab = new Int32Array(out.length), q = [];
  for (let pass = 0; pass < 2; pass++) {
    lab.fill(0);
    let L = 0;
    for (let n0 = 0; n0 < out.length; n0++) {
      if (lab[n0] || out[n0] === BI.ocean) continue;
      const b0 = out[n0];
      lab[n0] = ++L; q.length = 0; q.push(n0);
      for (let k = 0; k < q.length; k++) {
        const c = q[k], ci = c % w, cj = (c / w) | 0;
        for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const ii = ci + di, jj = cj + dj;
          if (ii < 0 || jj < 0 || ii >= w || jj >= w) continue;
          const nb = jj * w + ii;
          if (!lab[nb] && out[nb] === b0) { lab[nb] = L; q.push(nb); }
        }
      }
      if (q.length >= minN) continue;
      const votes = new Map();
      for (const c of q) {
        const ci = c % w, cj = (c / w) | 0;
        for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const ii = ci + di, jj = cj + dj;
          if (ii < 0 || jj < 0 || ii >= w || jj >= w) continue;
          const v = out[jj * w + ii];
          if (v !== b0 && v !== BI.ocean) votes.set(v, (votes.get(v) || 0) + 1);
        }
      }
      let best = -1, bv = 0;
      for (const [v, k] of votes) if (k > bv || (k === bv && v < best)) { bv = k; best = v; }
      if (best >= 0) for (const c of q) out[c] = best;
    }
  }
}
function buildBiomes() {
  for (let n = 0; n < B.h.length; n++) B.h[n] = WL.bil(P, P.h, B.x0 + (n % NB) * 256, B.z0 + ((n / NB) | 0) * 256);
  const table = PALETTE.map(name => {
    const j = JSON.parse(readFileSync(join(ROOT, 'assets', 'biomes', name + '.json'), 'utf8'));
    return {name, T: +j.climate.temperature, M: +j.climate.moisture};
  });
  const res = CL.climateJob({grid: B, seaY: SEA, vpm: VPM, params: CLIMATE, biomes: table, rivers: RIVERS, lakes: LAKES, classify: true});
  CLIM = res;
  const auto = res.biome, out = Uint8Array.from(auto);
  // the zone field: every zone's mask read at a position warped at four scales
  // (15 km down to 250 m), times a multi-scale noise; the cell keeps its two
  // strongest zones and the ground decides between them (below), so a border is
  // a band of interlocking fingers that follows valleys and ridges
  const zoneIds = ZONES.filter(z => z.id !== 'default').map(z => z.id);
  const zone1 = new Array(B.h.length), zone2 = new Array(B.h.length), zmix = new Float32Array(B.h.length);
  for (let n = 0; n < B.h.length; n++) {
    const x = B.x0 + (n % NB) * 256, z = B.z0 + ((n / NB) | 0) * 256;
    const qx = x + 15000 * TG.fbm(x / 60000, z / 60000, 601, 4) + 9000 * TG.fbm(x / 12000, z / 12000, 603, 3) + 12000 * TG.fbm(x / 5000, z / 5000, 609, 3) + 5000 * TG.fbm(x / 1800, z / 1800, 615, 2) + 2000 * TG.fbm(x / 600, z / 600, 625, 2);
    const qz = z + 15000 * TG.fbm(x / 60000 + 4.4, z / 60000 - 1.9, 605, 4) + 9000 * TG.fbm(x / 12000 - 2.2, z / 12000 + 3.1, 607, 3) + 12000 * TG.fbm(x / 5000 + 1.3, z / 5000 - 5.1, 611, 3) + 5000 * TG.fbm(x / 1800 - 0.7, z / 1800 + 2.9, 617, 2) + 2000 * TG.fbm(x / 600 + 3.3, z / 600 - 1.1, 627, 2);
    let b1 = 'default', v1 = 0.08, b2 = 'default', v2 = 0;
    for (const id of zoneIds) {
      const zj = zoneIds.indexOf(id) * 3.7;
      const v = WL.bil(C, ZONE_MASK[id], qx, qz) * (1 + 0.45 * TG.fbm(x / 2600 + zj, z / 2600 - zj, 613, 4) + 0.2 * TG.fbm(x / 700 - zj, z / 700 + zj, 619, 2));
      if (v > v1) { b2 = b1; v2 = v1; b1 = id; v1 = v; } else if (v > v2) { b2 = id; v2 = v; }
    }
    zone1[n] = b1; zone2[n] = b2; zmix[n] = v2 / Math.max(1e-6, v1 + v2);
  }
  // the ground the biomes read: relief against the land ~400 m round (valley < 0 < ridge, m),
  // the slope (deg), the climate's wetness (0..1: flow + near water)
  const blurH = TG.blur(B.h, NB, NB, 8, 2), REL = new Float32Array(B.h.length);
  for (let n = 0; n < REL.length; n++) REL[n] = (B.h[n] - blurH[n]) / VPM;
  const slopeAt = (x, z) => { const g = Math.hypot(groundAt(x + 64, z) - groundAt(x - 64, z), groundAt(x, z + 64) - groundAt(x, z - 64)) / 128; return Math.atan(g) * 180 / Math.PI; };
  // wetness at the biome cell's own scale (the climate's flow term is read on a
  // ~1 km proxy and would print its blocks): the catchment draining through
  // the cell (the 12.8 m flow grid) and the nearness of a river or lake
  const WET = new Float32Array(B.h.length);
  {
    const wm = CL.rasterWater(B, RIVERS, LAKES), dw = CL.distanceField(wm, NB, NB);
    const cellKm2 = (F.cell / VPM / 1000) ** 2;
    for (let n = 0; n < WET.length; n++) {
      const x = B.x0 + (n % NB) * 256, z = B.z0 + ((n / NB) | 0) * 256;
      let A = 0;
      for (const [ox, oz] of [[-64, -64], [64, -64], [-64, 64], [64, 64]]) {
        const i = Math.round((x + ox - F.x0) / F.cell), j = Math.round((z + oz - F.z0) / F.cell);
        if (i >= 0 && j >= 0 && i < F.w && j < F.hh) A = Math.max(A, FLOW.A[j * F.w + i]);
      }
      const wf = clamp(Math.log(Math.max(1e-9, A * cellKm2) / 0.02) / Math.log(500), 0, 1);
      const near = isFinite(dw[n]) ? Math.exp(-dw[n] * 25.6 / 150) : 0;
      WET[n] = Math.max(0.9 * wf, near);
    }
    const Wb = TG.blur(WET, NB, NB, 1, 1);
    WET.set(Wb);
  }
  const tongue = P2(TROUGHS.find(t => t.id === 'u_tongue').pts);
  /** What zone `zone` grows at a cell of ground c = {x, z, m, sl, rel, wet, sd, a, n1, n2}. */
  function propose(zone, c) {
    const {x, z, m, sl, rel, wet, sd, a, n1, n2} = c;
    const valley = sstep(0, 12, -rel), ridge = sstep(0, 12, rel), flat = 1 - sstep(3, 9, sl);
    // the mountains first: the treeline climbs the valleys and drops on the ridges
    const tl = 760 + 110 * n1 + 120 * valley - 90 * ridge;
    if (zone !== 'ashfall' && zone !== 'shardspire') {
      if (m > 1180 + 120 * n1 || (m > 980 + 80 * n1 && sl > 24 && rel > 0)) return 'glacier';
      if (m > tl) return 'alpine';
      if (m > tl - 260 - 80 * n2 && !['heatherhigh', 'redreach', 'steppe', 'goldsea'].includes(zone)) return valley > 0.3 || n2 > -0.1 ? 'pine' : 'alpine';
    }
    switch (zone) {
      case 'hearthvale': return wet > 0.45 + 0.15 * n2 || valley > 0.5 || sl > 9 ? 'forest' : (a === 'forest' && n1 < 0.1 ? 'forest' : 'meadow');
      case 'mirrormere': return sl > 10 || valley > 0.4 + 0.3 * n2 ? (m > 140 + 60 * n1 ? 'pine' : 'forest') : (wet > 0.5 ? 'forest' : 'meadow');
      case 'amberwood': return ridge > 0.5 && flat > 0.5 && n2 > 0.15 ? 'meadow' : (wet > 0.6 && valley > 0.4 ? 'forest' : 'amberwood');
      case 'heatherhigh': return wet > 0.5 && flat > 0.6 ? 'swamp' : (sl > 16 || valley > 0.6 + 0.2 * n2 ? 'pine' : 'moor');
      case 'eldwood': return wet > 0.55 && flat > 0.6 && m < 40 ? 'swamp' : (ridge > 0.6 && n2 > 0.2 ? 'forest' : 'eldwood');
      case 'glimmerfen': {
        // the glow where the water stands (wet, flat, low) and in drifts through the fen; reed swamp round it; carr wood and meadow on the rises
        const fen = (wet - 0.15) * 2 + 0.6 * n2 + 0.35 * n1 - 0.4 * ridge - sstep(13, 18, m);
        return flat > 0.3 && fen > 0.05 ? 'glowfen' : (fen > -0.35 ? 'swamp' : (ridge > 0.3 || n1 > 0.15 ? 'meadow' : 'forest'));
      }
      case 'saltreach': {
        // dunes only right behind the beaches, on low gentle ground; the marsh in the wet flats; heath and wood behind
        const beach = 280 + 260 * n2 + 120 * n1;
        if (sd < beach * VPM && m < 14 && sl < 9) return 'dunes';
        if (wet > 0.3 && flat > 0.6 && m < 7) return 'swamp';
        if (c.zone2 && c.zone2 !== 'saltreach' && sd > (650 + 500 * n1 + 250 * n2) * VPM) return propose(c.zone2, c);
        return valley > 0.3 || wet > 0.4 ? 'forest' : 'meadow';
      }
      case 'teeth': return sl > 26 || rel > 18 ? 'karst' : (wet > 0.5 && m < 6 ? 'swamp' : 'jungle');
      case 'downs': return valley > 0.55 && wet > 0.3 ? 'forest' : (sl > 22 ? 'downs' : (n2 > 0.35 && flat > 0.6 ? 'meadow' : 'downs'));
      case 'gullbay': return m < 4.5 && wet > 0.25 ? 'swamp' : (wet > 0.45 || valley > 0.5 ? 'forest' : (n1 > 0.1 ? 'forest' : 'meadow'));
      case 'steppe': return wet > 0.6 ? 'forest' : (valley > 0.5 && wet > 0.3 ? 'savanna' : 'steppe');
      case 'goldsea': return wet > 0.62 ? 'forest' : (ridge > 0.5 && n2 > 0 ? 'steppe' : 'savanna');
      case 'shardspire': return m > 900 ? 'alpine' : 'crystal_barrens';
      case 'redreach': return sl > 14 || rel > 8 ? 'mesa' : (wet > 0.6 ? 'savanna' : (sd < (600 + 300 * n2) * VPM ? 'desert' : (n1 > 0.1 ? 'desert' : 'mesa')));
      case 'sunscour': {
        const erg = sstep(-0.2, 0.25, TG.fbm(x / 9000, z / 9000, 9 + 11, 3));
        return erg > 0.5 + 0.2 * n2 && sd > 600 * VPM ? 'dunes' : (wet > 0.6 ? 'savanna' : 'desert');
      }
      case 'ashfall': {
        // the cone and the flows are ash and basalt; the old tephra round it greening (heath, scrub) where it is wet
        const rho = Math.hypot(x - V(CINDER.x), z - V(CINDER.z)) / V(CINDER.radius);
        const flow = LAVA_FLOWS.some(F => distToLine(P2(F.pts), x, z) < V(F.hw) * 2.2);
        if (rho < 0.72 + 0.12 * n1 || flow) return 'ashlands';
        return wet > 0.45 || n2 > 0.3 ? 'moor' : (n1 > 0.15 ? 'steppe' : 'ashlands');
      }
      case 'mereisles': return m > 50 ? 'tundra' : (valley > 0.3 ? 'pine' : 'tundra');
      case 'rimeholt': return m < 220 + 60 * n1 && (valley > 0.5 || wet > 0.45) && m > 6 ? 'pine' : 'tundra';
      default: return a === 'ocean' ? 'meadow' : a;
    }
  }
  /** How well a biome sits on this ground, roughly -1..1 (a transition band's tie-break). */
  function suit(b, c) {
    const valley = sstep(0, 12, -c.rel), ridge = sstep(0, 12, c.rel), flat = 1 - sstep(3, 9, c.sl);
    switch (b) {
      case 'forest': case 'eldwood': case 'amberwood': case 'jungle': case 'pine': return 0.7 * c.wet + 0.4 * valley - 0.3 * ridge;
      case 'swamp': case 'glowfen': return 1.1 * c.wet * flat - 0.5 * ridge - 0.3;
      case 'meadow': case 'downs': case 'moor': case 'steppe': case 'savanna': case 'tundra': return 0.35 * ridge + 0.2 * flat - 0.4 * c.wet;
      case 'desert': case 'dunes': case 'mesa': case 'crystal_barrens': case 'ashlands': return 0.3 * ridge - 0.7 * c.wet;
      case 'karst': return c.sl / 40;
      default: return 0;
    }
  }
  for (let n = 0; n < B.h.length; n++) {
    const x = B.x0 + (n % NB) * 256, z = B.z0 + ((n / NB) | 0) * 256, h = B.h[n], m = (h - SEA) / VPM;
    if (h < SEA || lakeAtXZ(x, z) >= 0 && h < LAKES[lakeAtXZ(x, z)].level) { if (h < SEA) out[n] = BI.ocean; continue; }
    const c = {zone2: zone2[n], x, z, m, sl: slopeAt(x, z), rel: REL[n], wet: WET ? WET[n] : 0, sd: coastSD(x, z), a: PALETTE[auto[n]],
               n1: TG.fbm(x / 3000, z / 3000, 77, 3), n2: TG.fbm(x / 1200, z / 1200, 79, 3)};
    let b = propose(zone1[n], c);
    if (zone2[n] !== zone1[n] && zmix[n] > 0.15) {
      // the border band: the second zone takes the cells whose ground suits its biome better
      const b2 = propose(zone2[n], c);
      if (b2 !== b) {
        const t = zmix[n] + 0.45 * (suit(b2, c) - suit(b, c)) + 0.2 * TG.fbm(x / 1500, z / 1500, 629, 3) + 0.12 * TG.fbm(x / 600, z / 600, 621, 3) + 0.06 * TG.fbm(x / 200, z / 200, 623, 2);
        if (t > 0.5) b = b2;
      }
    }
    // special places: the Eldwater's swampy mouth, the delta, the glacier tongue
    if (Math.hypot(x - V(-7.9), z - V(1.15)) < 0.75 * KM * (1 + 0.3 * c.n2) && m < 12 && c.wet > 0.2) b = 'swamp';
    if (Math.hypot(x - V(2.0), z - V(6.3)) < 0.8 * KM * (1 + 0.3 * c.n2) && m < 5 && c.wet > 0.2) b = 'swamp';
    if (distToLine(tongue, x, z) < 2600 * (1 + 0.3 * c.n2) && m < 900 && m > 3) b = 'glacier';
    out[n] = BI[b];
  }
  // no speckles: patches under 12 cells join the neighbour they share the most border with
  mergeSmallPatches(out, NB, 12);
  const zoneOf = zone1;
  // Harrowby and the spawn keep the forest they were in
  for (let n = 0; n < B.h.length; n++) {
    const x = B.x0 + (n % NB) * 256, z = B.z0 + ((n / NB) | 0) * 256;
    if (Math.hypot(x - 700, z - 3500) < 1300 + 400 * TG.fbm(x / 3000, z / 3000, 611, 3)) out[n] = BI.forest;
  }
  // the zones' outlines (the largest patch of each, simplified): the regions[] polygons
  for (const id of zoneIds) {
    const mask = new Uint8Array(B.h.length);
    for (let n = 0; n < mask.length; n++) if (zoneOf[n] === id && B.h[n] >= SEA) mask[n] = 1;
    // the largest 4-connected patch only
    const lab = new Int32Array(mask.length).fill(-1);
    let best = -1, bestN = 0, L = 0;
    for (let n0 = 0; n0 < mask.length; n0++) {
      if (!mask[n0] || lab[n0] >= 0) continue;
      const q = [n0]; lab[n0] = L;
      for (let h = 0; h < q.length; h++) { const c = q[h], ci = c % NB, cj = (c / NB) | 0; for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) { const ii = ci + di, jj = cj + dj; if (ii < 0 || jj < 0 || ii >= NB || jj >= NB) continue; const nb = jj * NB + ii; if (mask[nb] && lab[nb] < 0) { lab[nb] = L; q.push(nb); } } }
      if (q.length > bestN) { bestN = q.length; best = L; }
      L++;
    }
    if (best < 0 || bestN < 20) continue;
    for (let n = 0; n < mask.length; n++) mask[n] = lab[n] === best ? 1 : 0;
    const ring = HY.outline(mask, NB, NB);
    const simp = HY.simplify([...ring, ring[0]], 2.5).slice(0, -1);
    if (simp.length >= 3) ZONE_OUTLINE[id] = simp.map(([a, b]) => [MIN + a * 256, MIN + b * 256]);
  }
  // locks: every land cell whose biome is not what the climate alone picks
  LOCKS = new Uint8Array(B.h.length);
  for (let n = 0; n < out.length; n++) if (out[n] !== auto[n] && B.h[n] >= SEA) LOCKS[n] = 1;
  BIOME = out;
  const counts = {};
  for (const v of out) counts[PALETTE[v]] = (counts[PALETTE[v]] || 0) + 1;
  log('biomes: ' + PALETTE.map(p => `${p} ${counts[p] || 0}`).join(', '));
  const missing = PALETTE.filter(p => !counts[p]);
  if (missing.length) log('BIOMES MISSING: ' + missing.join(', '));
}

/* ===================================================================================
 * 10. PLACES, ROADS, REGIONS
 * =================================================================================== */
// [id, name, icon, x, z (km)]
const MARKERS = [
  ['gullhaven', 'Gullhaven', 'port', 1.35, 7.05], ['mereholm', 'Mereholm', 'village', 2.35, -1.55],
  ['kingsgate', 'Kingsgate', 'castle', -1.6, -6.05], ['rimeholt', 'Rimeholt', 'village', -1.85, -7.45],
  ['oasis_sunscour', 'Oasis of Sunscour', 'camp', 8.1, -2.25], ['redreach_watch', 'Redreach Watch', 'tower', 6.45, 0.35],
  ['cinders_maw', 'Cinder\'s Maw', 'ruin', 6.4, 6.8], ['shardspire', 'The Shardspire', 'tower', 6.0, -3.8],
  ['eldheart', 'Eldheart', 'shrine', -6.55, 0.7], ['glimmer_lights', 'Glimmerfen Lights', 'shrine', -2.9, 4.2],
  ['teeth_caves', 'The Verdant Teeth sea caves', 'cave', -8.1, 8.75], ['whitecliff_light', 'Whitecliff Lighthouse', 'tower', -1.0, 8.6],
  ['hollowcrown', 'Hollowcrown', 'shrine', 0.57, -6.95], ['saltreach_camp', 'Saltreach', 'camp', -8.6, -1.2],
  ['heather_cairn', 'The Tarn Cairn', 'ruin', -5.6, -3.4], ['amberhall', 'Amberhall', 'hut', -2.3, -1.2],
  ['shardwell', 'The Shardwell', 'mine', CRATER.x, CRATER.z - 0.45], ['rimeholt_tongue', 'The Ice Tongue', 'cave', 5.45, -8.2],
  ['mere_isles', 'The Mere Isles', 'ruin', -8.3, -6.0], ['sunfall_camp', 'Sunfall', 'camp', 3.9, -0.8],
  ['goldsea_kraal', 'Goldsea Kraal', 'hut', 4.3, 4.0], ['teeth_ruins', 'The Drowned Gate', 'ruin', -6.7, 7.4],
];
/** The nearest column to (x, z) (voxels) on dry land (>= minM m, no lake) and, for
 *  `flat`, gentler than 1 in 5 over 13 m: a marker never lands in the sea, a road
 *  never starts on a cliff. Searched in rings 32 voxels apart out to 1.6 km. */
function snapLand(x, z, minM = 1.5, flat = false) {
  const ok = (a, b) => groundAt(a, b) >= Y(minM) && lakeAtXZ(a, b) < 0 &&
    (!flat || Math.max(Math.abs(groundAt(a + 64, b) - groundAt(a - 64, b)), Math.abs(groundAt(a, b + 64) - groundAt(a, b - 64))) < 26);
  if (ok(x, z)) return [Math.round(x), Math.round(z)];
  for (let r = 32; r <= 16000; r += 32)
    for (let k = 0, n = Math.max(8, Math.round(r / 24)); k < n; k++) {
      const a = x + r * Math.cos(2 * Math.PI * k / n), b = z + r * Math.sin(2 * Math.PI * k / n);
      if (ok(a, b)) return [Math.round(a), Math.round(b)];
    }
  return [Math.round(x), Math.round(z)];
}
let MK = {};
// the road network: [id, name, kind, surface, waypoints (marker ids or [x, z] km), maxGrade %]
const HB_EAST = [0.32, 0.36];       // where the King's Road passes Harrowby (the village lane meets it)
const ROAD_SPECS = [
  ['road_kings', 'The King\'s Road', 'road', 'packed_gravel', ['gullhaven', [1.0, 4.6], HB_EAST, [1.9, -0.55], 'mereholm', [0.9, -2.75], [-0.5, -2.75], [-0.95, -3.4]], 9],
  ['road_kings_pass', 'The King\'s Road (Kingsgate Pass)', 'road', 'packed_gravel', {zigzag: [[-0.95, -3.4], [-1.55, -5.75]], legs: 7, amp: 0.36, seed: 1, end: 'kingsgate'}, 12],
  ['road_kings_rime', 'The King\'s Road (Rimeholt)', 'road', 'packed_gravel', {zigzag: [[-1.62, -6.25], [-1.75, -7.25]], legs: 9, amp: 0.28, seed: 3, start: 'kingsgate', end: 'rimeholt'}, 12],
  ['road_east', 'The East Road', 'road', 'packed_gravel', [{on: 'road_kings', near: [0.8, 1.6]}, [2.6, 0.9], 'sunfall_camp', [6.1, -1.8], [6.9, -1.82], [7.5, -1.95], 'oasis_sunscour'], 8],
  ['track_west', 'The West Road', 'track', 'packed_earth', [{on: 'road_kings', near: [0.7, -0.35]}, [-0.9, -0.2], 'amberhall', [-4.4, -0.6], 'eldheart', [-7.6, -0.4], 'saltreach_camp'], 12],
  ['track_heather', 'The Tarn Way', 'track', 'packed_earth', [{on: 'track_west', near: [-2.6, -1.0]}, [-3.8, -2.4], 'heather_cairn'], 14],
  ['track_fen', 'The Fen Walk', 'path', 'packed_earth', [{on: 'track_west', near: [-1.2, -0.2]}, [-1.6, 2.3], 'glimmer_lights'], 12],
  ['track_teeth', 'The Teeth Track', 'track', 'packed_earth', ['glimmer_lights', [-4.2, 5.3], [-5.6, 6.0], 'teeth_ruins', 'teeth_caves'], 14],
  ['track_cliff', 'The Clifftop Path', 'path', 'packed_earth', [{on: 'road_kings', near: [0.6, 6.4]}, [0.1, 7.6], 'whitecliff_light', [-2.6, 8.1], [-3.4, 7.2]], 18],
  ['track_gold', 'The Goldsea Track', 'track', 'packed_earth', [{on: 'road_kings', near: [1.4, 5.6]}, 'goldsea_kraal', [5.0, 5.4], [5.98, 6.06]], 14],
  ['track_maw', 'The Maw Path', 'path', 'packed_earth', {zigzag: [[5.98, 6.06], [6.27, 6.55]], legs: 10, amp: 0.28, seed: 5}, 18],
  ['track_shard', 'The Shard Road', 'track', 'packed_earth', [{on: 'road_east', near: [4.2, -1.2]}, [4.55, -2.3]], 13],
  // the Shard Road's climb onto the tableland, to the rim over the Redrun's canyon
  ['track_shard_climb', 'The Shard Road (the Climb)', 'track', 'packed_earth', {zigzag: [[4.55, -2.3], [4.8, -3.2]], legs: 8, amp: 0.22, seed: 7}, 14],
];
const ROADS = [];
function buildRoads() {
  // (the summit shrine stays on the summit: no flat-ground snap for it)
  for (const m of MARKERS) { const p = snapLand(V(m[3]), V(m[4]), 1.5, m[0] !== 'hollowcrown'); m[3] = p[0] / KM; m[4] = p[1] / KM; MK[m[0]] = p; }
  // the cost grid on the design grid's posts: water barred / costly, the sea barred
  const Gr = {w: NC, hh: NC, x0: MIN, z0: MIN, cell: CC, h: new Float32Array(NC * NC)};
  const extra = new Float32Array(NC * NC).fill(1);
  for (let n = 0; n < Gr.h.length; n++) {
    const x = MIN + (n % NC) * CC, z = MIN + ((n / NC) | 0) * CC;
    Gr.h[n] = groundAt(x, z);
    if (Gr.h[n] < SEA + 4) extra[n] = Infinity;
    const lk = lakeAtXZ(x, z);
    if (lk >= 0) extra[n] = Infinity;
  }
  const water = CL.rasterWater({...Gr, w: NC, hh: NC}, RIVERS, []);
  for (let n = 0; n < water.length; n++) if (water[n] && isFinite(extra[n])) extra[n] = 12;   // a ford: allowed, dear
  // Harrowby's own ground: the village's lanes are its own
  for (let n = 0; n < Gr.h.length; n++) {
    const x = MIN + (n % NC) * CC, z = MIN + ((n / NC) | 0) * CC;
    if (x > 0 && x < 1400 && z > 2900 && z < 3900) extra[n] = Infinity;
  }
  const onRoad = new Float32Array(NC * NC);
  for (const [id, name, kind, surface, wps, grade] of ROAD_SPECS) {
    if (!Array.isArray(wps)) {
      // a laid road: hairpins across an axis, then on to its end
      const [[ax, az], [bx, bz]] = wps.zigzag, L = Math.hypot(bx - ax, bz - az), nx = -(bz - az) / L, nz = (bx - ax) / L, pts = [];
      const Rz = TG.rng(wps.seed || 1);
      for (let k = 0; k <= wps.legs; k++) {
        // the hairpins uneven: each turn a little up or down the axis, each reach its own
        const jt = k === 0 || k === wps.legs ? 0 : (Rz() - 0.5) * 0.55 / wps.legs;
        const t = k / wps.legs + jt, side = k === 0 || k === wps.legs ? 0 : (k % 2 ? 1 : -1) * wps.amp * (1 - 0.35 * t) * (0.6 + 0.6 * Rz());
        pts.push([V(ax + (bx - ax) * t + nx * side), V(az + (bz - az) * t + nz * side)]);
      }
      if (wps.start) pts.unshift(MK[wps.start].slice());
      if (wps.end) pts.push(MK[wps.end].slice());
      ROADS.push({id, name, kind, surface, width: kind === 'road' ? 40 : kind === 'track' ? 24 : 12, shoulder: kind === 'road' ? 16 : kind === 'track' ? 12 : 6, maxGrade: grade, depth: 3, smooth: true, points: pts.map(p => [Math.round(p[0]), Math.round(p[1])])});
      log(`road ${id}: laid, ${pts.length} points, ${(WL.polyLength(pts) / KM).toFixed(2)} km`);
      continue;
    }
    const onRoad = w => {
      const R = ROADS.find(r => r.id === w.on), [nx, nz] = [V(w.near[0]), V(w.near[1])];
      let best = R.points[0], bd = Infinity;
      for (const p of R.points.slice(1, -1)) { const d = Math.hypot(p[0] - nx, p[1] - nz); if (d < bd) { bd = d; best = p; } }
      return best.slice();
    };
    const P3 = wps.map(w => (typeof w === 'string' ? MK[w] : w.on ? onRoad(w) : snapLand(V(w[0]), V(w[1]), 2, true)));
    let path = [];
    for (let k = 1; k < P3.length; k++) {
      // the gentlest grade that does not send the road on a long detour (a steep
      // pitch beats a 15 km loop round a canyon); failing that, the shortest found
      let seg = null, segHard = 0;
      const dd = Math.hypot(P3[k][0] - P3[k - 1][0], P3[k][1] - P3[k - 1][1]);
      for (const hard of [1.5, 2.2, 3.5, 8]) {
        const s2 = WL.route(Gr, P3[k - 1], P3[k], {maxGrade: grade / 100, hard, gw: id === 'road_east' ? 0.8 : 2.5, extra, bonus: onRoad});
        if (!s2) continue;
        if (!seg || WL.polyLength(s2) < WL.polyLength(seg)) { seg = s2; segHard = hard; }
        if (WL.polyLength(s2) <= 1.8 * dd + 1500) break;
      }
      if (seg && segHard > 1.5) log(`road ${id}: leg ${k} needs grades to ${(grade * segHard).toFixed(0)}%`);
      if (!seg) { log(`road ${id}: no route ${k - 1} -> ${k}`); continue; }
      { const dd = Math.hypot(P3[k][0] - P3[k - 1][0], P3[k][1] - P3[k - 1][1]), ll = WL.polyLength(seg); if (ll > 2 * dd + 3000) log(`road ${id}: leg ${k} wanders (${(ll / KM).toFixed(1)} km for ${(dd / KM).toFixed(1)} km)`); }
      path = path.concat(k > 1 ? seg.slice(1) : seg);
    }
    if (path.length < 2) continue;
    path[0] = P3[0].slice(); path[path.length - 1] = P3[P3.length - 1].slice();
    for (const p of path) { const i = Math.round((p[0] - MIN) / CC), j = Math.round((p[1] - MIN) / CC); for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) { const n = (j + dj) * NC + i + di; if (n >= 0 && n < onRoad.length) onRoad[n] = 0.7; } }
    const simp = HY.simplify(path, CC * (kind === 'road' ? 2.0 : 2.5)).map(p => [Math.round(p[0]), Math.round(p[1])]);
    const width = kind === 'road' ? 40 : kind === 'track' ? 24 : 12, shoulder = kind === 'road' ? 16 : kind === 'track' ? 12 : 6;
    ROADS.push({id, name, kind, surface, width, shoulder, maxGrade: grade + 2, depth: 3, smooth: true, points: simp});
    log(`road ${id}: ${simp.length} points, ${(WL.polyLength(simp) / KM).toFixed(2)} km`);
  }
  // the lane from the King's Road into Harrowby (to the field gate's end of its own lanes)
  ROADS.push({id: 'path_harrowby_lane', name: 'Harrowby Lane', kind: 'path', surface: 'packed_earth', width: 14, shoulder: 6, maxGrade: 20, depth: 2, smooth: true,
              points: [[560, 3212], [800, 3050], [1500, 3150], [2400, 3450], [V(HB_EAST[0]), V(HB_EAST[1])]]});
}

const REGIONS = [
  ['hearthvale', 'Hearthvale', '#7fbf5a'], ['amberwood', 'Amberwood', '#c8742c'], ['heatherhigh', 'Heatherhigh', '#7d5a7a'],
  ['eldwood', 'The Eldwood', '#1c3b24'], ['glimmerfen', 'Glimmerfen', '#3f8a86'], ['saltreach', 'Saltreach', '#e8d9a0'],
  ['teeth', 'The Verdant Teeth', '#1f8a3c'], ['downs', 'Whitecliff Downs', '#a9d18e'], ['gullbay', 'Gull Bay', '#5f9fc8'],
  ['steppe', 'Sunfall Steppe', '#b7b37a'], ['shardspire', 'The Shardspire', '#b9a3d6'], ['redreach', 'Redreach', '#b5532f'],
  ['sunscour', 'Sunscour', '#d9c27a'], ['goldsea', 'Goldsea', '#c9a74a'], ['ashfall', 'Ashfall', '#4a4440'],
  ['rimeholt', 'Rimeholt', '#d8e6f0'], ['mereisles', 'The Mere Isles', '#6f8f86'], ['mirrormere', 'Mirrormere', '#4f86b8'],
];
const GREYSPINE_REGION = [[-7.9, -4.3], [-4.6, -4.9], [-1.6, -4.6], [1.0, -4.4], [3.6, -4.6], [6.0, -4.9], [8.6, -4.6],
                          [8.6, -6.4], [6.0, -7.4], [3.0, -7.6], [0.4, -7.5], [-2.0, -7.3], [-4.6, -7.3], [-7.2, -6.6]];

/* ===================================================================================
 * WRITE: map.json and the plane tiles
 * =================================================================================== */
function buildJson() {
  const J0 = JSON.parse(readFileSync(join(MAPDIR, 'map.json'), 'utf8'));
  const keepRoads = (J0.roads || []).filter(r => r.id.startsWith('harrowby_'));
  const sites = (J0.sites || []).filter(s => ['spawn', 'harrowby_ground', 'home_lake'].includes(s.id));
  // the small authored waters: Cinder's lava pools, the oasis spring
  sites.push({id: 'cinder_lava_1', kind: 'water', preset: 'lava_pool', at: [V(CINDER.x) - 180, V(CINDER.z) + 120], radius: 90,
              about: 'Cinder\'s Maw: a lava pool on the caldera floor (preset lava_pool).'});
  sites.push({id: 'cinder_lava_2', kind: 'water', preset: 'lava_pool', at: [V(CINDER.x) + 260, V(CINDER.z) - 90], radius: 60});
  sites.push({id: 'cinder_lava_3', kind: 'water', preset: 'lava_pool', at: [V(CINDER.x) + 40, V(CINDER.z) + 330], radius: 45});
  sites.push({id: 'sunscour_spring', kind: 'water', preset: 'oasis', at: [V(OASIS[0]) + 1500, V(OASIS[1]) - 900],
              about: 'A second, smaller spring among the dunes east of the Oasis of Sunscour (preset oasis).'});
  sites.push({id: 'heather_kettle', kind: 'water', preset: 'kettle', at: [V(-6.9), V(-3.15)]});
  sites.push({id: 'rime_tarn', kind: 'water', preset: 'tarn', at: [V(-3.05), V(-7.95)]});
  const regions = REGIONS.map(([id, name, colour]) => ({id: 'region_' + id, name, colour, points: ZONE_OUTLINE[id] || P2(ZONES.find(z => z.id === id).poly)}));
  regions.push({id: 'region_greyspine', name: 'The Greyspine', colour: '#8f959c', points: P2(GREYSPINE_REGION)});
  // the treeline for a world with mountains (it is the snowline too); the range rung halved: the plane
  // now holds the relief down to 6.4 m, and +-12.8 m of noise sank the lowland rivers and the fen under the sea
  const terrain = {...J0.terrain, rangeAmplitude: 96, treeline: Y(1000), treelineJitter: 300};
  const J = {
    version: 2, name: 'default',
    about: 'TIRNHALLOW, the shipped world (docs/WORLD_tirnhallow.md): built by scripts/design_world.mjs from docs/WORLD_brief.md, then the owner\'s to edit on the tuner\'s Environment > World map page. Re-running the script overwrites the planes and the vector layers.',
    extent: J0.extent, siteCellLog2: J0.siteCellLog2, planes: J0.planes, seaLevelY: SEA, edgeFadeVox: J0.edgeFadeVox, warpAmpVox: J0.warpAmpVox,
    terrain, biomes: PALETTE, sites, rules: J0.rules || [], locales: J0.locales,
    roads: [...keepRoads, ...ROADS], rivers: RIVERS, lakes: LAKES, regions,
    markers: MARKERS.map(([id, name, icon, x, z]) => ({id, name, icon, at: [V(x), V(z)]})),
    ranges: RANGE_RECORDS,
    climate: {...CLIMATE, locks: CL.encodeLocks(LOCKS, NB, NB)},
    gen: {template: 'tirnhallow', seed: 1, script: 'scripts/design_world.mjs', ops: [
      {op: 'coast', polygon: COAST.length + ' km knots', islands: ISLANDS.length},
      {op: 'zones', zones: ZONES.map(z => z.id)}, {op: 'valleys', ids: VALLEYS.map(v => v.id)},
      {op: 'erode', strength: 0.45, proxy: 2}, {op: 'ranges', ids: RANGES.map(r => r.id)},
      {op: 'volcano', ...CINDER}, {op: 'troughs', ids: TROUGHS.map(t => t.id)}, {op: 'fjords', ids: FJORDS.map(f => f.id)}, {op: 'lake', ...MERE},
      {op: 'caprock', ids: CAPROCKS.map(k => k.id)}, {op: 'karst', ...KARST}, {op: 'dunes', fields: DUNEFIELDS.length},
    ]},
    editLayer: J0.editLayer,
  };
  return J;
}
/** map.json in the house style: two-space JSON, but numeric arrays of points on one line each. */
function stringify(J) {
  return JSON.stringify(J, null, 2).replace(/\[\s+(-?\d+(?:\.\d+)?)(?:,\s+(-?\d+(?:\.\d+)?))+\s+\]/g, m => '[' + m.slice(1, -1).split(',').map(s => s.trim()).join(', ') + ']') + '\n';
}
function writeTiles(name, planeJ, sampleAt) {
  const PL = MT.makePlane(name, planeJ);
  const dir = join(MAPDIR, 'planes', name);
  mkdirSync(dir, {recursive: true});
  const keep = new Set();
  let written = 0;
  for (let tz = 0; tz < PL.tilesZ; tz++)
    for (let tx = 0; tx < PL.tilesX; tx++) {
      const t = MT.newTile(PL), [sx, sz] = MT.tileSpan(PL, tx, tz);
      for (let j = 0; j < sz; j++) for (let i = 0; i < sx; i++) t[j * MT.TILE + i] = sampleAt(tx * MT.TILE + i, tz * MT.TILE + j);
      if (MT.isDefaultTile(PL, t)) continue;
      const f = `${tx}_${tz}.svt`;
      writeFileSync(join(dir, f), MT.encodeTile(PL, tx, tz, t));
      keep.add(f); written++;
    }
  for (const f of readdirSync(dir)) if (f.endsWith('.svt') && !keep.has(f)) unlinkSync(join(dir, f));
  return written;
}
function writeAll(J) {
  writeFileSync(join(MAPDIR, 'map.json'), stringify(J));
  const nh = writeTiles('height', J, (i, j) => P.h[j * NP + i]);
  const nb = writeTiles('biome', J, (i, j) => BIOME[Math.min(NB - 1, j) * NB + Math.min(NB - 1, i)]);
  log(`wrote map.json, ${nh} height tiles, ${nb} biome tiles`);
}

/* ===================================================================================
 * RENDERS of the whole design: hillshade + biome + water + roads + places
 * =================================================================================== */
function renderWorld(name, view, extra = {}) {
  const pal = PALETTE.map(b => WL.hex(CL.BIOME_COLORS[b] || '#ff00ff'));
  const lines = [];
  for (const r of ROADS) {
    const T = SPL.tessellate(r.points, {step: 64});
    lines.push({pts: T.pts, rgb: r.kind === 'road' ? [150, 40, 30] : r.kind === 'track' ? [120, 70, 40] : [90, 60, 40], width: r.kind === 'road' ? 1 : 0});
  }
  for (const r of RIVERS) {
    const T = SPL.tessellate(r.points.map(p => [p[0], p[1]]), {step: 32});
    const wpx = Math.max(0, Math.round((r.points[r.points.length - 1][2] || 20) / view.res / 2));
    lines.push({pts: T.pts, rgb: [40, 110, 210], width: Math.min(3, wpx)});
  }
  const dots = MARKERS.map(m => ({x: V(m[3]), z: V(m[4]), rgb: [255, 230, 60], r: Math.max(2, Math.round(30 * 256 / view.res / 40))}));
  dots.push({x: 580, z: 3620, rgb: [255, 60, 255], r: 3});
  const R = WL.renderView(view, {
    heightAt: (x, z) => WL.bil(P, P.h, x, z), seaY: SEA, lo: SEA, hi: Y(2000), contour: extra.contour,
    biomeAt: BIOME ? (x, z) => pal[BIOME[clamp(Math.floor((z - MIN) / 256), 0, NB - 1) * NB + clamp(Math.floor((x - MIN) / 256), 0, NB - 1)]] : null,
    biomeAlpha: extra.biomeAlpha == null ? 0.5 : extra.biomeAlpha,
    waterAt: (x, z, h) => { const k = lakeAtXZ(x, z); return k >= 0 && h < LAKES[k].level ? {depth: LAKES[k].level - h} : null; },
    lines, dots,
  });
  log('-> ' + writePNG(join(PNG, name + '.png'), R.img, R.W, R.H));
}

/* ===================================================================================
 * RUN: water, biomes, roads, the files
 * =================================================================================== */
log('8 water');
buildLakes();
fillHollows();
buildFlow();
buildRivers();
makeConfluence();
settleWater();
channelRivers();
harrowbyGround();   // last: nothing after the posts stage (hollow fill, channels) may move the village's ground
checkRivers();
log('9 biomes');
buildBiomes();
log('10 roads, places');
buildRoads();
siteBudget();
renderWorld('world', {x0: MIN, z0: MIN, x1: MIN + SIZE, z1: MIN + SIZE, res: 128});
for (const [k, b] of Object.entries(ZOOMS)) {
  const vb = kmBox(b), span = Math.max(vb[2] - vb[0], vb[3] - vb[1]);
  renderWorld('w_' + k, {x0: vb[0], z0: vb[1], x1: vb[2], z1: vb[3], res: Math.max(16, Math.round(span / 900))}, {contour: 100});
}
const MAPJSON = buildJson();
if (DRY) writeFileSync(join(PNG, 'map.json'), stringify(MAPJSON));
else writeAll(MAPJSON);
log('done');
