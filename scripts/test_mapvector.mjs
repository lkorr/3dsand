// test_mapvector.mjs — the World map page's vector layers (assets/editor/
// mapvector.js, docs/PLAN_map_v2.md E3), the parts that need no browser:
//   * every marker icon is 12 x 12 in the icon palette, and the icon set is
//     exactly src/sim/worldmap.h kMarkerIcons (the loader warns on others);
//   * the road defaults by kind and the clamps are the loader's numbers
//     (src/sim/worldmap.cpp, read out of the source);
//   * inPolygon / regionAt answer the probes the `worldmap` gate's part F
//     asks the engine's RegionAt (the same squares, ell and table order);
//   * vectorProblems names exactly the shapes the loader refuses.
//   node scripts/test_mapvector.mjs
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import * as V from '../assets/editor/mapvector.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let fail = 0;
const say = (ok, msg) => { console.log((ok ? 'PASS  ' : 'FAIL  ') + msg); if (!ok) fail++; };

// ---- icons
for (const [n, rows] of Object.entries(V.ICONS)) {
  const shape = rows.length === V.ICON_SIZE && rows.every(r => r.length === V.ICON_SIZE);
  const pal = rows.every(r => [...r].every(c => c === '.' || c in V.ICON_PALETTE));
  const ink = rows.join('').replace(/\./g, '').length;
  say(shape && pal && ink > 20, `icon ${n}: 12x12, palette chars only, ${ink} pixels inked`);
}
const hdr = fs.readFileSync(path.join(ROOT, 'src/sim/worldmap.h'), 'utf8');
const m = hdr.match(/kMarkerIcons\[\]\s*=\s*\{([^}]*)\}/);
const cpp = m ? [...m[1].matchAll(/"([a-z_]+)"/g)].map(x => x[1]) : [];
say(cpp.join() === V.ICON_NAMES.join(), `marker icons agree with worldmap.h kMarkerIcons: ${cpp.join(', ')}`);

// ---- road defaults: the loader's `path ? a : track ? b : c` lines
const src = fs.readFileSync(path.join(ROOT, 'src/sim/worldmap.cpp'), 'utf8');
const def = key => {
  const r = src.match(new RegExp(`rd\\.\\w+ = num\\("${key}", path \\? (\\d+) : track \\? (\\d+) : (\\d+)`));
  return r ? {path: +r[1], track: +r[2], road: +r[3]} : null;
};
for (const key of ['width', 'shoulder', 'maxGrade']) {
  const d = def(key);
  say(!!d && ['road', 'track', 'path'].every(k => V.ROAD_KINDS[k][key] === d[k]),
      `road ${key} defaults by kind match the loader: ${JSON.stringify(d)}`);
}
const lim = key => {
  const r = src.match(new RegExp(`num\\("${key}",[^;]*?, (-?\\d+|static_cast<int>\\(kSite\\w+\\)), (\\d+|static_cast<int>\\(kSite\\w+\\))\\);`));
  return r;
};
const w = lim('width'), g = lim('maxGrade');
say(!!w && +w[1] === V.ROAD_LIMITS.width[0] && +w[2] === V.ROAD_LIMITS.width[1] && !!g && +g[1] === V.ROAD_LIMITS.maxGrade[0] && +g[2] === V.ROAD_LIMITS.maxGrade[1],
    `road width / grade clamps match the loader (${w && w.slice(1).join('..')}, ${g && g.slice(1).join('..')})`);

// ---- regions: the worldmap gate's part F probes
const regions = [
  {id: 'e3_square', points: [[30000, 30000], [31000, 30000], [31000, 31000], [30000, 31000]]},
  {id: 'e3_ell', points: [[32000, 30000], [33000, 30000], [33000, 30400], [32400, 30400], [32400, 31000], [32000, 31000]]},
  {id: 'e3_inner', points: [[30400, 30400], [30600, 30400], [30600, 30600], [30400, 30600]]},
];
const probes = [[30100, 30100, 'e3_square'], [30999, 30999, 'e3_square'], [31000, 30500, '-'], [30500, 30500, 'e3_inner'],
                [32200, 30800, 'e3_ell'], [32800, 30200, 'e3_ell'], [32800, 30800, '-'], [29999, 30500, '-']];
