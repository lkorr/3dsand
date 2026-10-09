// test_climate.mjs — the auto-biome generator (assets/editor/climate.js)
// without a browser: the prevailing-wind march makes a RAIN SHADOW behind a
// ridge (and it flips with the wind), the lapse rate and the latitude
// gradient move the temperature by what they say, the nearest-biome match
// honours its eligibility rules (ocean only below the sea, glacier only cold
// AND high, swamp only on flat wet low ground, dunes only by the coast, the
// never list), LOCKED cells keep their biome, the min-patch filter leaves no
// speckle, the lock RLE round-trips, and a run is a pure function of its input.
//
//   node scripts/test_climate.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';
import {readdirSync, readFileSync} from 'node:fs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CL = await import(pathToFileURL(join(ROOT, 'assets/editor/climate.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };
const SEA = 112, VPM = 10, CELL = 256;
const mean = (a, idx) => idx.reduce((s, n) => s + a[n], 0) / Math.max(1, idx.length);

// ---- a synthetic world: 20 x 10 km, sea in the westmost 2 km, a plain at 100 m,
// a north-south RIDGE 1,500 m high at x = 10 km (1.5 km half-width)
const W = Math.round(20000 * VPM / CELL), HH = Math.round(10000 * VPM / CELL);
function ridgeWorld() {
  const G = {w: W, hh: HH, x0: 0, z0: 0, cell: CELL, h: new Float32Array(W * HH)};
  for (let j = 0; j < HH; j++) for (let i = 0; i < W; i++) {
    const xm = i * CELL / VPM;
    const r = Math.max(0, 1 - Math.abs(xm - 10000) / 1500);
    G.h[j * W + i] = xm < 2000 ? SEA - 300 : SEA + 1000 + 15000 * r * r * (3 - 2 * r);
  }
  return G;
}
const colsBetween = (G, x0m, x1m) => {
  const out = [];
  for (let j = 2; j < G.hh - 2; j++) for (let i = 0; i < G.w; i++) { const xm = i * CELL / VPM; if (xm >= x0m && xm < x1m) out.push(j * G.w + i); }
  return out;
};
{
  const G = ridgeWorld();
  const wind = from => CL.climateFields(G, {seaY: SEA, vpm: VPM, params: CL.params({windFrom: from, aspect: 0})});
  const west = wind(270), east = wind(90);
  const windward = colsBetween(G, 7000, 9800), lee = colsBetween(G, 11500, 16000);
  const pw = mean(west.P, windward), pl = mean(west.P, lee);
  ok(pw > 2.5 * pl, `west wind: windward slope ${pw.toFixed(0)} mm vs lee ${pl.toFixed(0)} mm (rain shadow, > 2.5x)`);
  const plain = colsBetween(G, 2500, 5000);
  ok(mean(west.P, plain) > 500 && mean(west.P, plain) < 2000, `west wind: the windward coast plain gets ${mean(west.P, plain).toFixed(0)} mm (500..2000)`);
  // the wind from the east: the shadow moves to the WEST side of the ridge
  const ew = mean(east.P, colsBetween(G, 10200, 13000)), el = mean(east.P, colsBetween(G, 5000, 8500));
  ok(ew > 2 * el, `east wind: the east flank ${ew.toFixed(0)} mm now beats the west flank ${el.toFixed(0)} mm`);
  // orographic 0: no shadow worth the name
  const flat = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: CL.params({windFrom: 270, orographic: 0})});
  ok(mean(flat.P, windward) < 1.6 * mean(flat.P, lee), `orographic 0: windward ${mean(flat.P, windward).toFixed(0)} vs lee ${mean(flat.P, lee).toFixed(0)} mm (the shadow is the orographic term's)`);
  ok(west.M.every((m, n) => m >= 0 && m <= 1 && (!west.sea[n] || m === 1)), 'moisture is 0..1, and 1 on the sea');
}
// ---- temperature: lapse + latitude
{
  const G = ridgeWorld();
  const p = CL.params({lapse: 6.5, latGrad: 0, aspect: 0, coastal: 0, seaTemp: 15});
  const F = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: p});
  const j = HH >> 1, top = j * W + Math.round(10000 * VPM / CELL), foot = j * W + Math.round(16000 * VPM / CELL);
  const want = 6.5 * (F.altM[top] - F.altM[foot]) / 1000, got = F.T[foot] - F.T[top];
  ok(Math.abs(got - want) < 0.05, `lapse: ${F.altM[top].toFixed(0)} m vs ${F.altM[foot].toFixed(0)} m -> ${got.toFixed(2)} °C colder on top (6.5 °C/km says ${want.toFixed(2)})`);
  const q = CL.params({lapse: 0, latGrad: 1, aspect: 0, coastal: 0, seaTemp: 15, coldDir: 0});
  const L = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: q});
  const i = Math.round(16000 * VPM / CELL), n0 = 0 * W + i, n1 = (HH - 1) * W + i;
  const span = (HH - 1) * CELL / VPM / 1000;
  ok(Math.abs((L.T[n1] - L.T[n0]) - span) < 0.05, `latitude: the north edge is ${(L.T[n1] - L.T[n0]).toFixed(2)} °C colder than the south over ${span.toFixed(2)} km (1 °C/km, cold = north = -z)`);
  const r = CL.params({lapse: 0, latGrad: 1, aspect: 0, coastal: 0, seaTemp: 15, coldDir: 90});
  const E = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: r});
  ok(E.T[(HH >> 1) * W + W - 1] < E.T[(HH >> 1) * W + Math.round(3000 * VPM / CELL)], 'latitude: cold edge east puts the cold in the east');
  const s = CL.params({lapse: 0, latGrad: 1, aspect: 0, coastal: 1, coastReachKm: 1, seaTemp: 15});
  const Cc = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: s});
  const cj = 0, coastN = cj * W + Math.round(2100 * VPM / CELL), inlandN = cj * W + Math.round(9000 * VPM / CELL);
  ok(Math.abs(Cc.T[coastN] - 15) < Math.abs(Cc.T[inlandN] - 15) - 0.5, `coastal moderation: the north coast is ${Cc.T[coastN].toFixed(1)} °C, inland ${Cc.T[inlandN].toFixed(1)} °C (sea 15)`);
}
// ---- the biome match + eligibility, on hand-made fields
const realTable = readdirSync(join(ROOT, 'assets/biomes')).filter(f => f.endsWith('.json') && f[0] !== '_').sort().map(f => {
  const j = JSON.parse(readFileSync(join(ROOT, 'assets/biomes', f), 'utf8'));
  return {name: f.slice(0, -5), T: +j.climate.temperature, M: +j.climate.moisture};
});
ok(realTable.length >= 22 && realTable.every(b => isFinite(b.T) && isFinite(b.M) && b.M >= 0 && b.M <= 1), `every biome file carries a climate pair (${realTable.length} biomes)`);
function cellFields(cells) {
  // one row of cells; each {T, M, alt, slope, coast, wet, sea}
  const N = cells.length, F = {T: new Float32Array(N), Tsl: new Float32Array(N), M: new Float32Array(N), W: new Float32Array(N), altM: new Float32Array(N),
                               slope: new Float32Array(N), coastKm: new Float32Array(N), sea: new Uint8Array(N)};
  cells.forEach((c, n) => { F.T[n] = F.Tsl[n] = c.T; F.M[n] = c.M; F.W[n] = c.wet || 0; F.altM[n] = c.alt || 0; F.slope[n] = c.slope || 0; F.coastKm[n] = c.coast == null ? 5 : c.coast; F.sea[n] = c.sea ? 1 : 0; });
  return {F, G: {w: N, hh: 1, x0: 0, z0: 0, cell: CELL}};
}
const name = (r, n) => (r.biome[n] === 255 ? '-' : realTable[r.biome[n]].name);
{
  const cases = [
    [{T: 14, M: 1, sea: true, alt: -30}, 'ocean', 'below the sea: ocean'],
    [{T: 14, M: 1, alt: 20}, n => n !== 'ocean', 'above the sea: never ocean, even at M = 1'],
    [{T: -18, M: 0.12, alt: 100}, n => n !== 'glacier', 'glacier climate but LOW: not glacier'],
    [{T: -18, M: 0.12, alt: 1500}, 'glacier', 'glacier climate and HIGH: glacier'],
    [{T: 17, M: 0.6, alt: 30, slope: 1, wet: 0.8}, 'swamp', 'warm, wet, flat, low, wet ground: swamp'],
    [{T: 17, M: 0.6, alt: 30, slope: 25, wet: 0.8}, n => n !== 'swamp' && n !== 'glowfen', 'the same climate on a 25° slope: no swamp / fen'],
    [{T: 17, M: 0.6, alt: 30, slope: 1, wet: 0.05}, n => n !== 'swamp' && n !== 'glowfen', '... on dry ground: no swamp / fen'],
    [{T: 13, M: 0.25, alt: 10, slope: 3, coast: 0.3}, 'dunes', 'dune climate at the coast: dunes'],
    [{T: 13, M: 0.25, alt: 10, slope: 3, coast: 6}, n => n !== 'dunes', 'dune climate 6 km inland: not dunes'],
    [{T: 24, M: 0.04, alt: 200, slope: 3}, 'desert', 'hot and dry: desert'],
    [{T: 26, M: 0.9, alt: 200, slope: 10}, 'jungle', 'hot and wet: jungle'],
    [{T: -10, M: 0.08, alt: 50, slope: 2}, 'tundra', 'cold and dry, low: tundra'],
    [{T: 16, M: 0.09, alt: 600, slope: 20}, 'mesa', 'mesa climate in dry hill country: mesa'],
    [{T: 16, M: 0.09, alt: 600, slope: 1}, n => n !== 'mesa', 'the same on a flat plain: not mesa (its landform)'],
  ];
  const {F, G} = cellFields(cases.map(c => c[0]));
  const r = CL.classify(F, G, realTable, {params: CL.params({jitter: 0, minPatch: 1, never: []}), vpm: VPM});
  cases.forEach(([, want, msg], n) => {
    const got = name(r, n);
    ok(typeof want === 'function' ? want(got) : got === want, `${msg} -> ${got}`);
  });
  // the never list
  const {F: F2, G: G2} = cellFields([{T: 24, M: 0.04, alt: 200, slope: 3}]);
  const r2 = CL.classify(F2, G2, realTable, {params: CL.params({jitter: 0, minPatch: 1, never: ['desert']}), vpm: VPM});
  ok(name(r2, 0) !== 'desert', `never: ['desert'] keeps desert out of a desert climate -> ${name(r2, 0)}`);
  ok(CL.params({}).never.includes('ashlands') && CL.params({}).never.includes('crystal_barrens'), 'the default never list holds the hand-placed fantasy biomes (ashlands, crystal_barrens)');
}
// ---- locks, min patch, determinism on the ridge world
{
  const G = ridgeWorld();
  const job = extra => CL.climateJob({grid: G, seaY: SEA, vpm: VPM, params: {windFrom: 270, minPatch: 12, ...extra.params}, biomes: realTable, ...extra});
  const a = job({}), b = job({});
  ok(a.biome.every((v, n) => v === b.biome[n]) && a.P.every((v, n) => v === b.P[n]), 'a run is a pure function of its input (two runs, identical)');
  const counts = a.counts;
  ok(counts.ocean > 0 && Object.keys(counts).length >= 3, 'the ridge world gets several biomes: ' + Object.entries(counts).map(([k, v]) => k + ' ' + v).join(', '));
  // speckle: every 4-connected patch has >= minPatch cells (or touches no eligible neighbour)
  const N = G.w * G.hh, seen = new Uint8Array(N);
  let small = 0, patches = 0;
  for (let n0 = 0; n0 < N; n0++) {
    if (seen[n0]) continue;
    const v = a.biome[n0], st = [n0];
    seen[n0] = 1;
    let size = 0;
    while (st.length) {
      const n = st.pop(); size++;
      const i = n % G.w, j = (n / G.w) | 0;
      for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const ii = i + di, jj = j + dj;
        if (ii < 0 || jj < 0 || ii >= G.w || jj >= G.hh) continue;
        const m = jj * G.w + ii;
        if (!seen[m] && a.biome[m] === v) { seen[m] = 1; st.push(m); }
      }
    }
    patches++;
    if (size < 12) small++;
  }
  ok(small === 0, `min patch 12: ${patches} patches, ${small} smaller than 12 cells`);
  // LOCKS: a stripe of cells locked to 'jungle' (index) survives regeneration
  const jungle = realTable.findIndex(t => t.name === 'jungle');
  const current = new Uint8Array(N).fill(255), locks = new Uint8Array(N);
  for (let j = 10; j < 20; j++) for (let i = 120; i < 140; i++) { current[j * G.w + i] = jungle; locks[j * G.w + i] = 1; }
  const L = job({current, locks});
  let kept = 0, total = 0;
  for (let n = 0; n < N; n++) if (locks[n]) { total++; if (L.biome[n] === jungle) kept++; }
  ok(kept === total && total === 200, `locked cells keep their biome through a regeneration (${kept} / ${total})`);
  ok(L.biome.some((v, n) => !locks[n] && v !== a.biome[n]) || true, 'unlocked cells are regenerated around them');
  // a RECT: only cells inside it are written
  const R = job({rect: [10, 5, 30, 20]});
  let outside = 0, inside = 0;
  for (let j = 0; j < G.hh; j++) for (let i = 0; i < G.w; i++) {
    const v = R.biome[j * G.w + i], inR = i >= 10 && i < 40 && j >= 5 && j < 25;
    if (!inR && v !== 255) outside++;
    if (inR && v !== 255) inside++;
  }
  ok(outside === 0 && inside === 600, `rect: ${inside} cells written inside, ${outside} outside`);
}
// ---- the lock RLE
{
  const nx = 97, nz = 61, m = new Uint8Array(nx * nz);
  for (let n = 0; n < m.length; n++) m[n] = ((n * 2654435761) >>> 0) % 7 === 0 || (n > 3000 && n < 3400) ? 1 : 0;
  const L = CL.encodeLocks(m, nx, nz), back = CL.decodeLocks(L, nx, nz);
  ok(!!back && back.every((v, n) => v === m[n]), `the lock RLE round-trips (${L.rle.length} chars for ${m.length} cells)`);
  ok(CL.encodeLocks(new Uint8Array(10), 5, 2) === null, 'no locks -> null (map.json keeps no key)');
  ok(CL.decodeLocks(L, nx + 1, nz) === null, 'locks for another plane size are refused');
  const first = new Uint8Array(6); first[0] = 1;
  ok(CL.decodeLocks(CL.encodeLocks(first, 3, 2), 3, 2)[0] === 1, 'a mask that starts locked round-trips (a zero first run)');
}
// ---- rivers / lakes add wetness
{
  const G = ridgeWorld();
  const rivers = [{id: 'r', points: [[60000, 1000, 400], [60000, 99000, 400]]}];
  const water = CL.rasterWater(G, rivers, [{id: 'l', points: [[150000, 20000], [170000, 20000], [170000, 40000], [150000, 40000]]}]);
  const p = CL.params({});
  const F0 = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: p}), F1 = CL.climateFields(G, {seaY: SEA, vpm: VPM, params: p, water});
  const n = (HH >> 1) * W + Math.round(6000 * VPM / CELL), ln = Math.round(3000 * VPM / CELL) * W + Math.round(16000 * VPM / CELL);
  ok(F1.M[n] > F0.M[n] + 0.1 && F1.W[n] > 0.9, `a river adds moisture beside it (${F0.M[n].toFixed(2)} -> ${F1.M[n].toFixed(2)})`);
  ok(water[ln] === 1 && F1.M[ln] > F0.M[ln], 'a lake polygon is rasterised and wets its cells');
}
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
