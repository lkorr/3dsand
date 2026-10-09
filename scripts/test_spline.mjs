// test_spline.mjs — assets/editor/spline.js, the world map's ONE spline
// (docs/PLAN_worldbuilder.md §2), against the ENGINE's table
// tests/spline_vectors.json. The table is written by the reference
// implementation scripts/gen_spline_vectors.py and read by the `rivers`
// selftest gate (C++ worldmap::SplineTessellate) too, so a pass here means the
// editor's curves are the engine's curves.
//
//   node scripts/test_spline.mjs            check spline.js against the table + properties
//   python scripts/gen_spline_vectors.py    (re)write the table: a CONVENTION change,
//                                           the C++ twin and spline.js follow in the same commit
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';
import {readFileSync} from 'node:fs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const S = await import(pathToFileURL(join(ROOT, 'assets/editor/spline.js')).href);
const VEC = join(ROOT, 'tests', 'spline_vectors.json');

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };

const CASES = [
  {name: 'two knots: a straight line', points: [[0, 0], [100, 0]]},
  {name: 'L corner', points: [[0, 0], [200, 0], [200, 160]]},
  {name: 'S curve with width + depth (some missing: interpolated by arc length)',
   points: [[-300, 40, 24, 6], [-120, -80], [40, 60, 60, 12], [220, -40], [400, 30, 120, 20]]},
  {name: 'uneven spacing (centripetal: no loop at the close pair)', points: [[0, 0], [10, 2], [400, 0], [410, 60]]},
  {name: 'duplicate consecutive knots merged', points: [[0, 0], [0, 0], [64, 32, 5], [64, 32, 9], [128, 0, 7]]},
  {name: 'single knot', points: [[17, -5, 3]]},
  {name: 'hairpin', points: [[0, 0], [300, 0], [300, 40], [0, 40]]},
  {name: 'negative coordinates, fractional knots', points: [[-1000.5, -2000.25, 1840], [-700, -1800, 1800], [-300.75, -1900.5, 1700]]},
  {name: 'polyline (smooth: false)', points: [[0, 0], [90, 0], [90, 90]], smooth: false},
];

// ---- 1. the engine's table: every point, its segment and its arc fraction ----
const table = JSON.parse(readFileSync(VEC, 'utf8'));
const STEP = table.step;
const TOL = 2e-3;
ok(typeof table.about === 'string' && /centripetal/i.test(table.about) && STEP === 16 && Array.isArray(table.cases) && table.cases.length >= 8,
   `the table states the convention (step ${STEP}, ${table.cases.length} cases)`);
for (const c of table.cases) {
  const t = S.tessellate(c.points, {step: STEP, smooth: c.smooth, closed: c.closed});
  let worst = 0, why = '';
  const sameN = t.pts.length === c.out.length && c.seg.length === c.out.length && c.f.length === c.out.length;
  if (!sameN) why = `${t.pts.length} points, the engine ${c.out.length}`;
  for (let i = 0; sameN && i < c.out.length; i++) {
    if (t.seg[i] !== c.seg[i]) { why = `point ${i} on segment ${t.seg[i]}, the engine ${c.seg[i]}`; break; }
    worst = Math.max(worst, Math.abs(t.pts[i][0] - c.out[i][0]), Math.abs(t.pts[i][1] - c.out[i][1]), Math.abs(t.f[i] - c.f[i]));
  }
  if (!why && worst > TOL) why = `worst diff ${worst.toFixed(4)} > ${TOL}`;
  ok(!why, `engine vector "${c.name}" (${c.closed ? 'closed' : 'open'}, ${c.smooth ? 'smooth' : 'polyline'}): ${c.out.length} points, worst |diff| ${worst.toFixed(4)}${why ? ' -- ' + why : ''}`);
}
{ // the comparison is real: a perturbed tessellation must FAIL it
  const c = table.cases.find(q => q.smooth && !q.closed && q.points.length > 2);
  const t = S.tessellate(c.points.map((p, i) => (i === 1 ? [p[0] + 1, p[1]] : p)), {step: STEP});
  let worst = 0;
  for (let i = 0; i < Math.min(t.pts.length, c.out.length); i++) worst = Math.max(worst, Math.abs(t.pts[i][0] - c.out[i][0]), Math.abs(t.pts[i][1] - c.out[i][1]));
  ok(worst > 0.1 || t.pts.length !== c.out.length, `the comparison has teeth: moving one knot 1 vox moves the output by ${worst.toFixed(3)}`);
}

