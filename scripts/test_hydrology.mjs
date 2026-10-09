// test_hydrology.mjs — assets/editor/hydrology.js (docs/PLAN_worldbuilder.md
// K1 item 3) without a browser: Priority-Flood fills to the spill level and
// the epsilon variant drains every flat, D8 + the stack order + accumulation
// conserve area, lakes and lakeAt find the right basins, the river network
// runs downhill and its tributaries join, the stream-power solver is
// deterministic / stable / carves valleys, thermal conserves mass.
//
//   node scripts/test_hydrology.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const HY = await import(pathToFileURL(join(ROOT, 'assets/editor/hydrology.js')).href);
const HS = await import(pathToFileURL(join(ROOT, 'assets/editor/heightshapes.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };
const grid = (w, hh, f) => { const h = new Float32Array(w * hh); for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) h[j * w + i] = f(i, j); return h; };

// ---- Priority-Flood ----
{
  // a bowl (centre 0, rim 100) with a notch at 60 on the east side
  const w = 41, hh = 41;
  const h = grid(w, hh, (i, j) => {
    const r = Math.hypot(i - 20, j - 20);
    let v = Math.min(100, r * 8);
    if (i > 20 && Math.abs(j - 20) <= 1) v = Math.min(v, 60);
    return v;
  });
  const f = HY.priorityFlood(h, w, hh, {epsilon: 0});
  ok(Math.abs(f[20 * w + 20] - 60) < 1e-9, `the bowl fills to its notch: centre ${f[20 * w + 20]} (spill 60)`);
  let raised = 0, lowered = 0;
  for (let n = 0; n < f.length; n++) { if (f[n] > h[n]) raised++; if (f[n] < h[n]) lowered++; }
  ok(lowered === 0 && raised > 0, `filling only raises (${raised} cells raised, none lowered)`);
  const fe = HY.priorityFlood(h, w, hh, {epsilon: 1e-3});
  const mask = HY.outletMask(h, w, hh);
  let flats = 0;
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const n = j * w + i;
      if (mask[n]) continue;
      let lower = false;
      for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) if ((di || dj) && fe[(j + dj) * w + i + di] < fe[n]) lower = true;
      if (!lower) flats++;
    }
  ok(flats === 0, 'the +epsilon fill leaves every inland cell a strictly lower neighbour (D8 drains the lake)');
  // the sea is an outlet: a basin below sea level is NOT filled when seaY says it is sea
  const s = HY.priorityFlood(h, w, hh, {epsilon: 0, seaY: 10});
  ok(s[20 * w + 20] === 0 && s[20 * w + 25] === h[20 * w + 25], 'cells below seaY are outlets (the sea is not filled)');
}

// ---- D8, stack, accumulation ----
{
  const w = 30, hh = 20;
  const h = grid(w, hh, (i, j) => 1000 - i * 10 + Math.abs(j - 10) * 30);   // tilts east, a valley along j = 10
  const F = HY.flow(h, w, hh, 64);
  const pos = new Int32Array(w * hh);
  F.stack.forEach((n, k) => { pos[n] = k; });
  let bad = 0;
  for (let n = 0; n < w * hh; n++) if (F.rcv[n] !== n && pos[F.rcv[n]] > pos[n]) bad++;
  ok(bad === 0 && new Set(F.stack).size === w * hh, 'the stack holds every cell once and every receiver before its donors');
  let atOutlets = 0;
  for (let n = 0; n < w * hh; n++) if (F.rcv[n] === n) atOutlets += F.A[n];
  ok(Math.abs(atOutlets - w * hh) < 1e-9, `accumulation conserves area: ${atOutlets} cells reach the outlets`);
  const valley = F.A[10 * w + w - 2];
  ok(valley > 0.3 * w * hh, `the valley carries most of the map at its mouth (${valley} of ${w * hh} cells)`);
  const M = HY.mfd(h, w, hh, 64);
  ok(M.A[10 * w + w - 2] > 0 && M.twi[10 * w + 15] > M.twi[2 * w + 15], 'MFD: the valley floor is wetter than the slope above it (TWI)');
}

