// test_maplocale.mjs — the locale editor's pure half (assets/editor/maplocale.js,
// docs/PLAN_map_v2.md E4), the parts that need no browser:
//   * LOCALE_ID_RE is worldmap.cpp LocaleIdOk's character set;
//   * the sculpt clip is EXACT: brute force over a small locale, every column
//     OUTSIDE the bounds reads (offset and gradient, worldgen.wgsl
//     sculptOctave's four samples) only samples the clip leaves alone, and the
//     clip does not refuse more than it must (every inside column's own cell
//     has an editable sample unless it is within one sample of the edge);
//   * a structure's yaw turns local +z (its front) the way world/structures.cpp
//     says (0 +Z, 90 +X, 180 -Z, 270 -X) and its footprint matches the
//     engine's rotated box (RotCorner) for all four turns;
//   * clampBoxSite keeps a box AND its feather inside; circleInside;
//   * localeProblems refuses what the loader refuses and passes the shipped
//     default map's locales (Harrowby).
//   node scripts/test_maplocale.mjs
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import * as L from '../assets/editor/maplocale.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let fail = 0;
const say = (ok, msg) => { console.log((ok ? 'PASS  ' : 'FAIL  ') + msg); if (!ok) fail++; };

// ---- the id
const src = fs.readFileSync(path.join(ROOT, 'src/sim/worldmap.cpp'), 'utf8');
const idOk = src.match(/bool LocaleIdOk[\s\S]*?\n\}/);
say(!!idOk && /'a' && c <= 'z'/.test(idOk[0]) && /'0' && c <= '9'/.test(idOk[0]) && /c == '_'/.test(idOk[0]) && /c == '-'/.test(idOk[0]) &&
    L.LOCALE_ID_RE.test('harrowby') && L.LOCALE_ID_RE.test('_e4-t3st') && !L.LOCALE_ID_RE.test('Harrowby') && !L.LOCALE_ID_RE.test('a/b') && !L.LOCALE_ID_RE.test(''),
    'LOCALE_ID_RE = LocaleIdOk: [a-z0-9_-]+');

// ---- the sculpt clip, brute force
const wgsl = fs.readFileSync(path.join(ROOT, 'assets/shaders/worldgen.wgsl'), 'utf8');
say(/let s10 = sculptSample\(t, k \+ 1\);/.test(wgsl) && /let sx = x >> WM_SCULPT_SPACING_LOG2;/.test(wgsl),
    'worldgen.wgsl sculptOctave still reads samples x >> 3 and (x >> 3) + 1 (the reach the clip assumes)');
for (const b of [[100, 200, 260, 330], [-77, -300, 41, -120], [0, 0, 31, 31], [5, 5, 12, 12]]) {
  const loc = {bounds: {min: [b[0], b[1]], max: [b[2], b[3]]}};
  let leak = 0, cells = 0, inner = 0, innerOk = 0;
  for (let z = b[1] - 40; z <= b[3] + 40; z++)
    for (let x = b[0] - 40; x <= b[2] + 40; x++) {
      const inside = x >= b[0] && x <= b[2] && z >= b[1] && z <= b[3];
      const samples = L.sculptSamplesOf(x, z);
      if (!inside) {
        cells++;
        if (samples.some(([sx, sz]) => L.sculptSampleInside(loc, sx, sz))) leak++;
      } else if (x - b[0] >= 16 && b[2] - x >= 16 && z - b[1] >= 16 && b[3] - z >= 16) {
        inner++;
        if (samples.some(([sx, sz]) => L.sculptSampleInside(loc, sx, sz))) innerOk++;
      }
    }
  say(leak === 0 && innerOk === inner, `sculpt clip over bounds ${JSON.stringify(b)}: 0 of ${cells} outside columns read an editable sample (${leak}); ` +
      `${innerOk} / ${inner} columns 1.6 m in can be sculpted`);
}

// ---- yaw and footprint
const fr = y => L.rotXZ(y, 0, 1).join();
say(fr(0) === '0,1' && fr(90) === '1,0' && fr(180) === '0,-1' && fr(270) === '-1,0' && fr(-90) === '-1,0' && fr(450) === '1,0',
    'rotXZ turns the front (local +z) to +Z / +X / -Z / -X at 0 / 90 / 180 / 270');
