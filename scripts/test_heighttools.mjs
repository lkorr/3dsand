// test_heighttools.mjs — the World map page's HEIGHT TOOLS (docs/PLAN_map_v2.md
// E2) without a browser: every brush mode changes only the tiles under it and
// moves the posts the way it says, a ridge / valley bake undoes byte-exact,
// the shape stamps raise / lower / level, erosion is a pure function of its
// seed. The browser half (pointer strokes, the worker, the PNG routes) is
// assets/environment_test.html's map section.
//
//   node scripts/test_heighttools.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const T = await import(pathToFileURL(join(ROOT, 'assets/editor/maptiles.js')).href);
const HS = await import(pathToFileURL(join(ROOT, 'assets/editor/heightshapes.js')).href);
const ER = await import(pathToFileURL(join(ROOT, 'assets/editor/erosion.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };

// 600 x 600 posts (3 x 3 tiles), 64-voxel posts, default y 200
const json = {
  version: 2, extent: {min: [0, 0], size: [38336, 38336]}, siteCellLog2: 6,
  planes: {height: {log2: 6, tile: 256, default: 200}, biome: {log2: 8, tile: 256, default: 'ocean'}},
  biomes: ['ocean'],
};
const fresh = () => T.makePlane('height', json);
const post = i => i * 64;                       // world voxel of post i
const bytesOf = (P, k) => { const t = P.tiles.get(k); const [tx, tz] = T.tileXZ(k); return t ? T.encodeTile(P, tx, tz, t) : null; };
const sameBytes = (a, b) => (a === null || b === null ? a === b : a.length === b.length && a.every((v, i) => v === b[i]));
const keys = P => [...P.dirty].sort((a, b) => a - b).map(T.tileXZ).map(p => p.join(',')).join(' ');

// ---- every mode, at the middle of tile (1,1): touches only that tile ----
const cx = post(384), cz = post(384);
const base = (P) => { // a gentle slope so smooth / terrace / erode have something to act on
  T.writeWindow(P, 256, 256, 256, 256, (n) => 200 + (n % 256) * 3 + Math.floor(n / 256) * 2 + ((n * 7) % 5) * 20, null);
  P.dirty.clear();
};
const modes = {
  raise: {amount: 100}, lower: {amount: 100}, smooth: {k: 1}, flatten: {target: 900, k: 1}, set: {target: 300, k: 1},
  terrace: {step: 120, sharp: 0.8, datum: 112, k: 1}, noise: {amp: 200, size: 2000, seed: 5, style: 'ridged'},
  erode: {k: 1, erosion: {iterations: 4, strength: 1, seed: 3}},
};
for (const m of T.HEIGHT_MODES) {
  const P = fresh();
  base(P);
  const before = new Map(), h0 = T.heightAt(P, cx, cz);
  const r = T.heightDab(P, cx, cz, {mode: m, radius: 3000, falloff: 'smooth', stroke: {}, ...modes[m]}, before);
  const h1 = T.heightAt(P, cx, cz);
  ok(r.changed > 0 && keys(P) === '1,1' && before.size === 1, `${m}: ${r.changed} posts changed, dirty tiles = ${keys(P)} (only the one under the brush)`);
  // and an undo of the dab restores the tile byte-exact
  const after = bytesOf(P, T.tileKey(1, 1));
  T.swapTiles(P, before);
  const back = fresh(); base(back);
  ok(sameBytes(bytesOf(P, T.tileKey(1, 1)), bytesOf(back, T.tileKey(1, 1))) && !sameBytes(after, bytesOf(back, T.tileKey(1, 1))),
     `${m}: undo restores the tile byte-exact (centre ${h0.toFixed(1)} -> ${h1.toFixed(1)} -> ${T.heightAt(P, cx, cz).toFixed(1)})`);
}
{ // the modes move the post the way they say
  const P = fresh(); base(P);
  const h0 = T.heightAt(P, cx, cz);
  T.heightDab(P, cx, cz, {mode: 'set', radius: 500, target: 1234, k: 1}, null);
  ok(Math.abs(T.heightAt(P, cx, cz) - 1234) < 1e-3, 'set at strength 100 % lands the centre post on the target');
  T.heightDab(P, cx, cz, {mode: 'terrace', radius: 3000, step: 100, sharp: 0.9, datum: 0, k: 1}, null);
  const tv = T.heightAt(P, cx + 640, cz + 640);
  ok(Math.abs(tv - Math.round(tv / 100) * 100) < 12, `terrace: a post in the core sits on a 100-vox shelf (${tv.toFixed(1)})`);
  ok(HS.terrace(149, 0, 100, 0.9) === 100 && HS.terrace(199, 0, 100, 0) > 190 && HS.terrace(-70, 0, 100, 0.5) === -100,
     'terrace(): 90 % shelf, soft steps at 0, bands below the datum');
  const Q = fresh();
  const st = {}, bq = new Map();
  for (let i = 0; i < 6; i++) T.heightDab(Q, cx + i * 10, cz, {mode: 'noise', radius: 2000, amp: 300, size: 1500, seed: 9, stroke: st}, bq);
  const once = fresh();
  T.heightDab(once, cx, cz, {mode: 'noise', radius: 2000, amp: 300, size: 1500, seed: 9, stroke: {}}, new Map());
  ok(Math.abs(T.heightAt(Q, cx, cz) - T.heightAt(once, cx, cz)) < 1e-3, 'noise is stroke-stable: six overlapping dabs leave the centre where one dab does');
  const R2 = fresh();
  T.heightDab(R2, cx, cz, {mode: 'noise', radius: 2000, amp: 300, size: 1500, seed: 10, stroke: {}}, null);
  ok(T.heightAt(R2, cx + 300, cz + 200) !== T.heightAt(once, cx + 300, cz + 200), 'a different noise seed is different ground');
  // falloffs: a flat brush holds its core, a linear one does not
  const F = fresh(), L = fresh();
  T.heightDab(F, cx, cz, {mode: 'raise', radius: 2000, amount: 100, falloff: 'flat'}, null);
  T.heightDab(L, cx, cz, {mode: 'raise', radius: 2000, amount: 100, falloff: 'linear'}, null);
  ok(Math.abs(T.heightAt(F, cx + 1280, cz) - 300) < 1e-3 && T.heightAt(L, cx + 1280, cz) < 250,
     `falloff: flat keeps 100 % at 0.64 r (${T.heightAt(F, cx + 1280, cz).toFixed(1)}), linear ${T.heightAt(L, cx + 1280, cz).toFixed(1)}`);
  // erode brush past the cap refuses rather than stalling the page
  const big = T.heightDab(fresh(), cx, cz, {mode: 'erode', radius: 20000, k: 1}, null);
  ok(big.tooBig && big.changed === 0, 'an erode dab over ERODE_BRUSH_MAX_POSTS is refused (tooBig)');
}

// ---- brush MASKS (PLAN_worldbuilder K1): land / sea / height band / slope band ----
{
  // a coast: posts west of column 384 under the sea (y 0), east of it land rising 4 vox a post
  const coast = () => {
    const P = fresh();
    T.writeWindow(P, 256, 256, 256, 256, (n) => { const i = n % 256; return i < 128 ? 0 : 112 + (i - 128) * 4; }, null);
    P.dirty.clear();
    return P;
  };
  const sea = 112, wx = post(384), wz = post(384);
  const dab = (P, mask) => T.heightDab(P, wx, wz, {mode: 'raise', radius: 3000, amount: 100, mask}, null);
  let P = coast();
  dab(P, {kind: 'land', seaY: sea, soft: 1});
  ok(T.heightAt(P, post(370), wz) === 0 && T.heightAt(P, post(400), wz) > 112 + 16 * 4 + 50,
     `mask land: the sea post stays (${T.heightAt(P, post(370), wz)}), the land post rises (${T.heightAt(P, post(400), wz).toFixed(1)})`);
  P = coast();
  dab(P, {kind: 'sea', seaY: sea, soft: 1});
  ok(T.heightAt(P, post(370), wz) > 50 && Math.abs(T.heightAt(P, post(400), wz) - (112 + 16 * 4)) < 1e-3, 'mask sea: only the sea floor rises');
  P = coast();
  dab(P, {kind: 'height', lo: 112 + 20, hi: 112 + 60, soft: 1});
  ok(Math.abs(T.heightAt(P, post(400), wz) - (112 + 64)) < 1e-3 && T.heightAt(P, post(392), wz) > 112 + 32 + 50,
     'mask height band: a post in the band rises, one above it does not');
  const S = fresh();   // two slopes: gentle west of 384, a 45-degree face east of it
  T.writeWindow(S, 256, 256, 256, 256, (n) => { const i = n % 256; return i < 128 ? 200 + i * 2 : 200 + 256 + (i - 128) * 64; }, null);
  const g0 = T.heightAt(S, post(370), wz), s0 = T.heightAt(S, post(400), wz);
  T.heightDab(S, wx, wz, {mode: 'raise', radius: 3000, amount: 100, mask: {kind: 'slope', lo: 30, hi: 60, soft: 2}}, null);
  ok(T.heightAt(S, post(370), wz) === g0 && T.heightAt(S, post(400), wz) > s0 + 50, 'mask slope band 30..60 deg: the 45-degree face rises, the gentle slope does not');
  const mw = T.maskWeights(S, 380, 380, 8, 1, {kind: 'none'});
  ok(mw === null && T.MASK_KINDS.join() === 'none,land,sea,height,slope', 'no mask = null weights; kinds: ' + T.MASK_KINDS.join(', '));
  const Q = coast(), bq = new Map();
  T.applyOp(Q, HS.shapeStamp(Q, {kind: 'peak', x: wx, z: wz, radius: 3000, height: 500}), bq, {kind: 'land', seaY: sea, soft: 1});
  ok(T.heightAt(Q, post(370), wz) === 0 && T.heightAt(Q, post(400), wz) > 112 + 64 + 100, 'applyOp honours a mask too (a peak stamped on the coast stays out of the sea)');
}

// ---- ridge / valley line bake ----
{
  const P = fresh();
  const line = {id: 'r0', kind: 'ridge', profile: 'sharp', width: 2000, points: [[post(100), post(300), 6000], [post(500), post(320), 9000]]};
  const before = new Map();
  const r = T.applyOp(P, HS.lineBake(P, line), before);
  const mid = T.heightAt(P, post(300), post(310));
  ok(r.changed > 0 && Math.abs(mid - 7500) < 40, `ridge bake: the crest at the middle vertex is ~7500 (${mid.toFixed(1)}), ${r.changed} posts, tiles ${keys(P)}`);
  ok(T.heightAt(P, post(300), post(310) + 2100) === 200, 'nothing past the width moves');
  const snap = new Map([...before.keys()].map(k => [k, bytesOf(P, k)]));
  const redo = T.swapTiles(P, before);
  ok([...before.keys()].every(k => !P.tiles.has(k)), 'undo of the bake: every touched tile is absent again (the plane was all default)');
  T.swapTiles(P, redo);
  ok([...snap].every(([k, b]) => sameBytes(bytesOf(P, k), b)), 'redo of the bake is byte-exact');
  const V = fresh();
  T.writeWindow(V, 0, 0, 600, 600, () => 3000, null);
  const vbefore = new Map(), copy = T.cloneTiles(new Map([...V.tiles].map(([k, t]) => [k, t])));
  T.applyOp(V, HS.lineBake(V, {kind: 'valley', profile: 'U', width: 1500, points: [[post(50), post(50), 500], [post(550), post(550), 500]]}), vbefore);
  ok(Math.abs(T.heightAt(V, post(300), post(300)) - 500) < 1 && T.heightAt(V, post(300) + 1100, post(300) - 1100) === 3000,
     `valley bake: floor 500 on the line (${T.heightAt(V, post(300), post(300)).toFixed(1)}), the land untouched past the width`);
  T.swapTiles(V, vbefore);
  ok([...copy].every(([k, t]) => sameBytes(bytesOf(V, k), T.encodeTile(V, ...T.tileXZ(k), t))), 'valley bake + undo round-trips byte-exact');
  // a ridge never lowers a higher hill
  const Hh = fresh();
  T.heightDab(Hh, post(300), post(310), {mode: 'raise', radius: 900, amount: 9000}, null);
  const hill = T.heightAt(Hh, post(300), post(310));
  T.applyOp(Hh, HS.lineBake(Hh, line), null);
  ok(T.heightAt(Hh, post(300), post(310)) === hill, 'a ridge baked over a higher hill leaves the hill alone (max, not set)');
  // unbake: the pre-bake posts come back, a stroke painted since stays as its offset
  const U = fresh();
  const ub = new Map();
  T.applyOp(U, HS.lineBake(U, line), ub);
  const rec = {before: ub, after: T.snapTiles(U, ub.keys())};
  T.heightDab(U, post(300), post(330), {mode: 'raise', radius: 300, amount: 77}, null);
  const undoU = new Map(), since = T.unbake(U, rec, undoU);
  ok(since > 0 && Math.abs(T.heightAt(U, post(300), post(330)) - (200 + 77)) < 1e-3 && T.heightAt(U, post(300), post(310)) === 200,
     `unbake: the line is gone (crest back to 200), the later +77 stroke stays (${since} posts edited since the bake)`);
  ok(HS.PROFILE_NAMES.join() === 'V,U,round,sharp', 'four cross-sections: ' + HS.PROFILE_NAMES.join(', '));
}

// ---- shape stamps ----
{
  for (const kind of HS.SHAPE_NAMES) {
    const P = fresh();
    const before = new Map();
    const o = {kind, x: cx, z: cz, radius: 3000, height: 4000, top: 4200, aspect: kind === 'ridge' ? 0.3 : 1, rot: 30};
    const r = T.applyOp(P, HS.shapeStamp(P, o), before);
    const c = T.heightAt(P, cx, cz);
    const want = {peak: 4200, ridge: 4200, basin: -3800, volcano: 200 + 4000 * (Math.pow(0.8, 1.3) - 0.42), plateau: 4200, mesa: 4200}[kind];
    ok(r.changed > 0 && Math.abs(c - want) < 2 && keys(P) === '1,1' && T.heightAt(P, cx + 3100, cz) === 200,
       `${kind}: centre ${c.toFixed(1)} (want ${want.toFixed(1)}), only tile 1,1, nothing past the radius`);
  }
  const P = fresh();
  T.applyOp(P, HS.shapeStamp(P, {kind: 'ridge', x: cx, z: cz, radius: 3000, height: 1000, aspect: 0.2, rot: 0}), null);
  ok(T.heightAt(P, cx + 1200, cz) > 1100 && T.heightAt(P, cx, cz + 900) < 400, 'a ridge at rot 0 runs along x (long), narrow along z');
  const Q = fresh();
  T.applyOp(Q, HS.shapeStamp(Q, {kind: 'ridge', x: cx, z: cz, radius: 3000, height: 1000, aspect: 0.2, rot: 90}), null);
  ok(T.heightAt(Q, cx, cz + 1200) > 1100 && T.heightAt(Q, cx + 900, cz) < 400, 'rotated 90 degrees it runs along z');
  const M = fresh();
  T.applyOp(M, HS.shapeStamp(M, {kind: 'mesa', x: cx, z: cz, radius: 3000, top: 2200}), null);
  ok(Math.abs(T.heightAt(M, cx + 2300, cz) - 2200) < 1 && T.heightAt(M, cx + 2900, cz) < 400, 'mesa: flat top to 0.77 r, cliff by 0.97 r');
}

// ---- erosion: deterministic for a seed ----
{
  const w = 96, h = 80, src = new Float32Array(w * h);
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) src[j * w + i] = 3000 * Math.max(0, 1 - Math.hypot(i - 48, j - 40) / 26) + HS.noise2(i / 9, j / 9, 4) * 100;
  const o = {seed: 42, iterations: 5, strength: 0.8};
  const a = ER.erode(src, w, h, 64, o), b = ER.erode(src, w, h, 64, o), c = ER.erode(src, w, h, 64, {...o, seed: 43});
  let diffAB = 0, diffAC = 0, moved = 0;
  for (let i = 0; i < a.length; i++) { if (a[i] !== b[i]) diffAB++; if (a[i] !== c[i]) diffAC++; moved = Math.max(moved, Math.abs(a[i] - src[i])); }
  ok(diffAB === 0, 'erosion is deterministic for a seed (two runs bit-identical)');
  ok(diffAC > 0 && moved > 1, `a different seed erodes differently (${diffAC} posts differ; max change ${moved.toFixed(1)} vox)`);
  let edge = 0;
  for (let i = 0; i < w; i++) edge = Math.max(edge, Math.abs(a[i] - src[i]), Math.abs(a[(h - 1) * w + i] - src[(h - 1) * w + i]));
  ok(edge === 0, 'the window edge is untouched (the change feathers out)');
  // conservation, measured without the edge feather (which fades the change
  // -- deposits included -- to nothing at the window edge)
  const nf = ER.erode(src, w, h, 64, {...o, feather: 0});
  let sIn = 0, sOut = 0;
  for (let i = 0; i < a.length; i++) { sIn += src[i]; sOut += nf[i]; }
  ok(Math.abs(sOut - sIn) / sIn < 0.01, `material is conserved inside the window (${((sOut - sIn) / sIn * 100).toFixed(3)} %)`);
}

console.log(fail ? `test_heighttools: ${fail} FAILED` : 'test_heighttools: OK');
process.exit(fail ? 1 : 0);