// ---- lakes, lakeAt ----
{
  const w = 60, hh = 40;
  // two basins in a 300 plateau, each drained by a notch channel to the east edge:
  // west floor 50 (rim 150 where its notch leaves), east floor 120 (rim 180)
  const h = grid(w, hh, (i, j) => {
    const a = Math.hypot(i - 15, j - 12), b = Math.hypot(i - 45, j - 30);
    if (a < 10) return 50 + a * 10;
    if (b < 8) return 120 + b * 7.5;
    if (j === 12 && i >= 25) return 150 - (i - 25) * 0.5;
    if (j === 30 && i >= 53) return 180 - (i - 53) * 0.5;
    return 300;
  });
  const L = HY.lakes(h, w, hh, {minDepth: 1});
  const levels = L.lakes.map(l => Math.round(l.level)).sort((a, b) => a - b);
  ok(L.lakes.length === 2 && levels[0] === 150 && levels[1] === 180, `two lakes, spill levels ${levels.join(', ')} (150, 180)`);
  const west = L.lakes.find(l => Math.round(l.level) === 150);
  ok(west && west.maxDepth >= 99 && west.outline.length >= 6, `the west lake: depth ${west && west.maxDepth.toFixed(0)}, outline ${west && west.outline.length} vertices`);
  // the outline encloses the lake's cells: shoelace area ~ cell count
  const area = ring => Math.abs(ring.reduce((s, p, k) => { const q = ring[(k + 1) % ring.length]; return s + p[0] * q[1] - q[0] * p[1]; }, 0)) / 2;
  ok(west && Math.abs(area(west.outline) - west.area) / west.area < 0.2, `outline area ${west && area(west.outline).toFixed(0)} ~ ${west && west.area} cells`);
  const at = HY.lakeAt(h, w, hh, 12 * w + 15);
  ok(at && Math.round(at.level) === 150 && at.area === west.area, 'lakeAt(the west floor) = the same basin, level 150');
  ok(HY.lakeAt(h, w, hh, 2 * w + 2) === null, 'lakeAt on draining ground is null');
  // outline of a plain square
  const sq = new Uint8Array(25); for (const n of [6, 7, 8, 11, 12, 13, 16, 17, 18]) sq[n] = 1;
  const ring = HY.outline(sq, 5, 5);
  ok(ring.length === 4 && area(ring) === 9, 'outline of a 3x3 block: 4 corners, area 9');
}

// ---- rivers ----
{
  // a fan of valleys: the main valley along z = 32 running east, side valleys joining it
  const w = 96, hh = 64;
  const h = grid(w, hh, (i, j) => {
    let v = 3000 - i * 20 + Math.abs(j - 32) * 25;
    v += 200 * Math.abs(Math.sin(i / 6)) * Math.min(1, Math.abs(j - 32) / 10);   // ribs: side valleys
    v += HS.noise2(i / 5, j / 5, 3) * 15;
    return v;
  });
  const R = HY.rivers(h, w, hh, 64, {threshold: 30});
  ok(R.length >= 3, `the network: ${R.length} rivers`);
  const main = R[0];
  ok(main.joins === null && main.points.length > 40, `the main stem: ${main.points.length} cells, no parent`);
  let mono = true;
  for (let k = 1; k < main.points.length; k++) if (main.points[k][2] < main.points[k - 1][2]) mono = false;
  ok(mono, 'accumulation grows source -> mouth along the main stem');
  const trib = R.find(r => r.joins === main.id);
  const lastOnParent = trib && main.points.some(p => p[0] === trib.points[trib.points.length - 1][0] && p[1] === trib.points[trib.points.length - 1][1]);
  ok(!!trib && lastOnParent, `a tributary (${trib && trib.id}) joins ${main.id} and ends ON its channel`);
  ok(R.every(r => r.line.length >= 2 && r.widths.every(x => x >= 8 && x <= 1024)), 'every river has a smoothed line and widths 8..1024');
  // downhill: the raw path never climbs on the filled surface
  const F = HY.flow(h, w, hh, 64);
  let climbs = 0;
  for (const r of R) for (let k = 1; k < r.points.length; k++) {
    const a = r.points[k - 1], b = r.points[k];
    if (F.filled[Math.floor(b[1]) * w + Math.floor(b[0])] > F.filled[Math.floor(a[1]) * w + Math.floor(a[0])] + 1e-6) climbs++;
  }
  ok(climbs === 0, 'no river runs uphill (on the filled surface)');
}

