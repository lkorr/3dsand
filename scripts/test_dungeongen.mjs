/* test_dungeongen.mjs — the dungeon generator library gate (PLAN_dungeons §6, JS).
 *
 *   node scripts/test_dungeongen.mjs
 *
 * WHAT IT GUARDS:
 *   1. KITS + PALETTES: every palette slot names a real material; timber burns,
 *      rubble is a powder, rock is static, lights are emissive.
 *   2. EVERY TYPE x EVERY PRESET generates, alone and compiled to a 3.4.1 shell
 *      with no errors (materials resolve, per-64^3-cell limits hold, no prop over
 *      256 cells), with an origin socket at (0,0,0).
 *   3. SOCKETS: unit axis dir, positive size, a known profile, the opening is
 *      carved just inside, and the piece lies BEHIND the socket (outward-facing).
 *   4. SLIDER MONOTONICITY: width widens the carve AABB, height / ceiling raise it.
 *   5. KEYED CHOICES: one slider does not reshuffle unrelated seeded choices.
 *   6. DETERMINISM: compile twice -> byte-identical shell.json AND shell.bin;
 *      grow is reproducible by seed (and a different seed differs).
 *   7. VALIDATE catches a seeded overlap, an open socket, an unreachable piece,
 *      a lock with no key, a dead signal and a surface breach; a taper appears
 *      for a near-miss join and a far-off join is refused with a reason.
 *   8. shell.bin READS BACK (a JS reader of the 3.4.1 layout) to the JSON content.
 *   9. PROPS: every prop x preset generates deterministically, fits a template,
 *      and every material resolves.
 *  10. THE SAMPLE: assets/dungeons/_d2_sample is current and validates clean.
 *
 * Node only, no build, no browser, no exe.
 */
