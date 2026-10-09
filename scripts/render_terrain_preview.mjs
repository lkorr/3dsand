// render_terrain_preview.mjs — hillshaded PNGs of the terrain generators
// (assets/editor/terraingen.js + hydrology.js), to LOOK at what they make
// without a browser: a mountain range from a few spline knots (designed
// envelope, then eroded), the continent / island / archipelago templates, the
// primitives, a whole-map stream-power pass. Satellite-DEM style: Horn
// hillshade from the NW over a muted hypsometric tint.
//
//   node scripts/render_terrain_preview.mjs [what ...] [--out DIR] [--cell VOX]
//     what: range range-noerode range-knots continent island archipelago
//           primitives erode-map all (default: all)
//   -> build/terrain_preview/<what>.png (DIR overrides)
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';
import {writePNG} from './pngout.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const TG = await import(pathToFileURL(join(ROOT, 'assets/editor/terraingen.js')).href);
const HY = await import(pathToFileURL(join(ROOT, 'assets/editor/hydrology.js')).href);

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const OUT = opt('--out', join(ROOT, 'build', 'terrain_preview'));
const CELL = +opt('--cell', 256);
const want = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));
const all = !want.length || want.includes('all');
const SEA = 112, M = 10;   // sea level y, voxels per metre

function save(name, G, extra = {}) {
  const rgba = TG.shade(G.h, G.w, G.hh, G.cell, {seaY: SEA, zf: extra.zf || 1, tint: extra.tint, lo: extra.lo, hi: extra.hi});
  if (extra.overlay) extra.overlay(rgba);
  const p = writePNG(join(OUT, name + '.png'), rgba, G.w, G.hh);
  let lo = Infinity, hi = -Infinity;
  for (const v of G.h) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
  console.log(`${name}: ${G.w}x${G.hh} cells of ${(G.cell / M).toFixed(1)} m, ${((lo - SEA) / M).toFixed(0)}..${((hi - SEA) / M).toFixed(0)} m above sea -> ${p}`);
}
const t0 = () => performance.now();
const ms = t => ((performance.now() - t) / 1000).toFixed(2) + ' s';

// a gently rolling plain at ~120 m, the ground a range is drawn onto
function plain(wkm, hkm, cell = CELL) {
  const w = Math.round(wkm * 1000 * M / cell), hh = Math.round(hkm * 1000 * M / cell);
  const G = TG.makeGrid(w, hh, 0, 0, cell);
  for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++)
    G.h[j * w + i] = SEA + 1200 + 300 * TG.fbm(i * cell / 30000, j * cell / 30000, 5, 4);
  return G;
}
// THE RANGE the owner draws: five clicks, crest 1,500-1,900 m, 3 km half-width
const RANGE = {
  id: 'range_test', profile: 'alpine', seed: 3, peaks: 0.65, spurs: 0.7, foothills: 0.6, erode: 0.7, wander: 0.5,
  points: [[25000, 90000, SEA + 14000, 22000], [55000, 62000, SEA + 17500, 30000], [92000, 58000, SEA + 19000, 32000],
           [125000, 70000, SEA + 16500, 28000], [150000, 50000, SEA + 12500, 20000]],
};
function knotsOverlay(G, r) {
  return rgba => {
    for (const p of r.points) {
      const i = Math.round((p[0] - G.x0) / G.cell), j = Math.round((p[1] - G.z0) / G.cell);
      for (let dj = -2; dj <= 2; dj++) for (let di = -2; di <= 2; di++) {
        const ii = i + di, jj = j + dj;
        if (ii < 0 || jj < 0 || ii >= G.w || jj >= G.hh) continue;
        const o = (jj * G.w + ii) * 4;
        rgba[o] = 255; rgba[o + 1] = 60; rgba[o + 2] = 40; rgba[o + 3] = 255;
      }
    }
  };
}