// ---- stream power ----
{
  const w = 80, hh = 80, dx = 256;
  const base = grid(w, hh, (i, j) => Math.max(0, 1 - Math.hypot(i - 40, j - 40) / 36) * 4000 + HS.noise2(i / 4, j / 4, 7) * 30);
  const fixed = new Uint8Array(w * hh);
  for (let n = 0; n < fixed.length; n++) if (base[n] < 5) fixed[n] = 1;
  const U = new Float32Array(w * hh);
  for (let n = 0; n < U.length; n++) U[n] = fixed[n] ? 0 : 20 * base[n] / 4000;
  const run = () => HY.streamPower(Float32Array.from(base), w, hh, dx, {iters: 60, K: 0.02, U, fixed, talusDeg: 40}).h;
  const a = run(), b = run();
  ok(a.every((v, n) => v === b[n]), 'stream power is deterministic (two runs bit-identical)');
  ok([...fixed.keys()].every(n => !fixed[n] || a[n] === base[n]), 'fixed (base-level) cells never move');
  // relief: the valleys are cut -- the spread of heights at a fixed radius grows (spurs + valleys)
  const ringSpread = (g) => { const v = []; for (let t = 0; t < 360; t += 3) { const i = Math.round(40 + 20 * Math.cos(t * Math.PI / 180)), j = Math.round(40 + 20 * Math.sin(t * Math.PI / 180)); v.push(g[j * w + i]); } const m = v.reduce((s, x) => s + x, 0) / v.length; return Math.sqrt(v.reduce((s, x) => s + (x - m) ** 2, 0) / v.length); };
  ok(ringSpread(a) > 3 * ringSpread(base), `valleys and spurs form round the cone: height spread at r = 20 cells ${ringSpread(base).toFixed(0)} -> ${ringSpread(a).toFixed(0)} vox`);
  const {rcv} = HY.flow(a, w, hh, dx, {outlets: fixed});
  let under = 0;
  for (let n = 0; n < a.length; n++) if (rcv[n] !== n && a[n] < a[rcv[n]] - 1e-3) under++;
  ok(under === 0, 'no cell ends below its receiver (the implicit scheme is stable)');
  // thermal conserves mass and lowers the steepest slope
  const t = Float32Array.from(base); t[40 * w + 40] += 3000;
  const s0 = t.reduce((s, x) => s + x, 0);
  HY.thermal(t, w, hh, 64, 35, 30);
  const s1 = t.reduce((s, x) => s + x, 0);
  ok(Math.abs(s1 - s0) / s0 < 1e-6 && t[40 * w + 40] < base[40 * w + 40] + 1500, `thermal: mass conserved (${((s1 - s0) / s0 * 100).toFixed(5)} %), the spike slumped`);
  // the proxy: an identity pass gives the input back; a lowering pass comes back smooth
  const id = HY.viaProxy(base, w, hh, 4, () => {});
  ok(id.every((v, n) => Math.abs(v - base[n]) < 1e-3), 'viaProxy(identity) returns the input');
  const low = HY.viaProxy(base, w, hh, 4, (c) => { for (let n = 0; n < c.length; n++) c[n] -= 100; });
  ok(Math.abs(low[40 * w + 40] - (base[40 * w + 40] - 100)) < 1e-2, 'viaProxy brings the coarse change back to the fine grid');
}

console.log(fail ? `test_hydrology: ${fail} FAILED` : 'test_hydrology: OK');
process.exit(fail ? 1 : 0);