import { readFileSync, existsSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const G = await load('assets/editor/dungeon/dungeongen.js');
const U = await load('assets/editor/dungeon/dgutil.js');
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const MAT = new Map(MATS.map(m => [m.id, m]));
const NAMES = MATS.map(m => m.id);

let fails = 0, count = 0;
const ok = (cond, what) => { count++; if (cond) return true; console.log('  FAIL ' + what); fails++; return false; };
const section = (s) => console.log('\n-- ' + s + ' --');
const J = JSON.stringify;

/* ---- 1. palettes ----------------------------------------------------------- */
section('kits + palettes');
const emissive = (n) => MAT.has(n) && (MAT.get(n).emission || 0) > 0;
for (const k of G.STYLES) {
  const K = G.getKit(k);
  for (const [v, pal] of Object.entries(K.PALETTES)) {
    for (const [slot, name] of Object.entries(pal)) ok(MAT.has(name), k + '/' + v + ' slot ' + slot + ' -> "' + name + '" is a material');
    ok(MAT.get(pal.support) && (k === 'mines' ? (MAT.get(pal.support).tags || []).includes('flammable') : true), k + '/' + v + ': mine supports burn');
    ok(MAT.get(pal.rubble) && MAT.get(pal.rubble).class === 'powder', k + '/' + v + ': rubble "' + pal.rubble + '" is a loose powder');
    ok(MAT.get(pal.wall) && MAT.get(pal.wall).class === 'solid', k + '/' + v + ': wall "' + pal.wall + '" is static');
    ok(emissive(pal.light) && emissive(pal.lightCool), k + '/' + v + ': light slots are emissive');
    ok(MAT.get(pal.liquid) && MAT.get(pal.liquid).class === 'liquid', k + '/' + v + ': liquid slot is a liquid');
  }
  ok(K.TYPE_ORDER.length >= 8, k + ': ' + K.TYPE_ORDER.length + ' piece types');
}
const want = {caves: ['mouth', 'tunnel', 'cavern', 'shaft', 'chasm', 'grotto_pool', 'crawl', 'rubble_choke'],
              mines: ['adit_entrance', 'drift', 'junction', 'incline', 'shaft', 'stope', 'collapse', 'flooded_sump', 'camp'],
              ruins: ['ruin_entrance', 'hall', 'corridor', 'stair', 'crypt', 'pit_room', 'sanctum', 'flooded_hall', 'collapsed_passage']};
for (const k in want) for (const t of want[k]) ok(!!G.getKit(k).TYPES[t], k + ' has §4 type ' + t);

/* ---- 2 + 3. every type x preset ---------------------------------------------- */
section('every type x preset: generate, sockets, compile alone');
const PROFILES = new Set(['rect', 'arch', 'round', 'rough', 'shaft']);
let nGen = 0;
for (const k of G.STYLES) for (const t of G.getKit(k).TYPE_ORDER) {
  const T = G.getType(k, t);
  ok(T.SCHEMA.some(r => r.k === 'decay') && T.SCHEMA.some(r => r.k === 'lightCount') && T.SCHEMA.some(r => r.k === 'blend'), k + '/' + t + ': universal sliders present');
  ok(Object.keys(T.PRESETS).length >= 3, k + '/' + t + ': >= 3 presets');
  for (const pn of Object.keys(T.PRESETS)) for (const seed of [1, 77]) {
    const tag = k + '/' + t + '/' + pn + ' seed ' + seed;
    let r;
    try { r = T.generate(T.PRESETS[pn].params, seed); } catch (e) { ok(false, tag + ' threw ' + e.stack); continue; }
    nGen++;
    ok(r.prims.length > 0 && r.sockets.length > 0, tag + ': prims + sockets');
    ok(r.sockets.some(s => s.pos[0] === 0 && s.pos[1] === 0 && s.pos[2] === 0), tag + ': an origin socket at (0,0,0)');
    ok(!!T.surface === !!r.surface || t === 'shaft', tag + ': surface flag');
    const carves = r.prims.filter(p => p.op === 'carve');
    for (const s of r.sockets) {
      const unit = s.dir.filter(v => v !== 0).length === 1 && s.dir.every(v => v === 0 || v === 1 || v === -1);
      ok(unit && s.w > 0 && s.h > 0 && PROFILES.has(s.profile), tag + ': socket ' + s.name + ' well-formed');
      const vert = s.dir[1] !== 0;
      const inside = vert ? [s.pos[0] + 0.5, s.pos[1] - s.dir[1] * 3 + 0.5, s.pos[2] + 0.5]
                          : [s.pos[0] - s.dir[0] * 3 + 0.5, s.pos[1] + Math.min(Math.floor(s.h / 2), 8) + 0.5, s.pos[2] - s.dir[2] * 3 + 0.5];
      ok(carves.some(p => U.primContains(p, inside)), tag + ': socket ' + s.name + ' opening is carved just inside');
      let bb = null;
      for (const p of carves.filter(p => p.tag !== 'apron')) bb = U.boxUnion(bb, U.shapeBounds(p));
      const c = bb.min.map((v, i) => (v + bb.max[i]) / 2);
      const dot = (c[0] - s.pos[0]) * s.dir[0] + (c[1] - s.pos[1]) * s.dir[1] + (c[2] - s.pos[2]) * s.dir[2];
      ok(dot < 0 || (s.tags || []).includes('surface'), tag + ': socket ' + s.name + ' faces outward (piece behind it, dot ' + dot.toFixed(1) + ')');
    }
    if (seed === 1) {
      const d = G.newDungeon({name: 'one', style: k, seed: 3});
      d.pieces.push(G.newPiece(d, {kit: k, type: t, preset: pn}));
      const prog = G.compile(d, {id: 'one', materials: NAMES});
      ok(prog.errors.length === 0, tag + ': compiles alone with no errors ' + (prog.errors.length ? J(prog.errors.slice(0, 3)) : ''));
      ok(prog.prims.every(p => !p.scatter || p.scatter.length <= 4), tag + ': <= 4 scatter rules a prim');
    }
  }
}
console.log('  generated ' + nGen + ' piece instances');

/* ---- 4. slider monotonicity ------------------------------------------------- */
section('slider monotonicity');
const carveBox = (k, t, params) => {
  const r = G.getType(k, t).generate(params, 5);
  let bb = null;
  for (const p of r.prims) if (p.op === 'carve' && p.tag === 'room') bb = U.boxUnion(bb, U.shapeBounds(p));
  return bb;
};
const span = (b, i) => b.max[i] - b.min[i];
const mono = (k, t, key, lo, hi, axis, base) => {
  const a = carveBox(k, t, Object.assign({}, base || {}, {[key]: lo})), b = carveBox(k, t, Object.assign({}, base || {}, {[key]: hi}));
  const grows = axis === 'top' ? b.max[1] > a.max[1] : span(b, axis) > span(a, axis);
  ok(grows, k + '/' + t + ': ' + key + ' ' + lo + ' -> ' + hi + ' grows the carve ' + (axis === 'top' ? 'top' : 'xyz'[axis]) + ' (' + J(a) + ' -> ' + J(b) + ')');
};
mono('mines', 'drift', 'width', 2, 4, 0, {niches: 0});
mono('mines', 'drift', 'height', 2, 3.5, 'top');
mono('mines', 'drift', 'ceiling', 0, 1.5, 'top');
mono('mines', 'drift', 'length', 6, 20, 2);
mono('ruins', 'hall', 'width', 8, 16, 0);
mono('ruins', 'hall', 'ceiling', 1, 6, 'top');
mono('ruins', 'corridor', 'width', 1.6, 3.6, 0, {alcoves: 0});
mono('ruins', 'corridor', 'height', 2.2, 4, 'top');
mono('caves', 'tunnel', 'width', 2, 5, 0, {meander: 0});
mono('caves', 'cavern', 'width', 10, 24, 0);
mono('caves', 'cavern', 'ceiling', 1, 10, 'top');
mono('mines', 'stope', 'height', 4, 9, 'top');

/* ---- 5. keyed choices ------------------------------------------------------- */
section('one slider does not reshuffle unrelated choices');
{
  const T = G.getType('mines', 'drift');
  const strip = (r) => r.prims.filter(p => p.tag !== 'light').map(p => J(p));
  const a = T.generate({lightCount: 1}, 9), b = T.generate({lightCount: 4}, 9);
  ok(J(strip(a)) === J(strip(b)), 'drift: lightCount 1 -> 4 leaves every non-light prim identical');
  const lightsA = a.prims.filter(p => p.tag === 'light').map(J), lightsB = b.prims.filter(p => p.tag === 'light').map(J);
  ok(lightsA.every(x => lightsB.includes(x)), 'drift: raising lightCount keeps the lights already placed');
  const sets = (r) => r.prims.filter(p => p.mat === 'support' && p.tag !== 'light').map(J);
  const c = T.generate({niches: 0}, 9), dd = T.generate({niches: 1}, 9);
  ok(J(sets(c)) === J(sets(dd)), 'drift: niche frequency does not move a single timber set');
  const C = G.getType('caves', 'cavern');
  const m2 = C.generate({mushrooms: 2, lightType: 'crystal'}, 4), m6 = C.generate({mushrooms: 6, lightType: 'crystal'}, 4);
  ok(J(m2.prims) === J(m6.prims), 'cavern: mushroom count leaves the shell untouched');
  const mush = (r) => r.props.filter(p => p.prop === 'caves/glow_mushroom_patch' && !p.light).map(p => J(p.pos));
  ok(mush(m2).every((x, i) => mush(m6)[i] === x), 'cavern: the first mushroom patches stay put when more are added');
  const H = G.getType('ruins', 'hall');
  const cols = (r) => r.prims.filter(p => p.mat === 'support').map(J);
  ok(J(cols(H.generate({banners: 1}, 2))) === J(cols(H.generate({banners: 8}, 2))), 'hall: banner count does not move a column');
}

/* ---- 6. determinism --------------------------------------------------------- */
section('determinism');
const samplePath = join(ROOT, 'assets/dungeons/_d2_sample/dungeon.json');
const sampleText = readFileSync(samplePath, 'utf8');
const sample = JSON.parse(sampleText);
{
  const p1 = G.compile(sample, {id: '_d2_sample', sourceText: sampleText, materials: NAMES});
  const p2 = G.compile(JSON.parse(sampleText), {id: '_d2_sample', sourceText: sampleText, materials: NAMES});
  ok(G.emitShellJson(p1) === G.emitShellJson(p2), 'compile twice -> byte-identical shell.json');
  const b1 = G.emitShellBin(p1), b2 = G.emitShellBin(p2);
  ok(b1.length === b2.length && b1.every((v, i) => v === b2[i]), 'compile twice -> byte-identical shell.bin (' + b1.length + ' B)');
  ok(p1.sourceHash === G.sourceHashOf(sampleText.replace(/\r/g, '')), 'sourceHash ignores CRs');
  let d = G.newDungeon({name: 'g', style: 'mines', seed: 11});
  d.pieces.push(G.newPiece(d, {kit: 'mines', type: 'adit_entrance', preset: 'working'}));
  d.entrance = {piece: 'p0', socket: 'surface'};
  const g1 = G.grow(d, {piece: 'p0', socket: 'in'}, {count: 6, seed: 42, branch: 0.5});
  const g2 = G.grow(JSON.parse(J(d)), {piece: 'p0', socket: 'in'}, {count: 6, seed: 42, branch: 0.5});
  const g3 = G.grow(d, {piece: 'p0', socket: 'in'}, {count: 6, seed: 43, branch: 0.5});
  ok(J(g1) === J(g2), 'grow: same seed -> identical proposal (' + g1.pieces.map(p => p.type).join(',') + ')');
  ok(J(g1.pieces) !== J(g3.pieces), 'grow: another seed -> a different proposal');
  ok(g1.pieces.length >= 6, 'grow: count honoured (' + g1.pieces.length + ' incl. caps)');
  const grown = G.applyProposal(d, g1);
  const v = G.validate(grown);
  ok(!v.some(i => i.severity === 'error'), 'grow: the grown dungeon validates with no errors ' + J(v.filter(i => i.severity === 'error').slice(0, 3)));
  for (const k of ['caves', 'ruins']) {
    let e = G.newDungeon({name: 'g', style: k, seed: 5});
    e.pieces.push(G.newPiece(e, {kit: k, type: k === 'caves' ? 'mouth' : 'ruin_entrance', preset: Object.keys(G.getType(k, k === 'caves' ? 'mouth' : 'ruin_entrance').PRESETS)[0]}));
    e.entrance = {piece: 'p0', socket: 'surface'};
    const g = G.grow(e, {piece: 'p0', socket: 'in'}, {count: 5, seed: 7, branch: 0.4});
    const ve = G.validate(G.applyProposal(e, g));
    ok(g.pieces.length >= 3 && !ve.some(i => i.severity === 'error'), k + ' grow: ' + g.pieces.map(p => p.type).join(',') + ' validates clean ' + J(ve.filter(i => i.severity === 'error').slice(0, 2)));
  }
}

/* ---- 7. validate ------------------------------------------------------------ */
section('validate');
{
  let d = G.newDungeon({name: 'v', style: 'mines', seed: 2});
  d.pieces.push(G.newPiece(d, {kit: 'mines', type: 'drift', preset: 'standard'}));
  d.pieces.push(G.newPiece(d, {kit: 'mines', type: 'drift', preset: 'standard', id: 'p1', pos: [6, 0, 30]}));
  const v = G.validate(d);
  ok(v.some(i => i.code === 'overlap' && i.severity === 'error'), 'a seeded overlap is an error');
  ok(v.some(i => i.code === 'open'), 'an open socket is reported');
  ok(v.some(i => i.code === 'unreachable' && i.piece === 'p1'), 'the unconnected piece is unreachable');
  // Lock + key + signal.
  let e = G.newDungeon({name: 'l', style: 'mines', seed: 2});
  e.pieces.push(G.newPiece(e, {kit: 'mines', type: 'drift', preset: 'standard'}));
  e = G.attachPiece(e, {piece: 'p0', socket: 'out'}, {kit: 'mines', type: 'drift', preset: 'standard'}).dungeon;
  e.entrance = {piece: 'p0', socket: 'in'};
  const refsBad = [{id: 'door1', kind: 'door', pos: [0, 0, 120], props: {lock: 'iron_key'}}, {id: 'gate1', kind: 'gate', pos: [0, 0, 60], on: 'lever_a'}];
  const v2 = G.validate(e, {refs: refsBad});
  ok(v2.some(i => i.code === 'lock'), 'a lock without a key is an error');
  ok(v2.some(i => i.code === 'signal' && i.severity === 'error'), 'a receiver with no emitter is a dead signal');
  const refsKeyBehind = [{id: 'door1', kind: 'door', pos: [0, 0, 120], props: {lock: 'k'}}, {id: 'key1', kind: 'key', pos: [0, 0, 230], props: {keyId: 'k'}}];
  ok(G.validate(e, {refs: refsKeyBehind}).some(i => i.code === 'lock' && /cannot be reached/.test(i.msg)), 'a key behind its own lock is an error');
  const refsGood = [{id: 'door1', kind: 'door', pos: [0, 0, 120], props: {lock: 'k'}}, {id: 'key1', kind: 'key', pos: [0, 0, 30], props: {keyId: 'k'}},
                    {id: 'lev', kind: 'lever', pos: [0, 0, 20], props: {signal: 's1'}}, {id: 'g', kind: 'gate', pos: [0, 0, 60], on: 's1'}];
  ok(!G.validate(e, {refs: refsGood}).some(i => i.code === 'lock' || (i.code === 'signal' && i.severity === 'error')), 'a reachable key and a wired lever pass');
  // Surface breach: ground 1 m above a 2.4 m drift.
  const vb = G.validate(e, {groundAt: () => 30});
  ok(vb.some(i => i.code === 'breach'), 'a drift under 1 m of ground breaks the surface');
  ok(!G.validate(e, {groundAt: () => 200}).some(i => i.code === 'breach'), '...and 17 m of cover does not');
  // Taper + refusal.
  let t = G.newDungeon({name: 't', style: 'mines', seed: 2});
  t.pieces.push(G.newPiece(t, {kit: 'mines', type: 'drift', preset: 'standard'}));
  t = G.attachPiece(t, {piece: 'p0', socket: 'out'}, {kit: 'mines', type: 'drift', params: {width: 3.4, height: 2.8}}).dungeon;
  const tp = G.compile(t, {materials: NAMES});
  ok(tp.prims.some(p => p.piece === 'p0' || p.piece === 'p1') && G.validate(t).some(i => i.code === 'taper'), 'a 2.6 -> 3.4 m join gets an auto taper');
  const far = G.socketCompat({dir: [0, 0, 1], w: 26, h: 24, profile: 'rect'}, {dir: [0, 0, -1], w: 160, h: 60, profile: 'rect'});
  ok(!far.ok && /too different/.test(far.reason), 'a 2.6 m -> 16 m join is refused with a reason');
  ok(!G.socketCompat({dir: [0, 1, 0], w: 20, h: 20, profile: 'shaft'}, {dir: [0, 0, -1], w: 20, h: 20, profile: 'rect'}).ok, 'vertical vs horizontal is refused');
}

/* ---- 8. shell.bin reads back ------------------------------------------------ */
section('shell.bin layout');
function readBin(u8) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let at = 0;
  const U32 = () => { const v = dv.getUint32(at, true); at += 4; return v; }, I32 = () => { const v = dv.getInt32(at, true); at += 4; return v; };
  const out = {};
  if (U32() !== 0x47445653) throw new Error('magic');
  out.version = U32(); out.sourceHash = U32(); out.naturalCaves = (U32() & 1) === 1; out.anchor = [I32(), I32(), I32()]; out.keepOut = I32();
  const ns = U32(), strs = [];
  for (let i = 0; i < ns; i++) { const n = U32(); strs.push(new TextDecoder().decode(u8.subarray(at, at + n))); at += (n + 3) & ~3; }
  const S = (i) => i === 0xFFFFFFFF ? undefined : strs[i];
  out.materials = {}; const nm = U32(); for (let i = 0; i < nm; i++) { const a = U32(), b = U32(); out.materials[S(a)] = S(b); }
  const np = U32(), P = [];
  for (let i = 0; i < np; i++) { const w = []; for (let k = 0; k < 26; k++) w.push(k === 2 || k === 17 || k === 21 || k >= 22 ? U32() : I32()); P.push(w); }
  const nt = U32(), tube = []; for (let i = 0; i < nt; i++) tube.push([I32(), I32(), I32(), I32()]);
  const nsc = U32(), sc = []; for (let i = 0; i < nsc; i++) sc.push([U32(), U32(), I32(), I32(), U32(), U32(), U32(), I32()]);
  const ntp = U32(), tpl = [];
  for (let i = 0; i < ntp; i++) { const id = S(U32()), size = [I32(), I32(), I32()], npl = U32(), pal = []; for (let k = 0; k < npl; k++) pal.push(S(U32())); const nv = U32(), vox = []; for (let k = 0; k < nv; k++) vox.push([I32(), I32(), I32(), I32()]); tpl.push({id, size, palette: pal, voxels: vox}); }
  const ni = U32(), ins = []; for (let i = 0; i < ni; i++) ins.push({template: U32(), pos: [I32(), I32(), I32()], yaw: U32() * 90, id: S(U32())});
  const npc = U32(); const pcs = []; for (let i = 0; i < npc; i++) pcs.push(S(U32()));
  if (U32() !== 0x21444E45) throw new Error('no END!');
  if (at !== u8.length) throw new Error('trailing bytes');
  const OPS = ['carve', 'fill', 'skin', 'keep'], SH = ['box', 'rbox', 'ellipsoid', 'cylinder', 'capsule', 'ramp', 'vault', 'tube'];
  const KINDS = ['rubble', 'stalagmite', 'stalactite', 'crystal', 'moss', 'puddle', 'bones', 'ore_vein'], SURF = ['floor', 'ceil', 'wall', 'any'];
  out.prims = P.map((w, i) => ({op: OPS[w[0]], shape: SH[w[1]], mat: S(w[2]), a: w.slice(3, 6), b: w.slice(6, 9), r: w[9], axis: w[10], stepD: w[11], height: w[12], below: w[13],
    noise: w[16] ? [w[14], w[15], w[16], w[17]] : null, blend: w[18], skin: w[19], level: w[20] === -2147483648 ? undefined : w[20],
    tube: tube.slice(w[22], w[22] + w[23]), scatter: sc.slice(w[24], w[24] + w[25]).map(c => ({kind: KINDS[c[0]], q16: c[1], size: [c[2], c[3]], surface: SURF[c[4]], mat: S(c[5]), seed: c[6], spacing: c[7]})), piece: pcs[i]}));
  out.templates = tpl; out.instances = ins;
  return out;
}
{
  const prog = G.compile(sample, {id: '_d2_sample', sourceText: sampleText, materials: NAMES});
  const bin = readBin(G.emitShellBin(prog));
  ok(bin.sourceHash === prog.sourceHash && J(bin.anchor) === J(prog.anchor) && bin.keepOut === prog.keepOut && bin.naturalCaves === prog.naturalCaves, 'bin header == json header');
  ok(J(bin.materials) === J(prog.materials), 'bin materials table == json');
  ok(bin.prims.length === prog.prims.length, 'bin prim count == json (' + bin.prims.length + ')');
  const AX = {x: 0, y: 1, z: 2}, DR = {'+x': 0, '-x': 1, '+z': 2, '-z': 3};
  let bad = 0;
  prog.prims.forEach((p, i) => {
    const q = bin.prims[i];
    const geomA = p.shape === 'capsule' ? p.a : p.shape === 'tube' ? [0, 0, 0] : p.min, geomB = p.shape === 'capsule' ? p.b : p.shape === 'tube' ? [0, 0, 0] : p.max;
    const r = p.shape === 'vault' ? p.rise : p.shape === 'ramp' ? p.stepH : p.shape === 'tube' ? 0 : (p.r || 0);
    const axis = p.shape === 'ramp' ? DR[p.dir] : (p.shape === 'cylinder' || p.shape === 'vault') ? AX[p.axis] : 0;
    const same = q.op === p.op && q.shape === p.shape && q.mat === p.mat && J(q.a) === J(geomA) && J(q.b) === J(geomB) && q.r === r && q.axis === axis &&
      q.stepD === (p.stepD || 0) && q.height === (p.height || 0) && q.below === (p.below || 0) && q.blend === (p.blend || 0) && q.skin === (p.skin || 0) &&
      q.level === p.level && q.piece === p.piece && J(q.noise) === J(p.noise ? [p.noise.amp, p.noise.scale, p.noise.octaves, p.noise.seed] : null) &&
      J(q.tube) === J(p.shape === 'tube' ? p.pts.map((t, k) => [...t, p.r[k]]) : []) &&
      J(q.scatter) === J((p.scatter || []).map(s => ({kind: s.kind, q16: Math.floor(s.density * 65536 + 0.5), size: s.size, surface: s.surface, mat: s.mat, seed: s.seed, spacing: s.spacing || 0})));
    if (!same && bad++ < 3) console.log('    prim ' + i + ' differs: ' + J(p).slice(0, 200) + ' vs ' + J(q).slice(0, 200));
  });
  ok(bad === 0, 'every bin prim == its json twin');
  ok(J(bin.templates) === J(prog.props.templates), 'bin templates == json');
  ok(J(bin.instances) === J(prog.props.instances.map(i => ({template: i.template, pos: i.pos, yaw: i.yaw, id: i.id}))), 'bin instances == json');
}

