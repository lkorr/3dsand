// test_terraingen.mjs — assets/editor/terraingen.js (docs/PLAN_worldbuilder.md
// K1 item 4) without a browser: the range generator (only raises, reaches its
// crest heights, stays inside its reach, deterministic for a seed, erosion
// cuts valleys), the bake at post resolution, every template and primitive,
// the terrace formula, whole-land erosion. To LOOK at the output:
// node scripts/render_terrain_preview.mjs (PNGs under build/terrain_preview/).
//
//   node scripts/test_terraingen.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const TG = await import(pathToFileURL(join(ROOT, 'assets/editor/terraingen.js')).href);
const HY = await import(pathToFileURL(join(ROOT, 'assets/editor/hydrology.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };
const SEA = 112, M = 10;
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const t0 = () => performance.now(), sec = t => ((performance.now() - t) / 1000).toFixed(2) + ' s';

// ---- noise + terrace ----
{
  let lo = Infinity, hi = -Infinity;
  for (let k = 0; k < 4000; k++) { const v = TG.ridged(k * 0.137, k * 0.071, 5); lo = Math.min(lo, v); hi = Math.max(hi, v); }
  ok(lo >= 0 && hi <= 1.2 && hi > 0.6, `ridged multifractal in 0..1.2 (${lo.toFixed(2)}..${hi.toFixed(2)})`);
  ok(TG.ridged(1.3, 2.7, 9) === TG.ridged(1.3, 2.7, 9) && TG.ridged(1.3, 2.7, 9) !== TG.ridged(1.3, 2.7, 10), 'ridged is a pure function of (x, z, seed)');
  ok(Math.abs(TG.terraceH(150, 0, 100, 1) - 150) < 1e-9 && TG.terraceH(130, 0, 100, 4) < 101 && Math.abs(TG.terraceH(200, 0, 100, 3) - 200) < 1e-9,
     'terrace: h\' = step (floor(x) + smoothstep(frac)^sharp): midpoint kept at sharp 1, a shelf at sharp 4, band edges exact');
  const [wx, wz] = TG.warp(1, 2, 3, 0.5);
  ok((wx !== 1 || wz !== 2) && Math.hypot(wx - 1, wz - 2) < 1.5, 'domain warp moves the point, by at most about its amplitude');
}

// a 9 x 6 km plain at ~120 m, 51.2 m cells (fast), and a range over it
const plain = (wm, hm, cell) => {
  const w = Math.round(wm * M / cell), hh = Math.round(hm * M / cell), G = TG.makeGrid(w, hh, 0, 0, cell);
  for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) G.h[j * w + i] = SEA + 1200 + 150 * TG.fbm(i * cell / 20000, j * cell / 20000, 5, 3);
  return G;
};
const RANGE = {id: 'r', profile: 'alpine', seed: 3, peaks: 0.6, spurs: 0.6, foothills: 0.5, erode: 0.7,
  points: [[15000, 30000, SEA + 13000, 12000], [45000, 26000, SEA + 15000, 14000], [75000, 32000, SEA + 12000, 11000]]};

// ---- the range ----
{
  const G = plain(9000, 6000, 512);
  let t = t0();
  const E = TG.rangeField({...RANGE, erode: 0}, G, {seaY: SEA});
  const tE = sec(t); t = t0();
  const R = TG.rangeField(RANGE, G, {seaY: SEA});
  const tR = sec(t);
  ok(E.h.every((v, n) => v >= G.h[n]) && R.h.every((v, n) => v >= G.h[n]), `a range only raises (envelope ${tE}, eroded ${tR} on ${G.w}x${G.hh} cells of 51.2 m)`);
  const near = (H, x, z, r) => { let m = -Infinity; for (let j = 0; j < G.hh; j++) for (let i = 0; i < G.w; i++) if (Math.hypot(TG.gx(G, i) - x, TG.gz(G, j) - z) < r) m = Math.max(m, H[j * G.w + i]); return m; };
  const crest = near(R.h, 45000, 26000, 4000), want = SEA + 15000;
  ok(Math.abs(crest - want) / (want - G.h[0]) < 0.2, `the eroded crest near the middle knot reaches its height: ${((crest - SEA) / M).toFixed(0)} m (knot ${((want - SEA) / M).toFixed(0)} m)`);
  const box = TG.rangeBox(RANGE);
  let outside = 0;
  for (let j = 0; j < G.hh; j++) for (let i = 0; i < G.w; i++) {
    const x = TG.gx(G, i), z = TG.gz(G, j);
    if ((x < box[0] || x > box[2] || z < box[1] || z > box[3]) && R.h[j * G.w + i] !== G.h[j * G.w + i]) outside++;
  }
  ok(outside === 0, 'nothing outside rangeBox() moves');
  const R2 = TG.rangeField(RANGE, G, {seaY: SEA}), R3 = TG.rangeField({...RANGE, seed: 4}, G, {seaY: SEA});
  ok(same(R.h, R2.h) && !same(R.h, R3.h), 'deterministic for a seed; another seed is another range');
  // erosion cuts valleys: along a line across the flank, the eroded profile is rougher than the envelope's
  const rough = H => { let s = 0; const j = Math.round(34000 / G.cell); for (let i = 20; i < G.w - 21; i++) s += Math.abs(H[j * G.w + i + 1] - 2 * H[j * G.w + i] + H[j * G.w + i - 1]); return s; };
  ok(rough(R.h) > 1.5 * rough(E.h), `erosion cuts valleys and spurs into the flank (roughness ${rough(E.h).toFixed(0)} -> ${rough(R.h).toFixed(0)})`);
  // the drainage of the eroded range reaches the plain: no closed pits inside it deeper than a few metres
  const F = HY.priorityFlood(R.h, G.w, G.hh, {epsilon: 0});
  let pit = 0;
  for (let n = 0; n < F.length; n++) if (R.mask[n] && F[n] - R.h[n] > 30) pit++;
  ok(pit < R.mask.reduce((a, b) => a + b, 0) * 0.01, `the eroded range drains (${pit} cells in pits deeper than 3 m)`);
  ok(TG.RANGE_PROFILE_NAMES.join() === 'alpine,sharp,round,massif', 'profiles: ' + TG.RANGE_PROFILE_NAMES.join(', '));
}

// ---- the bake at post resolution ----
{
  const G = plain(5000, 4000, 64);
  const r = {...RANGE, points: [[8000, 20000, SEA + 9000, 7000], [42000, 18000, SEA + 10000, 8000]]};
  const t = t0();
  const B = TG.rangeBake(r, G, {seaY: SEA});
  ok(B.factor === 4 && B.proxy.cell === 256, `the bake runs on a 25.6 m proxy (factor ${B.factor}) and comes back to the 6.4 m posts (${sec(t)})`);
  let changed = 0, lower = 0;
  for (let n = 0; n < B.h.length; n++) { if (B.changed[n]) changed++; if (B.h[n] < G.h[n]) lower++; }
  ok(changed > 0 && lower === 0, `${changed} posts raised, none lowered`);
  ok(TG.proxyFactor(3137, 3137, 64) === 8 && TG.proxyFactor(400, 400, 64) === 4, 'proxyFactor: the whole 20 km map goes to 51.2 m, a small window to 25.6 m');
}

// ---- templates ----
for (const name of TG.TEMPLATE_NAMES) {
  const n = 129, G = TG.makeGrid(n, n, -100352, -100352, 1568);
  const t = t0();
  TG.runTemplate(G, TG.TEMPLATES[name], {seed: 5, seaY: SEA, vpm: M});
  const G2 = TG.makeGrid(n, n, -100352, -100352, 1568);
  TG.runTemplate(G2, TG.TEMPLATES[name], {seed: 5, seaY: SEA, vpm: M});
  let land = 0, hi = -Infinity;
  for (const v of G.h) { if (v > SEA) land++; hi = Math.max(hi, v); }
  const frac = land / G.h.length;
  const expectSea = name !== 'highlands';
  ok(same(G.h, G2.h) && frac > 0.04 && (!expectSea || frac < 0.9) && (hi - SEA) / M > 200,
     `template ${name}: deterministic, ${(frac * 100).toFixed(0)} % land, top ${((hi - SEA) / M).toFixed(0)} m (${sec(t)})`);
}
{
  const G = TG.makeGrid(65, 65, 0, 0, 1000);
  let threw = '';
  try { TG.runTemplate(G, [{op: 'nonsense'}], {seed: 1, seaY: SEA}); } catch (e) { threw = e.message; }
  ok(/unknown template op/.test(threw), 'an unknown op is refused: ' + threw);
  ok(TG.TEMPLATE_OPS.includes('range') && TG.TEMPLATE_OPS.includes('strait') && TG.TEMPLATE_OPS.includes('mask'), 'ops: ' + TG.TEMPLATE_OPS.join(' '));
}

// ---- primitives ----
{
  const flat = () => { const G = TG.makeGrid(160, 120, 0, 0, 128); G.h.fill(SEA + 1500); return G; };
  const at = (G, x, z) => TG.sampleGrid(G, x, z);
  let G = TG.crater(flat(), {x: 10000, z: 7500, radius: 4000, depth: 800, seed: 2});
  ok(at(G, 10000, 7500) < SEA + 800 && at(G, 14000, 7500) > SEA + 1550 && at(G, 1000, 1000) === SEA + 1500, 'crater: a floor below the ground, a rim above it, nothing far away');
  G = TG.volcano(flat(), {x: 10000, z: 7500, radius: 6000, height: 3000, seed: 2});
  ok(at(G, 10000 + 1500, 7500) > SEA + 3000 && at(G, 10000, 7500) < at(G, 10000 + 900, 7500), 'volcano: a tall cone with a summit crater');
  G = TG.canyon(flat(), {box: [1000, 5000, 19000, 10000], width: 1500, depth: 600, seed: 2});
  let mn = Infinity; for (const v of G.h) mn = Math.min(mn, v);
  ok(mn < SEA + 950 && at(G, 10000, 500) === SEA + 1500, `canyon: cut ${((SEA + 1500 - mn) / M).toFixed(0)} m deep, the land beside it untouched`);
  G = TG.uValley(flat(), {box: [1000, 5000, 19000, 10000], width: 2000, depth: 700, fjord: true, seaY: SEA, seed: 2});
  mn = Infinity; for (const v of G.h) mn = Math.min(mn, v);
  ok(mn < SEA - 600, `fjord: the trough floor is dug below the sea (${((mn - SEA) / M).toFixed(0)} m)`);
  G = TG.mesaField(flat(), {box: [0, 0, 20480, 15360], height: 1000, size: 1500, density: 0.8, seed: 2});
  let mx = -Infinity; for (const v of G.h) mx = Math.max(mx, v);
  ok(mx > SEA + 2300 && G.h.every(v => v >= SEA + 1500), 'mesas: flat tops ~100 m up, nothing dug');
  G = TG.dunes(flat(), {box: [2000, 2000, 18000, 13000], height: 150, wavelength: 1200, wind: 30, seed: 2});
  ok(at(G, 500, 500) === SEA + 1500 && G.h.some(v => v > SEA + 1600), 'dunes: inside the box only');
  G = flat();
  TG.edgeMask(G, {power: 2, seaY: SEA, depth: 800, seed: 2});
  ok(G.h[0] < SEA && G.h[60 * 160 + 80] > SEA + 1000, 'edgeMask: the edge drowns, the middle stays');
}

// ---- erosion of land ----
{
  const n = 129, G = TG.makeGrid(n, n, -100352, -100352, 1568);
  TG.runTemplate(G, TG.TEMPLATES.island.filter(o => o.op !== 'erode'), {seed: 5, seaY: SEA, vpm: M});
  const before = Float32Array.from(G.h);
  let hb = -Infinity; for (const v of before) hb = Math.max(hb, v);
  TG.erodeLand(G, {strength: 0.8, seaY: SEA, seed: 3});
  let ha = -Infinity, seaMoved = 0;
  for (let k = 0; k < G.h.length; k++) { ha = Math.max(ha, G.h[k]); if (before[k] <= SEA && G.h[k] !== before[k]) seaMoved++; }
  ok(Math.abs(ha - hb) / (hb - SEA) < 0.15 && seaMoved === 0, `erodeLand keeps the high points (${((hb - SEA) / M).toFixed(0)} -> ${((ha - SEA) / M).toFixed(0)} m) and leaves the sea floor alone`);
}

console.log(fail ? `test_terraingen: ${fail} FAILED` : 'test_terraingen: OK');
process.exit(fail ? 1 : 0);
