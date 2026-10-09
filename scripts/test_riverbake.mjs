// test_riverbake.mjs — assets/editor/riverbake.js (the JS port of the
// engine's river / lake bake, docs/PLAN_worldbuilder.md E2 item 1) against
// the ENGINE's own output, plus the pure logic of the River / Lake tools.
//
//   node scripts/test_riverbake.mjs
//
// 1. tests/river_vectors.json (scripts/gen_river_vectors.py: `sandvox
//    --mapcheck` on a copy of the peaks map) holds the engine's bake of six
//    rivers and two lakes with the bare ground at every sample. The port is
//    fed that ground and must reproduce every sample's arc and water surface,
//    the reaches, pieces, waterfalls, drain level and lakes EXACTLY.
// 2. Hand-derivable cases (flat / staircase / lake / sea), so a reader can
//    check the rules without the engine.
// 3. The lake tool (HY.lakeAt -> a lakes[] record), the sill warning, the
//    meander, derive -> records.
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';
import {readFileSync} from 'node:fs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const RB = await import(pathToFileURL(join(ROOT, 'assets/editor/riverbake.js')).href);
const HY = await import(pathToFileURL(join(ROOT, 'assets/editor/hydrology.js')).href);
const WT = RB;

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };
const eqArr = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

/* ---- 1. the engine's table --------------------------------------------------------------- */
{
  const T = JSON.parse(readFileSync(join(ROOT, 'tests', 'river_vectors.json'), 'utf8'));
  const E = new Map(T.engine.rivers.map(r => [r.id, r]));
  const json = {seaLevelY: T.seaLevelY, rivers: T.rivers, lakes: T.lakes};
  const out = RB.bakeAll(json, {ground: (x, z, i, id) => E.get(id).profile.ground[i]});
  ok(out.errors.length === 0 && out.rivers.length === T.engine.rivers.length,
     `engine table: ${T.rivers.length} rivers, ${T.lakes.length} lakes parsed and baked (${out.errors.join('; ') || 'no errors'})`);
  for (const e of T.engine.lakes) {
    const j = out.lakes.find(l => l.id === e.id);
    ok(j && j.poly.length === e.outline && eqArr([j.x0, j.z0, j.x1, j.z1], e.bbox) && j.level === e.level && j.band === e.band,
       `lake "${e.id}": outline ${j && j.poly.length} (engine ${e.outline}), bbox ${j && [j.x0, j.z0, j.x1, j.z1].join(',')} (engine ${e.bbox.join(',')}), level ${e.level}, band ${e.band}`);
  }
  let falls = 0, reaches = 0;
  for (const e of T.engine.rivers) {
    const j = out.rivers.find(r => r.id === e.id);
    if (!j) { ok(false, `river "${e.id}" baked`); continue; }
    const sOk = eqArr(j.samples.s, e.profile.s), yOk = eqArr(j.samples.S, e.profile.y);
    let firstBad = -1;
    for (let i = 0; i < e.profile.y.length && firstBad < 0; i++) if (j.samples.S[i] !== e.profile.y[i] || j.samples.s[i] !== e.profile.s[i]) firstBad = i;
    const fOk = j.falls.length === e.waterfalls.length && j.falls.every((f, k) => eqArr(f, e.waterfalls[k]));
    ok(sOk && yOk && fOk && j.reaches === e.reaches && j.pieces === e.pieces && j.endLevel === e.endLevel &&
       j.lakeIn === e.lakeIn && j.lakeOut === e.lakeOut,
       `river "${e.id}": ${e.profile.s.length} samples, ${e.reaches} reaches, ${e.pieces} pieces, ${e.waterfalls.length} waterfalls, drains to ${e.endLevel}` +
       `${e.lakeIn ? ', from ' + e.lakeIn : ''}${e.lakeOut ? ', into ' + e.lakeOut : ''}` +
       (firstBad >= 0 ? ` -- FIRST DIFF at sample ${firstBad}: s ${j.samples.s[firstBad]} / ${e.profile.s[firstBad]}, S ${j.samples.S[firstBad]} / ${e.profile.y[firstBad]}` : '') +
       (j.reaches !== e.reaches ? ` -- JS reaches ${j.reaches}` : '') + (j.pieces !== e.pieces ? ` -- JS pieces ${j.pieces}` : '') +
       (!fOk ? ` -- JS falls ${JSON.stringify(j.falls).slice(0, 120)}` : ''));
    falls += e.waterfalls.length; reaches += e.reaches;
  }
  ok(falls > 0 && reaches > T.rivers.length && T.engine.rivers.some(r => r.lakeIn) && T.engine.rivers.some(r => r.lakeOut) &&
     T.rivers.some(r => r.joins) && T.rivers.some(r => r.smooth === false),
     `the table exercises waterfalls (${falls}), stepped reaches (${reaches}), a lake source, a lake mouth, a join and a polyline`);
  const jw = out.warnings.filter(w => /clamped to it/.test(w)), ew = T.engine.warnings.filter(w => /clamped to it/.test(w));
  ok(jw.length === ew.length && jw.every((w, k) => w === ew[k]), `the junction clamp warning matches the engine's (${ew.length})`);
}