/* ---- 9. props -------------------------------------------------------------- */
section('props');
{
  const req = {mines: ['ore_cart', 'rail_segment', 'bunk', 'crate', 'barrel', 'lantern', 'pickaxe_rack', 'table'],
               ruins: ['altar', 'statue', 'sarcophagus', 'brazier', 'candle_cluster', 'broken_column', 'bones', 'urn', 'banner'],
               caves: ['glow_mushroom_patch', 'bone_pile', 'crystal_cluster', 'campfire_remains', 'rope_ladder']};
  for (const k in req) for (const n of req[k]) ok(!!G.PROPS[k + '/' + n], 'prop ' + k + '/' + n + ' exists');
  for (const id of G.PROP_IDS) {
    const P = G.PROPS[id];
    for (const pn of Object.keys(P.PRESETS)) {
      const a = P.generate(P.PRESETS[pn].params, 5), b = P.generate(P.PRESETS[pn].params, 5);
      ok(J(a.cells) === J(b.cells) && a.cells.length > 0, id + '/' + pn + ': deterministic, ' + a.cells.length + ' cells');
      for (const k of G.STYLES) {
        const pal = G.resolvePalette({style: k, palette: {}}, k);
        const t = G.bakeProp(id, P.PRESETS[pn].params, 5, (m) => m[0] === '@' ? (pal[m.slice(1)] || pal.wall) : m);
        if (k === 'mines') ok(t.size.every(n => n <= 256), id + '/' + pn + ': template ' + t.size.join('x') + ' fits 256');
        const missing = t.palette.filter(m => m !== '@carve' && !MAT.has(m));
        ok(missing.length === 0, id + '/' + pn + ' in ' + k + ': materials resolve ' + (missing.length ? J(missing) : ''));
      }
    }
  }
}

