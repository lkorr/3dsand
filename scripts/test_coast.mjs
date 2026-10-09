// test_coast.mjs — the World map page's COAST tools (assets/editor/mapcoast.js)
// without a browser: wobble frays the waterline but leaves the inland and the
// deep sea alone and does not compound on a second pass; beach lays a low
// band at the waterline; cliff raises the land at the water to the cliff
// height and drops the sea at its foot; smooth shortens a jagged coastline;
// the sea-only mask keeps the land; every op undoes byte-exact.
//
//   node scripts/test_coast.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const MT = await import(pathToFileURL(join(ROOT, 'assets/editor/maptiles.js')).href);
const CO = await import(pathToFileURL(join(ROOT, 'assets/editor/mapcoast.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };
const SEA = 112, VPM = 10;
const json = {
  version: 2, extent: {min: [0, 0], size: [38336, 38336]}, siteCellLog2: 6,
  planes: {height: {log2: 6, tile: 256, default: 200}, biome: {log2: 8, tile: 256, default: 'ocean'}}, biomes: ['ocean'],
};
// 600 x 600 posts, 6.4 m: sea west of post 300 (40 m deep), a gentle beach slope
// up to the band, then a plateau 100 m up from post 360 on
const shore = (i, j, jag) => {
  const c = 300 + (jag ? ((j >> 2) & 1) * 6 : 0);
  if (i < c) return SEA - Math.min(400, (c - i) * 20);
  if (i < c + 60) return SEA + (i - c) * 4;
  return SEA + 1000;
};
function plane(jag = false) {
  const P = MT.makePlane('height', json);
  MT.writeWindow(P, 0, 0, 600, 600, n => shore(n % 600, Math.floor(n / 600), jag), null);
  P.dirty.clear();
  return P;
}
const snap = P => [...P.tiles.entries()].map(([k, t]) => [k, Array.from(t)]);
const same = (a, b) => a.length === b.length && a.every(([k, t], q) => b[q][0] === k && t.every((v, i) => v === b[q][1][i]));
const post = i => i * 64;
const S = (o) => ({...CO.voxOpts({...CO.COAST_DEFAULTS, ...o}, VPM, SEA), mask: null});

{ // WOBBLE
  const P = plane(), pre = snap(P), before = new Map(), stroke = {};
  const o = S({mode: 'wobble', ampM: 60, sizeM: 300, bandM: 20, strength: 100});
  const r = CO.coastDab(P, post(300), post(300), 2000, o, before, stroke);
  let toLand = 0, toSea = 0, inland = 0, deep = 0;
  for (let j = 260; j < 340; j++) for (let i = 250; i < 400; i++) {
    const a = shore(i, j), b = MT.sample(P, i, j);
    if (a < SEA && b >= SEA) toLand++;
    if (a >= SEA && b < SEA) toSea++;
    if (a > SEA + 200 && b !== a) inland++;
    if (a < SEA - 200 && b !== a) deep++;
  }
  ok(r.changed > 0 && toLand > 10 && toSea > 10, `wobble: ${r.changed} posts moved; the waterline frays both ways (${toLand} sea posts became land, ${toSea} land became sea)`);
  ok(inland === 0 && deep === 0, `wobble: the inland plateau and the deep sea are untouched (${inland} / ${deep} moved)`);
  const mid = snap(P);
  const r2 = CO.coastDab(P, post(300), post(300), 2000, o, before, stroke);
  ok(r2.changed === 0 && same(mid, snap(P)), 'wobble: a second pass of the same stroke over the same spot does not compound');
  MT.swapTiles(P, before);
  ok(same(pre, snap(P)), 'wobble: undo restores every tile byte-exact');
}
{ // BEACH over an area
  const P = plane(), before = new Map();
  const o = S({mode: 'beach', widthM: 60, topM: 2, shelfM: 2, bandM: 25, strength: 100});
  const r = CO.coastArea(P, [200, 200, 200, 200], o, before);
  let hi = -Infinity, lo = Infinity, far = 0;
  for (let j = 240; j < 360; j++) {
    for (let i = 300; i < 305; i++) hi = Math.max(hi, MT.sample(P, i, j));      // the first 32 m of beach
    for (let i = 296; i < 300; i++) lo = Math.min(lo, MT.sample(P, i, j));      // the shelf
    for (let i = 380; i < 390; i++) if (MT.sample(P, i, j) !== shore(i, j)) far++;
  }
  ok(r.changed > 0 && hi <= SEA + 2 * VPM + 1 && hi >= SEA, `beach: the band by the water lies ${((hi - SEA) / VPM).toFixed(1)} m above sea at most (top 2 m)`);
  ok(lo >= SEA - 2 * VPM - 1 && lo < SEA, `beach: the shelf off it is no deeper than ${((SEA - lo) / VPM).toFixed(1)} m (shelf 2 m)`);
  ok(far === 0, 'beach: ground beyond the band (inland plateau) is untouched');
}
{ // CLIFF
  const P = plane(), before = new Map(), stroke = {};
  const o = S({mode: 'cliff', cliffM: 30, plateauM: 40, dropM: 8, strength: 100});
  CO.coastDab(P, post(300), post(300), 1500, o, before, stroke);
  const j = 300;
  const landTop = MT.sample(P, 301, j), seaFoot = MT.sample(P, 299, j);
  ok(landTop >= SEA + 0.97 * 30 * VPM, `cliff: the first land post stands ${((landTop - SEA) / VPM).toFixed(1)} m above sea (30 m cliff; the brush falloff at one post off centre)`);
  ok(seaFoot <= SEA - 8 * VPM + 1, `cliff: the sea post at its foot is ${((SEA - seaFoot) / VPM).toFixed(1)} m deep (drop 8 m)`);
  ok(MT.sample(P, 300, j) >= SEA && MT.sample(P, 299, j) < SEA, 'cliff: the waterline stays where it was');
  ok(((landTop - seaFoot) / 64) > 3, `cliff: the face is a wall (${((landTop - seaFoot) / 64).toFixed(1)} : 1 across one post)`);
}
{ // SMOOTH a jagged coast
  const P = plane(true), before = new Map();
  const edges = () => { let e = 0; for (let j = 220; j < 380; j++) for (let i = 280; i < 330; i++) if ((MT.sample(P, i, j) >= SEA) !== (MT.sample(P, i, j + 1) >= SEA)) e++; return e; };
  const e0 = edges();
  CO.coastArea(P, [200, 200, 200, 200], S({mode: 'smooth', smoothM: 40, bandM: 30, strength: 100}), before);
  const e1 = edges();
  ok(e1 < e0 * 0.6, `smooth: the jagged coastline's north-south land/sea edges ${e0} -> ${e1}`);
}
{ // the MASK: sea only never touches a land post
  const P = plane(), before = new Map();
  const o = {...S({mode: 'cliff', cliffM: 30, strength: 100}), mask: {kind: 'sea', seaY: SEA, soft: 1}};
  CO.coastDab(P, post(300), post(300), 1500, o, before, {});
  let landMoved = 0, seaMoved = 0;
  for (let j = 270; j < 330; j++) for (let i = 280; i < 320; i++) {
    const a = shore(i, j), b = MT.sample(P, i, j);
    if (a >= SEA + 10 && b !== a) landMoved++;
    if (a < SEA && b !== a) seaMoved++;
  }
  ok(landMoved === 0 && seaMoved > 0, `mask sea-only: ${seaMoved} sea posts dropped, ${landMoved} land posts moved`);
}
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