// world/structures.cpp RotCorner + YawToRot (read from the source so a change there fails here)
const sc = fs.readFileSync(path.join(ROOT, 'src/world/structures.cpp'), 'utf8');
say(/return \(4 - q\) & 3;/.test(sc) && /case 1: \*ox = nz - z; \*oz = x;/.test(sc), 'structures.cpp YawToRot / RotCorner are the ones this test restates');
function engineBox(sj, pos, yaw) {
  const q = (((yaw / 90) % 4) + 4) % 4, rot = (4 - q) & 3;
  const nx = sj.size[0], nz = sj.size[2];
  const rc = (x, z) => rot === 0 ? [x, z] : rot === 1 ? [nz - z, x] : rot === 2 ? [nx - x, nz - z] : [z, nx - x];
  const [ox, oz] = rc(sj.origin[0], sj.origin[2]);
  const cs = [rc(0, 0), rc(nx, nz)];
  return [Math.min(cs[0][0], cs[1][0]) - ox + pos[0], Math.min(cs[0][1], cs[1][1]) - oz + pos[2],
          Math.max(cs[0][0], cs[1][0]) - ox + pos[0], Math.max(cs[0][1], cs[1][1]) - oz + pos[2]];
}
const sj = JSON.parse(fs.readFileSync(path.join(ROOT, 'assets/structures/harrowby_smithy.struct.json'), 'utf8'));
let fpOk = true, fpMsg = [];
for (const yaw of [0, 90, 180, 270]) {
  const fp = L.footprint(sj, [700, 5, 3460], yaw);
  const xs = fp.map(p => p[0]), zs = fp.map(p => p[1]);
  const mine = [Math.min(...xs), Math.min(...zs), Math.max(...xs), Math.max(...zs)];
  const eng = engineBox(sj, [700, 5, 3460], yaw);
  // the engine's corner origin is the anchor CELL's low corner: one voxel of
  // difference on the turned axes is the cell, not a different box
  const ok = mine.every((v, i) => Math.abs(v - eng[i]) <= 1);
  fpOk = fpOk && ok;
  fpMsg.push(yaw + ': ' + mine.join(',') + (ok ? '' : ' vs engine ' + eng.join(',')));
}
say(fpOk, 'footprint of harrowby_smithy (' + sj.size.join('x') + ') matches the engine\'s turned box within the anchor cell: ' + fpMsg.join(' | '));
const door = sj.slots.find(s => s.name === 'door_front_0');
const dw = L.slotWorld(sj, [700, 5, 3460], 270, door);
say(dw[0] < 700 && Math.abs(dw[2] - 3460) < 20, `yaw 270 (front faces -X) puts the smithy's front door west of its anchor: (${dw.join(',')})`);
say(L.insidePoly(L.footprint(sj, [0, 0, 0], 90), 1, 1) && !L.insidePoly(L.footprint(sj, [0, 0, 0], 90), 500, 1), 'insidePoly hits the footprint');

// ---- boxes and discs
const loc = {bounds: {min: [0, 0], max: [199, 199]}};
const s1 = {min: [-50, 20], max: [80, 260], feather: 64};
L.clampBoxSite(loc, s1, 64);
say(s1.min.join() === '0,20' && s1.max.join() === '80,199' && s1.feather === 0, 'a box over the edge is clamped and loses its feather: ' + JSON.stringify(s1));
const s2 = {min: [50, 60], max: [100, 120], feather: 96};
L.clampBoxSite(loc, s2, 96);
say(s2.feather === 50 && s2.min.join() === '50,60', 'a box 50 voxels in keeps a 50-voxel feather: ' + JSON.stringify(s2));
const s3 = {min: [70, 70], max: [90, 90]};
say(!L.clampBoxSite(loc, s3, 20) && s3.feather === undefined, 'a box well inside is untouched');
say(L.circleInside(loc, 100, 100, 99) && !L.circleInside(loc, 100, 100, 101) && L.inBounds(loc, 199, 0) && !L.inBounds(loc, 200, 0), 'circleInside / inBounds (inclusive bounds)');

// ---- loader refusals
const P = j => L.localeProblems(j) || [];
const good = {id: 'x', bounds: {min: [0, 0], max: [10, 10]}, anchor: {at: [5, 5], y: 200}};
say(P({locales: [good]}).length === 0, 'a well-formed locale passes');
say(P({locales: [{...good, id: 'Bad'}]}).length === 1 && P({locales: [good, good]}).length === 1 &&
    P({locales: [{...good, bounds: {min: [10, 0], max: [0, 10]}}]}).length === 1 &&
    P({locales: [{...good, anchor: {at: [5, 5]}}]}).length === 1 &&
    P({locales: [{...good, editLayer: '../x'}]}).length === 1 && P({locales: [{...good, editLayer: 'cellar.svedit'}]}).length === 0,
    'refused: a bad id, a duplicate, min > max, no anchor.y, an editLayer that is a path (a stem with .svedit is fine)');
const dm = JSON.parse(fs.readFileSync(path.join(ROOT, 'assets/worldmap/default/map.json'), 'utf8'));
const hb = (dm.locales || []).find(l => l.id === 'harrowby');
say(!!hb && P(dm).length === 0 && hb.icon === 'village', 'the default map\'s locales (Harrowby, icon village) pass');
say(L.freshLocaleId([{id: 'locale_0'}, {id: 'locale_1'}]) === 'locale_2' && L.linkTarget('harrowby', 'path_west') === 'harrowby/path_west' &&
    L.linkTarget('harrowby', 'harrowby/smithy/waynode_front_0_out') === 'harrowby/smithy/waynode_front_0_out', 'freshLocaleId, linkTarget');

console.log(fail ? `test_maplocale: FAIL (${fail})` : 'test_maplocale: OK');
process.exit(fail ? 1 : 0);