const got = probes.map(([x, z]) => (V.regionAt(regions, x, z) || {id: '-'}).id);
say(got.every((id, i) => id === probes[i][2]), `regionAt == the engine's RegionAt probes: ${got.join(' ')}`);
say(Math.abs(V.polygonArea(regions[1].points)) === 1000 * 400 + 400 * 600, 'polygonArea of the ell is 640000 vox²');
const c = V.polygonLabelAt(regions[0].points);
say(Math.abs(c[0] - 30500) < 1e-9 && Math.abs(c[1] - 30500) < 1e-9, 'a square labels at its centre');

// ---- arcs
const line = [[0, 0], [30, 40], [30, 100]];
say(V.vertexArcs(line).join() === '0,50,110', 'vertexArcs 0,50,110');
const p = V.pointAtArc(line, 80);
say(Math.abs(p[0] - 30) < 1e-9 && Math.abs(p[1] - 70) < 1e-9, 'pointAtArc(80) = (30, 70)');

// ---- what the loader refuses
const ok = {roads: [{id: 'a', points: [[0, 0], [10, 0]]}], regions: [{id: 'r', points: [[0, 0], [9, 0], [9, 9]]}], markers: [{id: 'm', at: [1, 2]}]};
say(V.vectorProblems(ok) === null, 'a valid set has no problems');
const bad = {roads: [{id: 'a', points: [[0, 0], [0, 0]]}, {id: 'a', points: [[0, 0], [5, 5]]}],
             regions: [{id: 'r', points: [[0, 0], [9, 0], [0, 0]]}], markers: [{id: 'm'}]};
const pr = V.vectorProblems(bad) || [];
say(pr.length === 4 && /two distinct/.test(pr.join()) && /two roads are called "a"/.test(pr.join()) && /three distinct/.test(pr.join()) && /no at/.test(pr.join()),
    'vectorProblems: ' + pr.join(' | '));
say(V.freshId([{id: 'path_0'}, {id: 'path_1'}], 'path') === 'path_2' && V.freshId([{id: 'path_2'}], 'path') === 'path_1',
    'freshId skips taken ids');

// ---- E2: roads that BEND (smooth: true = the engine's SmoothRoadPoints)
{
  const poly = {id: 'p', points: [[0, 0], [400, 0], [400, 400]]};
  const bent = {...poly, smooth: true};
  const cl = V.roadCentreline(bent);
  say(V.roadCentreline(poly) === poly.points && cl.length > 3 && cl[0][0] === 0 && cl[cl.length - 1][1] === 400 && cl.every(p => Number.isInteger(p[0]) && Number.isInteger(p[1])),
      `a smooth road's centreline is the engine's spline (${cl.length} integer vertices, the ends exact); a polyline road is its points`);
  // the corner is cut: the bent road passes inside (400, 0)
  const cut = Math.max(...cl.map(p => Math.min(V.segDist([0, 0], [400, 0], p[0], p[1]).d, V.segDist([400, 0], [400, 400], p[0], p[1]).d)));
  say(cut > 5 && cut < 120, `the curve bends through the corner point, bowing off the straight legs (up to ${cut.toFixed(0)} vox)`);
  const ka = V.knotArcs(bent), L = V.vertexArcs(cl)[cl.length - 1];
  say(ka[0] === 0 && Math.abs(ka[2] - L) < 1 && ka[1] > 300 && ka[1] < L - 300, `knotArcs: each point's arc along the curve (${ka.map(a => a.toFixed(0)).join(', ')} of ${L.toFixed(0)})`);
  // a click on the curve inserts between the right points, at the curve
  const h = V.roadCurveHit(bent, 200, -20);
  say(h && h.k === 0 && h.d < 40 && h.t > 0 && h.t < 1, `roadCurveHit near the bend: insert after point ${h && h.k}, ${h && h.d.toFixed(1)} vox off the curve`);
  const h2 = V.roadCurveHit(bent, 380, 300);
  say(h2 && h2.k === 1, 'roadCurveHit on the second span: insert after point 1');
  const hp = V.roadCurveHit(poly, 200, 3);
  say(hp && hp.k === 0 && Math.abs(hp.d - 3) < 1e-9, 'roadCurveHit on a polyline road is its segment');
}

console.log(fail ? `\nRESULT FAIL(${fail})` : '\nRESULT OK');
process.exit(fail ? 1 : 0);