/* ---- 10. the sample -------------------------------------------------------- */
section('the sample dungeon');
{
  const prog = G.compile(sample, {id: '_d2_sample', sourceText: sampleText, materials: NAMES});
  ok(prog.errors.length === 0, '_d2_sample compiles with no errors ' + J(prog.errors.slice(0, 3)));
  const kits = new Set(sample.pieces.map(p => p.kit));
  ok(kits.size === 3, '_d2_sample mixes all three kits');
  ok(['adit_entrance', 'mouth', 'ruin_entrance'].every(t => sample.pieces.some(p => p.type === t)), '_d2_sample has each style\'s entrance');
  const v = G.validate(sample, {materials: NAMES});
  ok(!v.some(i => i.severity === 'error'), '_d2_sample validates with no errors ' + J(v.filter(i => i.severity === 'error').slice(0, 3)));
  const shellPath = join(ROOT, 'assets/dungeons/_d2_sample/shell.json');
  ok(existsSync(shellPath) && readFileSync(shellPath, 'utf8') === G.emitShellJson(prog), '_d2_sample/shell.json is current (else: node scripts/compile_dungeon.mjs _d2_sample)');
  const binPath = join(ROOT, 'assets/dungeons/_d2_sample/shell.bin');
  const want2 = G.emitShellBin(prog);
  ok(existsSync(binPath) && Buffer.compare(readFileSync(binPath), Buffer.from(want2)) === 0, '_d2_sample/shell.bin is current');
}

console.log('\n' + (fails ? 'FAILED ' + fails + ' of ' + count : 'all ' + count + ' checks passed'));
process.exit(fails ? 1 : 0);