// THE TEMPLATES: a 20 km map (the default map's extent) at 51.2 m
for (const name of TG.TEMPLATE_NAMES) {
  if (!(all || want.includes(name) || want.includes('templates'))) continue;
  const cell = +opt('--tcell', 512), n = Math.round(200704 / cell) + 1;
  const G = TG.makeGrid(n, n, -100352, -100352, cell);
  const t = t0();
  TG.runTemplate(G, TG.TEMPLATES[name], {seed: +opt('--seed', 7), seaY: SEA, vpm: M});
  console.log(`template ${name}: ${ms(t)}`);
  save('template_' + name, G);
}
// THE PRIMITIVES: one 8 x 6 km panel each on a 150 m plain
if (all || want.includes('primitives')) {
  const cell = 128, w = Math.round(8000 * M / cell), hh = Math.round(6000 * M / cell);
  const flat = () => { const G = TG.makeGrid(w, hh, 0, 0, cell); for (let n = 0; n < G.h.length; n++) G.h[n] = SEA + 1500 + 40 * TG.fbm((n % w) * cell / 8000, Math.floor(n / w) * cell / 8000, 9, 4); return G; };
  const box = [8000, 8000, 72000, 52000];
  const P = {
    crater: G => TG.crater(G, {x: 40000, z: 30000, radius: 14000, depth: 2500, seed: 4}),
    volcano: G => TG.volcano(G, {x: 40000, z: 30000, radius: 26000, height: 9000, seed: 4}),
    canyon: G => TG.canyon(G, {box, width: 6000, depth: 1300, steps: 4, seed: 4}),
    glacial: G => { for (let n = 0; n < G.h.length; n++) G.h[n] += 7000 * TG.ridged((n % w) * cell / 9000, Math.floor(n / w) * cell / 9000, 5, 5); return TG.uValley(G, {box, width: 7000, depth: 2500, seed: 4}); },
    fjord: G => { for (let n = 0; n < G.h.length; n++) G.h[n] += 4000; return TG.uValley(G, {box: [0, 26000, 80000, 34000], width: 7000, depth: 2500, fjord: true, seaY: SEA, seed: 4}); },
    mesas: G => TG.mesaField(G, {box: [0, 0, 80000, 60000], height: 2000, size: 5000, density: 0.6, seed: 4}),
    dunes: G => TG.dunes(G, {box: [0, 0, 80000, 60000], height: 250, wavelength: 3500, wind: 20, seed: 4}),
  };
  for (const [k, f] of Object.entries(P)) { const G = flat(); f(G); save('primitive_' + k, G); }
}
// A WHOLE-MAP stream-power pass over the island template (the Erode panel's global pass)
if (all || want.includes('erode-map')) {
  const cell = 512, n = Math.round(200704 / cell) + 1;
  const G = TG.makeGrid(n, n, -100352, -100352, cell);
  const ops = TG.TEMPLATES.island.filter(o => o.op !== 'erode');
  TG.runTemplate(G, ops, {seed: 7, seaY: SEA, vpm: M});
  save('erode_map_before', G);
  const t = t0();
  TG.erodeLand(G, {strength: 0.8, seaY: SEA, seed: 3});
  console.log('erode-map: ' + ms(t));
  save('erode_map_after', G);
}

if (all || want.includes('range-noerode') || want.includes('range') || want.includes('range-knots')) {
  const G = plain(17.5, 12.5);
  if (all || want.includes('range-noerode')) {
    let t = t0();
    const R = TG.rangeField({...RANGE, erode: 0}, G, {seaY: SEA});
    console.log('range (designed envelope, no erosion): ' + ms(t));
    save('range_noerode', {...G, h: R.h});
  }
  if (all || want.includes('range') || want.includes('range-knots')) {
    let t = t0();
    const R = TG.rangeField(RANGE, G, {seaY: SEA, progress: null});
    console.log('range (eroded): ' + ms(t));
    save('range', {...G, h: R.h});
    if (all || want.includes('range-knots')) save('range_knots', {...G, h: R.h}, {overlay: knotsOverlay(G, RANGE)});
  }
}
