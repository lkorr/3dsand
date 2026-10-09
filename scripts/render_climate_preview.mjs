// render_climate_preview.mjs — LOOK at the auto-biome generator
// (assets/editor/climate.js) without a browser: a template continent the
// size of the default map (terraingen.js), its climate (temperature, the
// prevailing-wind precipitation with its rain shadow, wetness) and the biomes
// the generator assigns from every assets/biomes/*.json climate pair, drawn
// hillshaded in the map page's biome colours.
//
//   node scripts/render_climate_preview.mjs [--template continent] [--seed 7]
//        [--wind 270] [--out DIR] [--cell 256] [--no-plains] [key=value ...]
//     key=value: any climate input (seaTemp=16 latGrad=0.6 jitter=0.3 ...)
//   -> build/climate_preview/{biomes,temperature,precip,moisture}.png
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';
import {readdirSync, readFileSync} from 'node:fs';
import {writePNG} from './pngout.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const imp = p => import(pathToFileURL(join(ROOT, p)).href);
const TG = await imp('assets/editor/terraingen.js');
const HY = await imp('assets/editor/hydrology.js');
const CL = await imp('assets/editor/climate.js');

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const OUT = opt('--out', join(ROOT, 'build', 'climate_preview'));
const SEA = 112, VPM = 10, CELL = +opt('--cell', 256), SIZE = 200704;
const kv = {};
for (const a of args) { const m = /^(\w+)=(.+)$/.exec(a); if (m) kv[m[1]] = isNaN(+m[2]) ? m[2] : +m[2]; }
if (opt('--wind')) kv.windFrom = +opt('--wind');

// the biome table: every assets/biomes/*.json climate pair
const table = readdirSync(join(ROOT, 'assets/biomes')).filter(f => f.endsWith('.json') && f[0] !== '_').sort().map(f => {
  const j = JSON.parse(readFileSync(join(ROOT, 'assets/biomes', f), 'utf8'));
  return {name: f.slice(0, -5), T: +j.climate.temperature, M: +j.climate.moisture};
});

// the land: the template at 51.2 m, brought to the biome cells (bicubic + relief detail)
const t0 = performance.now();
const tpl = opt('--template', 'continent'), seed = +opt('--seed', 7);
const fc = 512, nc = Math.round(SIZE / fc) + 1;
const Gc = TG.makeGrid(nc, nc, -SIZE / 2, -SIZE / 2, fc);
TG.runTemplate(Gc, TG.TEMPLATES[tpl], {seed, seaY: SEA, vpm: VPM});
const f = fc / CELL, n = Math.round(SIZE / CELL) + 1;
const up = HY.upsample(Gc.h, nc, nc, f, n, n);
const G = {w: n, hh: n, x0: -SIZE / 2 + CELL / 2, z0: -SIZE / 2 + CELL / 2, cell: CELL, h: up};
for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) {
  const k = j * n + i, rel = up[k] - SEA;
  if (rel > 1) up[k] += TG.reliefDetail(G.x0 + i * CELL, G.z0 + j * CELL, Math.min(rel, 4000), seed * 7 + 3, 0.6);
}
// COASTAL PLAINS (unless --no-plains): the templates make rugged land right down
// to the sea; a real continent has flat lowlands (deltas, marsh, the plains a
// swamp needs). The relief below ~120 m is compressed toward a gentle plain.
if (!args.includes('--no-plains')) {
  const sm = TG.blur(up, n, n, 6, 3);
  for (let k = 0; k < n * n; k++) {
    const rel = (sm[k] - SEA) / VPM;
    if (rel <= 0 || rel > 160) continue;
    const t = TG.smoothstep(60, 160, rel), plain = SEA + (sm[k] - SEA) * 0.45;
    up[k] = plain + (up[k] - plain) * t;
  }
}
console.log(`land: ${tpl} seed ${seed}, ${n}x${n} cells of ${CELL / VPM} m (${((performance.now() - t0) / 1000).toFixed(1)} s)`);

const t1 = performance.now();
const res = CL.climateJob({grid: G, seaY: SEA, vpm: VPM, params: kv, biomes: table, classify: true});
console.log(`climate + biomes: ${((performance.now() - t1) / 1000).toFixed(1)} s (proxy x${res.proxy})`);
const total = Object.values(res.counts).reduce((a, b) => a + b, 0);
console.log(Object.entries(res.counts).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k} ${(100 * v / total).toFixed(1)}%`).join(', '));

const hex = s => [1, 3, 5].map(i => parseInt(s.slice(i, i + 2), 16));
const shadeOf = () => TG.shade(G.h, n, n, CELL, {seaY: SEA, zf: 2, tint: 0});
function blend(colOf, name, alpha = 0.72) {
  const rgba = shadeOf();
  for (let k = 0; k < n * n; k++) {
    const c = colOf(k);
    if (!c) continue;
    const o = k * 4, g = rgba[o] / 255;
    for (let q = 0; q < 3; q++) rgba[o + q] = (1 - alpha) * rgba[o + q] + alpha * c[q] * (0.45 + 0.75 * g);
  }
  console.log('-> ' + writePNG(join(OUT, name + '.png'), rgba, n, n));
}
const pal = table.map(b => hex(CL.BIOME_COLORS[b.name] || '#ff00ff'));
blend(k => pal[res.biome[k]], 'biomes', 0.8);
const ramp = (stops, t) => {
  t = Math.max(0, Math.min(1, t));
  const s = t * (stops.length - 1), i = Math.min(stops.length - 2, Math.floor(s)), u = s - i;
  return stops[i].map((v, q) => v + (stops[i + 1][q] - v) * u);
};
const TEMP = [[40, 60, 200], [120, 180, 240], [240, 240, 240], [250, 200, 80], [210, 50, 30]];
const WET = [[150, 100, 50], [220, 200, 90], [90, 180, 70], [30, 120, 200], [20, 40, 160]];
blend(k => ramp(TEMP, (res.T[k] + 20) / 50), 'temperature');
blend(k => ramp(WET, res.P[k] / 3000), 'precip');
blend(k => ramp(WET, res.M[k]), 'moisture');