// ---- 2. properties ----
{
  const t = S.tessellate([[0, 0], [100, 0]], {step: 16});
  ok(t.pts.length === 8 && t.pts.every(p => Math.abs(p[1]) < 1e-9) && Math.abs(t.pts[1][0] - 100 / 7) < 1e-9,
     `two knots: a straight line, 7 even pieces of ${(100 / 7).toFixed(3)} (<= 16)`);
  const sc = S.tessellate(CASES[2].points, {step: 16});
  let maxGap = 0, minGap = Infinity;
  for (let i = 1; i < sc.pts.length; i++) { const g = Math.hypot(sc.pts[i][0] - sc.pts[i - 1][0], sc.pts[i][1] - sc.pts[i - 1][1]); maxGap = Math.max(maxGap, g); minGap = Math.min(minGap, g); }
  ok(maxGap <= 16 + 1e-6 && minGap > 14, `S curve: chord spacing ${minGap.toFixed(2)}..${maxGap.toFixed(2)} (arc spacing uniform, <= step)`);
  const p0 = sc.pts[0], pn = sc.pts[sc.pts.length - 1];
  ok(p0[0] === -300 && p0[1] === 40 && pn[0] === 400 && pn[1] === 30, 'both ends exact');
  // the curve passes through every knot
  const fine = S.tessellate(CASES[2].points, {step: 1});
  const through = CASES[2].points.every(k => S.closest(fine, k[0], k[1]).dist < 0.05);
  ok(through, 'the curve passes through every knot (interpolating)');
  // attribute fill: knot 1 has no width; it lies between 24 and 60 by arc length
  const w1 = S.closest(sc, -120, -80).attrs[0];
  ok(w1 > 24 && w1 < 60, `a knot without w takes the arc-length interpolation (${w1.toFixed(2)})`);
  ok(Math.abs(pn[2] - 120) < 1e-9 && Math.abs(p0[3] - 6) < 1e-9, 'end attributes exact');
  // centripetal: no loop at the close pair -- the curve between knots 0 and 1 stays within a few voxels of the chord
  const un = S.tessellate(CASES[3].points, {step: 1});
  let excursion = 0;
  for (const p of un.pts) if (p[0] <= 10) excursion = Math.max(excursion, Math.abs(p[1] - p[0] * 0.2));
  ok(excursion < 6, `uneven spacing: the short segment stays near its chord (excursion ${excursion.toFixed(2)} vox, no cusp / loop)`);
  // duplicate knots merged, attrs of the first kept
  const du = S.tessellate(CASES[4].points, {step: 16});
  ok(du.knots.length === 3 && du.knots[1][2] === 5, 'duplicates merged, the first one kept (w 5, not 9)');
  const one = S.tessellate([[17, -5, 3]], {});
  ok(one.pts.length === 1 && one.pts[0][2] === 3 && one.length === 0, 'a single knot is one point');
  // closest: signed offset (+ = right of travel: travelling east, south (+z) is right)
  const line = S.tessellate([[0, 0], [1000, 0]], {step: 16});
  const c1 = S.closest(line, 500, 30), c2 = S.closest(line, 500, -30);
  ok(Math.abs(c1.offset - 30) < 1e-9 && Math.abs(c2.offset + 30) < 1e-9 && Math.abs(c1.s - 500) < 1e-6 && Math.abs(c1.t - 0.5) < 1e-6,
     `closest: s ${c1.s.toFixed(1)}, t ${c1.t.toFixed(3)}, offset +30 south / -30 north`);
  // insert / delete
  const ins = S.insertKnot(CASES[2].points, sc.length * 0.3, {step: 16});
  const t2 = S.tessellate(ins.points, {step: 16});
  ok(ins.points.length === 6 && S.closest(t2, ins.points[ins.index][0], ins.points[ins.index][1]).dist < 0.05 &&
     Math.abs(t2.length - sc.length) / sc.length < 0.02, `insertKnot at 30 %: knot ${ins.index} on the curve, length ${sc.length.toFixed(0)} -> ${t2.length.toFixed(0)}`);
  ok(S.deleteKnot(ins.points, ins.index).length === 5, 'deleteKnot');
  // handles (editor-only) override the tangent out of a knot
  const h = S.tessellate([[0, 0], [200, 0], [400, 0]], {step: 8, handles: [null, {out: [0, 150], in: [0, -150]}, null]});
  const ymax = Math.max(...h.pts.map(p => Math.abs(p[1])));
  ok(ymax > 20, `a Bezier handle bends a straight run (max offset ${ymax.toFixed(1)})`);
  // polyline mode stays straight
  const pl = S.tessellate(CASES[8].points, {step: 16, smooth: false});
  ok(pl.pts.every(p => Math.abs(p[1]) < 1e-9 || Math.abs(p[0] - 90) < 1e-9) && Math.abs(pl.length - 180) < 1e-9, 'smooth:false is the polyline (length 180)');
  // determinism: same input, bit-identical output
  const a = JSON.stringify(S.tessellate(CASES[7].points, {step: 16}).pts), b = JSON.stringify(S.tessellate(CASES[7].points, {step: 16}).pts);
  ok(a === b, 'deterministic');
}

console.log(fail ? `test_spline: ${fail} FAILED` : 'test_spline: OK');
process.exit(fail ? 1 : 0);