/* ---- 2. by hand ---------------------------------------------------------------------------- */
{
  // a straight 400-voxel river on ground falling 1 voxel per 4 (one per sample), stepVox 4
  const json = {seaLevelY: 0, rivers: [{id: 'r', points: [[0, 0, 20, 4], [400, 0, 20, 4]], stepVox: 4}]};
  const g = x => 1000 - x / 4;
  const b = RB.bakeAll(json, {ground: (x) => g(x)}).rivers[0];
  // samples every 4 vox: 101 of them; the reach steps every 4 samples (the bed falls stepVox = 4 below the reach's G0)
  const S = b.samples.S;
  ok(b.samples.s.length === 101 && b.samples.s[100] === 400 && S[0] === 999 && S[3] === 999 && S[4] === 995 && S[100] === 899,
     `by hand: a 400-vox run falling 1/4 -> 101 samples, S 999 for samples 0..3, 995 from 4, 899 at the end (got ${S[0]}, ${S[3]}, ${S[4]}, ${S[100]})`);
  ok(b.reaches === 26 && b.falls.length === 0 && b.endLevel === 0, `by hand: 26 reaches of 4 vox, no waterfalls (each step 4 <= 20), drains to the sea 0 (got ${b.reaches})`);
  let mono = true;
  for (let i = 1; i < S.length; i++) if (S[i] > S[i - 1]) mono = false;
  ok(mono, 'by hand: the surface never rises source -> mouth');
  // a 30-voxel cliff at x = 200: one waterfall there, of 30
  const c = RB.bakeAll(json, {ground: x => (x < 200 ? 1000 : 970)}).rivers[0];
  ok(c.reaches === 2 && c.falls.length === 1 && c.falls[0][0] === 200 && c.falls[0][2] === 30,
     `by hand: a 30-vox cliff is one waterfall of 30 at x 200 (got ${JSON.stringify(c.falls)})`);
  // the bed below the sea: S clamped to the sea level
  const d = RB.bakeAll({...json, seaLevelY: 990}, {ground: x => 1000 - x / 2}).rivers[0];
  ok(Math.min(...d.samples.S) === 990 && d.samples.S[100] === 990, 'by hand: the surface never goes below the sea it drains to');
  // a mouth in a lake: the tail is held at the lake's level
  const lj = {seaLevelY: 0, rivers: [{id: 'r', points: [[0, 0, 20, 4], [400, 0, 20, 4]]}],
              lakes: [{id: 'L', level: 950, points: [[300, -100], [600, -100], [600, 100], [300, 100]]}]};
  const e = RB.bakeAll(lj, {ground: () => 900}).rivers[0];
  ok(e.lakeOut === 'L' && e.endLevel === 950 && e.samples.S.every(v => v === 950), `by hand: a river ending in a lake at 950 over flat ground at 900 holds the lake's level all along (${e.reaches} reach)`);
}

/* ---- 3. tool logic ---------------------------------------------------------------------- */
{
  // a bowl on a 64x64 grid, cells 64 vox: lakeAt at the centre -> a record
  const w = 64, hh = 64, cell = 64, h = new Float32Array(w * hh);
  for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) {
    const r = Math.hypot(i - 32, j - 32);
    h[j * w + i] = 1000 + Math.min(r, 20) * 6 - (r > 20 ? 0 : 0) + (i === 52 && j === 32 ? -200 : 0);
  }
  // a notch in the rim at (52, 32) lowers the spill level to the rim beside it
  const G = {w, hh, x0: 1000, z0: 2000, cell, h};
  const rec = WT.lakeFromGrid(G, 1000 + 32 * cell, 2000 + 32 * cell, {id: 'bowl', tolerance: 0.75});
  ok(rec && rec.level > 1000 && rec.level <= 1120 && rec.points.length >= 4 && rec.smooth === true,
     `lake tool: the bowl's spill level ${rec && rec.level} (rim 1120), outline ${rec && rec.points.length} points, smooth`);
  const inside = rec && RB.lakeInside(RB.lakePoly(RB.parseLake(rec)).poly, 1000 + 32 * cell, 2000 + 32 * cell);
  ok(inside, 'lake tool: the clicked column is inside the outline the engine will test');
  const lower = WT.lakeFromGrid(G, 1000 + 32 * cell, 2000 + 32 * cell, {id: 'bowl', level: 1040});
  const area = p => Math.abs(p.reduce((a, q, k) => { const n = p[(k + 1) % p.length]; return a + q[0] * n[1] - n[0] * q[1]; }, 0) / 2);
  ok(lower && lower.level === 1040 && area(lower.points) < area(rec.points), `lake tool: a lower level re-floods a smaller outline (${(area(lower.points) / 1e6).toFixed(2)} < ${(area(rec.points) / 1e6).toFixed(2)} Mvox²)`);
  ok(WT.lakeFromGrid(G, 1000 + 2 * cell, 2000 + 2 * cell, {id: 'x'}) === null, 'lake tool: a click on a slope that drains is no lake (null)');
  // sill warning: a lake whose polygon reaches past its basin
  const L = {...RB.parseLake({id: 's', level: 1100, points: [[0, 0], [400, 0], [400, 400], [0, 400]]})};
  Object.assign(L, RB.lakePoly(L));
  const sills = RB.sillCells(L, (x) => (x > 380 ? 900 : 1200), 16);
  ok(sills.length > 0 && sills.every(s => s[0] > 380 && s[2] > 0), `sill: ground below level+1 just outside the polygon is flagged (${sills.length} columns, all east)`);
  ok(RB.sillCells(L, () => 1200, 16).length === 0, 'sill: ground above the level all round: no warning');
}
{
  // meander keeps the endpoints, bends the middle, and bends less where it is steep
  const pts = [[0, 0, 20, 4], [3000, 0, 20, 4]];
  const flat = RB.meander(pts, {amount: 1, seed: 7, ground: () => 1000});
  const steep = RB.meander(pts, {amount: 1, seed: 7, ground: x => 1000 - x * 0.05});
  const dev = k => Math.max(...k.map(p => Math.abs(p[1])));
  ok(flat[0][0] === 0 && flat[0][1] === 0 && flat[flat.length - 1][0] === 3000 && flat[flat.length - 1][1] === 0 && flat[0][2] === 20,
     `meander: the source and the mouth stay put (${flat.length} knots)`);
  ok(dev(flat) > 20 && dev(flat) <= 60, `meander: on flat ground it swings ${dev(flat).toFixed(0)} vox (<= 3 x width 20)`);
  ok(dev(steep) < dev(flat) * 0.5, `meander: on a 5 % slope it swings less (${dev(steep).toFixed(0)} vs ${dev(flat).toFixed(0)})`);
  const again = RB.meander(pts, {amount: 1, seed: 7, ground: () => 1000});
  ok(JSON.stringify(again) === JSON.stringify(flat), 'meander: deterministic for a seed');
  // wavelength ~ 12 x width: count the sign changes of the offset along 3000 vox at w 20 (lambda 240)
  const T = (await import(pathToFileURL(join(ROOT, 'assets/editor/spline.js')).href)).tessellate(flat, {step: 8});
  let flips = 0;
  for (let i = 1; i < T.pts.length; i++) if (Math.sign(T.pts[i][1]) !== Math.sign(T.pts[i - 1][1]) && Math.abs(T.pts[i][1]) > 1) flips++;
  ok(flips >= 8 && flips <= 60, `meander: ${flips} crossings of the old line over 3000 vox (wavelength ~240)`);
}
{
  // derive: HY.rivers on a tilted valley grid -> records with joins, orders, world coords, widths
  const w = 96, hh = 96, h = new Float32Array(w * hh);
  for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) h[j * w + i] = 500 + j * 4 + Math.abs(i - 48) * 3 + ((i * 7 + j * 13) % 5) * 0.1;
  const F = HY.flow(h, w, hh, 256, {seaY: 0});
  const list = HY.rivers(h, w, hh, 256, {flow: F, threshold: 40});
  const G = {x0: -5000, z0: 7000, cell: 256};
  const recs = RB.derivedToRecords(list, G, {vpm: 10, taken: ['river_d0']});
  const trunk = recs[0];
  ok(recs.length >= 2 && !recs.some(r => r.id === 'river_d0') && recs.some(r => r.joins === trunk.id) && trunk.order > 1,
     `derive: ${recs.length} rivers, ids fresh, tributaries join the trunk (order ${trunk.order})`);
  const xs = trunk.points.map(p => p[0]), zs = trunk.points.map(p => p[1]);
  ok(Math.min(...xs) >= G.x0 - 256 && Math.max(...xs) <= G.x0 + w * 256 && Math.min(...zs) >= G.z0 - 256 && Math.max(...zs) <= G.z0 + hh * 256,
     'derive: points are world voxels inside the grid');
  ok(trunk.points.every(p => p.length === 4 && p[2] >= 8 && p[2] <= 1024 && p[3] >= 2) && trunk.points[trunk.points.length - 1][2] >= trunk.points[0][2],
     `derive: every knot carries w / d, widening downstream (${trunk.points[0][2]} -> ${trunk.points[trunk.points.length - 1][2]} vox)`);
  const baked = RB.bakeAll({seaLevelY: 0, rivers: recs.map(r => ({id: r.id, points: r.points, smooth: true, joins: r.joins || undefined}))},
                           {ground: (x, z) => 500 + (z - G.z0) / 256 * 4});
  ok(baked.errors.length === 0 && baked.rivers.length === recs.length, `derive: the accepted records load in the engine's parse (${baked.errors.join('; ') || 'no errors'})`);
}
{
  // the River tool's snapping: onto a river (joins), into a lake, onto the coast
  const json = {seaLevelY: 100, rivers: [{id: 'main', smooth: true, points: [[0, 0, 40, 8], [2000, 0, 60, 10]]}],
                lakes: [{id: 'L', level: 900, points: [[3000, 3000], [3400, 3000], [3400, 3400], [3000, 3400]]}]};
  const ground = (x, z) => (z > 5000 ? 50 : 500);
  const s1 = WT.snapMouth(json, 'trib', 1000, 30, {ground, reach: 60});
  ok(s1.kind === 'river' && s1.joins === 'main' && Math.abs(s1.z) <= 1, `snap: a mouth near a river lands on its centreline and joins it (${JSON.stringify(s1)})`);
  const s2 = WT.snapMouth(json, 'trib', 3200, 3200, {ground, reach: 60});
  ok(s2.kind === 'lake' && s2.lake === 'L' && s2.joins === null, 'snap: a mouth inside a lake drains into it');
  const s3 = WT.snapMouth(json, 'trib', 100, 5200, {ground, reach: 60});
  ok(s3.kind === 'sea', 'snap: a mouth on ground below the sea level is the coast');
  const s4 = WT.snapMouth(json, 'trib', 100, 3000, {ground, reach: 60});
  ok(s4.kind === 'none', 'snap: open ground snaps to nothing');
  const s5 = WT.snapMouth(json, 'main', 1000, 30, {ground, reach: 60});
  ok(s5.kind !== 'river', 'snap: a river never joins itself');
}

console.log(fail ? `test_riverbake: ${fail} FAILED` : 'test_riverbake: OK');
process.exit(fail ? 1 : 0);
