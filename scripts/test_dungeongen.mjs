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
              ruins: ['ruin_entrance', 'hall', 'corridor', 'turn', 'junction', 'stair', 'crypt', 'pit_room', 'sanctum', 'flooded_hall', 'collapsed_passage']};
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
    ok(!!T.surface === !!r.surface || t === 'shaft' || r.params.keyLight === 'shaft', tag + ': surface flag');
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
  const noFoot = (r) => r.prims.filter(p => p.tag !== 'footing').map(J);
  ok(J(noFoot(m2)) === J(noFoot(m6)), 'cavern: mushroom count leaves the shell untouched (but for the patch footings)');
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
  let tag = U32();
  const ext = [];
  if (tag === 0x584D5250) {                       // PRMX: the 3.4.1 extras
    const nx = U32();
    if (nx !== np) throw new Error('PRMX count ' + nx + ' != prims ' + np);
    for (let i = 0; i < nx; i++) { const fl = U32(), m2 = U32(), ck = I32(); U32(); ext.push({openOnly: (fl & 1) === 1, mat2: S(m2), checker: ck}); }
    tag = U32();
  }
  if (tag !== 0x21444E45) throw new Error('no END!');
  out.extras = ext;
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
  ok(existsSync(shellPath) && readFileSync(shellPath, 'utf8').replace(/\r/g, '') === G.emitShellJson(prog), '_d2_sample/shell.json is current (else: node scripts/compile_dungeon.mjs _d2_sample)');
  const binPath = join(ROOT, 'assets/dungeons/_d2_sample/shell.bin');
  const want2 = G.emitShellBin(prog);
  ok(existsSync(binPath) && Buffer.compare(readFileSync(binPath), Buffer.from(want2)) === 0, '_d2_sample/shell.bin is current');
}

/* ---- 11. D8 variety sliders --------------------------------------------------- */
section('D8 variety: every new slider has a visible, deterministic effect and keeps the sockets');
{
  const gen = (k, t, params, seed) => G.getType(k, t).generate(params, seed || 7);
  const sk = (r) => J(r.sockets.map(s => [s.name, s.pos, s.dir, s.w, s.h]));
  const room = (r) => { let bb = null; for (const p of r.prims) if (p.op === 'carve' && p.tag !== 'apron') bb = U.boxUnion(bb, U.shapeBounds(p)); return bb; };
  const carveCount = (r, f) => r.prims.filter(p => p.op === 'carve' && (!f || f(p))).length;
  /** slider `key` from `lo` to `hi` on k/t (base params): the prims change, twice the same, sockets as given. */
  const effect = (k, t, key, lo, hi, base, sockets) => {
    const a = gen(k, t, Object.assign({}, base || {}, {[key]: lo})), b = gen(k, t, Object.assign({}, base || {}, {[key]: hi})), b2 = gen(k, t, Object.assign({}, base || {}, {[key]: hi}));
    ok(J(a.prims) !== J(b.prims), k + '/' + t + ': ' + key + ' ' + lo + ' -> ' + hi + ' changes the shell');
    ok(J(b.prims) === J(b2.prims) && J(b.props) === J(b2.props), k + '/' + t + ': ' + key + ' ' + hi + ' generates the same twice');
    if (sockets !== false) ok(sk(a) === sk(b), k + '/' + t + ': ' + key + ' leaves every socket where it was');
    return [a, b];
  };
  // wander: sockets stay put, the carve widens sideways with the amplitude (monotone)
  for (const [k, t, base] of [['mines', 'drift', {length: 24}], ['mines', 'collapse', {length: 20}], ['ruins', 'corridor', {length: 24, alcoves: 0}],
                              ['caves', 'tunnel', {length: 24, meander: 0}], ['caves', 'crawl', {length: 14, meander: 0}], ['mines', 'adit_entrance', {length: 20}]]) {
    const r0 = gen(k, t, Object.assign({}, base, {wander: 0})), r1 = gen(k, t, Object.assign({}, base, {wander: 1.5})), r2 = gen(k, t, Object.assign({}, base, {wander: 3.5}));
    ok(sk(r0) === sk(r1) && sk(r0) === sk(r2), k + '/' + t + ': wander leaves every socket where it was');
    const w = (r) => { const b = room(r); return b.max[0] - b.min[0]; };
    ok(w(r0) < w(r1) && w(r1) <= w(r2), k + '/' + t + ': wander 0 -> 1.5 -> 3.5 m widens the swept carve (' + [w(r0), w(r1), w(r2)].join(' -> ') + ')');
    ok(J(gen(k, t, Object.assign({}, base, {wander: 2})).prims) === J(gen(k, t, Object.assign({}, base, {wander: 2})).prims), k + '/' + t + ': wander deterministic');
  }
  effect('mines', 'drift', 'wanderWave', 8, 30, {length: 24, wander: 2});
  // slope: the far socket rises by exactly the slope, the near one stays
  for (const [k, t, far] of [['mines', 'drift', 'out'], ['ruins', 'corridor', 'out'], ['mines', 'collapse', 'out'], ['caves', 'cavern', 'out']]) {
    const a = gen(k, t, {slope: 0}), b = gen(k, t, {slope: 2.3});
    const fa = a.sockets.find(s => s.name === far), fb = b.sockets.find(s => s.name === far);
    ok(fb.pos[1] - fa.pos[1] === 23 && J(a.sockets.find(s => s.name === 'in').pos) === J(b.sockets.find(s => s.name === 'in').pos),
       k + '/' + t + ': slope 2.3 m lifts the far socket by 23 voxels (' + fa.pos[1] + ' -> ' + fb.pos[1] + '), the origin stays');
  }
  effect('ruins', 'corridor', 'slopeSteps', 0, 3, {slope: 2, length: 20}, false);
  effect('mines', 'drift', 'slopeSteps', 0, 2, {slope: 1.5, length: 20}, false);
  // width / height variation: the shell changes, the ends (sockets) do not
  for (const [k, t] of [['mines', 'drift'], ['ruins', 'corridor'], ['caves', 'crawl'], ['caves', 'tunnel'], ['ruins', 'collapsed_passage']]) {
    effect(k, t, 'widthVar', 0, 0.5, {length: 24});
    effect(k, t, 'heightVar', 0, 0.5, {length: 24});
  }
  // jitter: the repeated elements move (sets, ribs, columns, loculi), sockets do not
  {
    const zs = (r) => r.prims.filter(p => p.op === 'fill' && p.mat === 'support' && p.shape === 'box').map(p => p.min[2]);
    const [a, b] = effect('mines', 'drift', 'jitter', 0, 1, {length: 24, broken: 0, sag: 0});
    ok(J(zs(a)) !== J(zs(b)), 'drift: jitter moves the timber sets');
    effect('ruins', 'corridor', 'jitter', 0, 1, {length: 24, archSpacing: 2.5});
    effect('ruins', 'hall', 'jitter', 0, 1, {columnCount: 6});
    effect('ruins', 'crypt', 'jitter', 0, 1, {});
    effect('mines', 'stope', 'jitter', 0, 1, {});
    effect('mines', 'incline', 'jitter', 0, 1, {length: 20});
    ok(J(gen('mines', 'drift', {jitter: 0}).prims) === J(gen('mines', 'drift', {}).prims), 'drift: jitter 0 is the regular drift');
  }
  // bays + lobe: more carves / a bigger room, the doors stay
  for (const [k, t] of [['caves', 'cavern'], ['mines', 'stope'], ['mines', 'camp'], ['ruins', 'hall'], ['ruins', 'crypt'], ['ruins', 'sanctum'], ['caves', 'grotto_pool'], ['ruins', 'pit_room'], ['mines', 'flooded_sump']]) {
    const r0 = gen(k, t, {bays: 0}), r3 = gen(k, t, {bays: 3});
    ok(carveCount(r3) > carveCount(r0) && sk(r0) === sk(r3), k + '/' + t + ': bays 0 -> 3 add carves (' + carveCount(r0) + ' -> ' + carveCount(r3) + '), sockets kept');
    const l0 = room(gen(k, t, {lobe: 0})), l1 = room(gen(k, t, {lobe: 0.8}));
    ok((l1.max[0] - l1.min[0]) > (l0.max[0] - l0.min[0]), k + '/' + t + ': lobe 0 -> 0.8 widens the room (' + (l0.max[0] - l0.min[0]) + ' -> ' + (l1.max[0] - l1.min[0]) + ')');
  }
  // a bay never cuts a side door: a 3-exit cavern / an all-exits hall with many bays still validates its sockets
  {
    const r = gen('caves', 'cavern', {exits: 3, bays: 6, lobe: 1});
    const side = r.sockets.filter(s => s.name === 'left' || s.name === 'right');
    ok(side.length === 2, 'cavern exits 3 + bays 6: both side sockets present');
  }
  // key lights
  {
    const sh = gen('caves', 'cavern', {keyLight: 'shaft', keyHeight: 30});
    const tube = sh.prims.find(p => p.tag === 'apron' && p.op === 'carve');
    ok(sh.surface && tube && U.shapeBounds(tube).max[1] >= 290, 'cavern keyLight shaft: a carve to 30 m, the piece becomes a surface piece');
    const fire = gen('mines', 'stope', {keyLight: 'fire'}), crys = gen('ruins', 'hall', {keyLight: 'crystals'});
    ok(fire.prims.some(p => p.mat === 'candle_flame' && p.tag === 'keylight') && !fire.surface, 'stope keyLight fire: a bed of flame, not a surface piece');
    ok(crys.prims.some(p => p.mat === 'crystal' && p.tag === 'keylight'), 'hall keyLight crystals: crystal prisms');
    const vol = (r) => r.prims.filter(p => p.tag === 'keylight' && p.mat === 'candle_flame').reduce((a, p) => { const b = U.shapeBounds(p); return a + (b.max[0] - b.min[0]) * (b.max[1] - b.min[1]) * (b.max[2] - b.min[2]); }, 0);
    ok(vol(gen('mines', 'stope', {keyLight: 'fire', keySize: 0.9})) > vol(gen('mines', 'stope', {keyLight: 'fire', keySize: 0.1})), 'keySize grows the fire');
    ok(J(gen('ruins', 'hall', {keyLight: 'none'}).prims) === J(gen('ruins', 'hall', {}).prims), 'keyLight none is the plain room');
  }
  // light power: bigger emitters; light density: more lights in a big room
  {
    const fl = (r) => r.prims.filter(p => p.tag === 'light' && (p.mat === 'light' || p.mat === 'lightCool')).reduce((a, p) => { const b = U.shapeBounds(p); return a + (b.max[0] - b.min[0]) * (b.max[1] - b.min[1]) * (b.max[2] - b.min[2]); }, 0);
    ok(fl(gen('ruins', 'corridor', {lightType: 'torch', lightPower: 1})) > 4 * fl(gen('ruins', 'corridor', {lightType: 'torch', lightPower: 0})), 'torch: lightPower 1 is a flame over 4x the volume of lightPower 0');
    const cr = (r) => r.prims.filter(p => p.tag === 'light').reduce((a, p) => a + (p.r || 0), 0);
    ok(cr(gen('caves', 'cavern', {lightType: 'crystal', lightPower: 1})) > cr(gen('caves', 'cavern', {lightType: 'crystal', lightPower: 0.2})), 'crystal: lightPower grows the lumps');
    const lp = gen('mines', 'drift', {lightPower: 0.8}).props.filter(p => p.light);
    ok(lp.length && lp.every(p => p.params.glow === 0.8), 'lantern lights carry lightPower as the lantern\'s glow');
    const nl = (r) => r.refs.filter(x => x.kind === 'light').length;
    const big = {width: 30, length: 36, lightCount: 2};
    ok(nl(gen('caves', 'cavern', Object.assign({lightDensity: 1}, big))) > nl(gen('caves', 'cavern', Object.assign({lightDensity: 0}, big))) &&
       nl(gen('caves', 'cavern', Object.assign({lightDensity: 0}, big))) === 2, 'lightDensity adds lights by floor area (0 = exactly the count)');
    const a = gen('ruins', 'hall', {lightCount: 3, lightDensity: 0}), b = gen('ruins', 'hall', {lightCount: 3});
    ok(J(a.refs) === J(b.refs), 'lightDensity 0 keeps the mount list (and the seeded picks) of the plain piece');
  }
  // anchored lights: on a rough wall a crystal lump is rooted behind the mount
  {
    const r = gen('caves', 'cavern', {lightType: 'crystal', lightPower: 0.5, wallRough: 1, floorUneven: 1});
    const lumps = r.prims.filter(p => p.tag === 'light' && p.shape === 'capsule');
    ok(lumps.length > 0 && lumps.every(p => p.a[1] < p.b[1]), 'crystal lights on a rough floor are capsules rooted below the mount (' + lumps.length + ')');
  }
  // kit sliders
  effect('caves', 'tunnel', 'xsec', 0, 0.8, {length: 20});
  effect('caves', 'tunnel', 'canyon', 0, 0.8, {length: 20});
  effect('caves', 'tunnel', 'pockets', 0, 3, {length: 20});
  effect('caves', 'tunnel', 'breakdown', 0, 0.8, {length: 20});
  {
    const [, b] = effect('mines', 'drift', 'vein', 0, 0.8, {length: 20});
    ok(b.prims.some(p => p.op === 'skin' && p.tag === 'vein' && p.mat === 'ore'), 'drift vein: an ore SKIN band (never a fill in the passage)');
  }
  effect('ruins', 'hall', 'settle', 0, 0.8, {});
  effect('ruins', 'hall', 'breach', 0, 0.8, {});
  effect('ruins', 'corridor', 'breach', 0, 0.8, {length: 20});
  effect('ruins', 'crypt', 'settle', 0, 0.8, {});
  {
    const r = gen('ruins', 'hall', {settle: 0.8});
    ok(r.prims.some(p => p.tag === 'settle' && p.shape === 'ramp' && p.mask === 'air'), 'settle: tilted slabs are open-space-only ramps');
  }
  // ruins turn / junction: the out socket is at the end of the out arm, facing away
  {
    const tl = gen('ruins', 'turn', {side: 'left'}), tr = gen('ruins', 'turn', {side: 'right'});
    ok(tl.sockets.find(s => s.name === 'out').dir[0] === -1 && tr.sockets.find(s => s.name === 'out').dir[0] === 1, 'ruins turn: left turns to -x, right to +x');
    const jx = gen('ruins', 'junction', {shape: 'X'});
    ok(['in', 'left', 'right', 'out'].every(n => jx.sockets.some(s => s.name === n)), 'ruins junction X: four sockets');
  }
}

/* ---- 12. compiler extras + fractional flights + doors -------------------------- */
section('compiler: openOnly, checker, door refs, fractional flights');
{
  const one = (k, t, params) => { const d = G.newDungeon({name: 'x', style: k, seed: 3}); d.pieces.push(G.newPiece(d, {kit: k, type: t, params})); return d; };
  const pr = G.compile(one('mines', 'collapse', {}), {id: 'x', materials: NAMES});
  ok(pr.prims.some(p => p.op === 'fill' && p.openOnly && p.mat === 'gravel'), 'a rubble heap (mask air) compiles to an openOnly fill');
  ok(pr.prims.every(p => !p.openOnly || p.op === 'fill'), 'openOnly only on fills');
  const ck = G.compile(one('ruins', 'hall', {tilePattern: 'checker', tileSize: 0.8}), {id: 'x', materials: NAMES});
  const cp = ck.prims.find(p => p.checker);
  ok(cp && cp.mat2 === 'chalk_rock' && cp.checker === 8 && cp.op === 'fill', 'a checker floor compiles to mat2 + checker (' + (cp ? cp.mat + '/' + cp.mat2 + ' ' + cp.checker : 'none') + ')');
  const bin = readBin(G.emitShellBin(ck));
  const ex = bin.extras || [];
  ok(ex.length === ck.prims.length && ck.prims.every((p, i) => ex[i].openOnly === !!p.openOnly && ex[i].mat2 === p.mat2 && ex[i].checker === (p.checker || 0)), 'the PRMX block reads back to the json extras');
  const stripped = Object.assign({}, ck, {prims: ck.prims.map(p => { const q = Object.assign({}, p); delete q.openOnly; delete q.mat2; delete q.checker; return q; })});
  ok(readBin(G.emitShellBin(stripped)).extras.length === 0, 'no extras -> no PRMX block (END! straight after the pieces)');
  // the door ref: hinge cell + width covers the opening, in the tagged piece
  {
    let d = G.newDungeon({name: 'door', style: 'ruins', seed: 4});
    d.pieces.push(G.newPiece(d, {kit: 'ruins', type: 'corridor', params: {width: 2.4}}));
    d = G.attachPiece(d, {piece: 'p0', socket: 'out'}, {kit: 'ruins', type: 'crypt', params: {doorWidth: 2.2}}).dungeon;
    const p = G.compile(d, {id: 'door', materials: NAMES});
    const door = p.refs.find(r => r.kind === 'door');
    const s = p.sockets.find(q => q.piece === 'p1' && q.name === 'in');
    const w = door.props.width;
    // yaw = the tagged socket's facing; along = HeadingAxis(yaw + 90)
    const along = U.rotV(U.quarter(door.yaw + 90), [0, 0, 1]);
    const cells = Array.from({length: w}, (_, i) => door.pos[0] + along[0] * i);
    const lo = Math.min(...cells), hi = Math.max(...cells);
    ok(lo === s.pos[0] - Math.floor(s.w / 2) && hi === lo + w - 1, 'door ref: the leaf from the hinge cell covers the opening exactly (' + lo + '..' + hi + ' for an opening centred on ' + s.pos[0] + ', ' + s.w + ' wide)');
  }
  // fractional flights: a run that does not divide reaches the far socket with no flat remainder
  for (const L of [11.3, 12.7, 20.9]) {
    const r = G.getType('mines', 'incline').generate({mode: 'stairs', rise: 3.1, length: L, stepHeight: 0.2}, 1);
    const ramps = r.prims.filter(p => p.shape === 'ramp' && p.op === 'carve');
    const flats = r.prims.filter(p => p.tag === 'flat');
    const run = ramps.reduce((a, p) => a + (p.max[2] - p.min[2]), 0);
    ok(flats.length === 0 && run === Math.round(L * 10), 'incline ' + L + ' m: the treads share the whole run (' + run + ' cells, ' + ramps.length + ' ramps, no flat landing)');
  }
}

/* ---- 13. variants (dungeon inheritance) ------------------------------------------ */
section('variants: extends, piece edits, removals, cycles, staleness');
{
  const base = JSON.parse(sampleText);
  const bases = {base: base};
  const load = (id) => bases[id] || null;
  // palette only
  const v1 = {version: 1, extends: 'base', palette: {variant: 'basalt', kits: {ruins: {variant: 'dark'}}}};
  const r1 = G.resolveDungeon(v1, load);
  ok(J(r1.dungeon.pieces) === J(G.normalizeDungeon(base).pieces) && r1.dungeon.palette.variant === 'basalt' && r1.dungeon.palette.kits.ruins.variant === 'dark', 'palette-only variant: same pieces, palette merged');
  const c1 = G.compile(v1, {id: 'v1', materials: NAMES, loadBase: load});
  ok(c1.errors.length === 0 && c1.source.extends === 'base' && J(c1.source.chain) === J(['base']), 'palette-only variant compiles clean, its shell says what it extends');
  // piece params
  const pid = base.pieces[1].id;
  const v2 = {version: 1, extends: 'base', pieces: {[pid]: {params: {width: base.pieces[1].params.width + 0.6}}}};
  const r2 = G.resolveDungeon(v2, load).dungeon;
  ok(r2.pieces.find(p => p.id === pid).params.width === Math.round((base.pieces[1].params.width + 0.6) * 10) / 10 &&
     J(r2.pieces.filter(p => p.id !== pid)) === J(G.normalizeDungeon(base).pieces.filter(p => p.id !== pid)), 'piece-param variant: only that piece\'s param moved');
  // removed branch: the piece and everything only reachable through it go, links to it are cut
  const nb = G.normalizeDungeon(base);
  const leafPar = nb.pieces.find(p => p.links && Object.keys(p.links).length && p.id !== (nb.entrance && nb.entrance.piece));
  const v3 = {version: 1, extends: 'base', pieces: {[leafPar.id]: {remove: 'branch'}}};
  const r3 = G.resolveDungeon(v3, load);
  ok(!r3.dungeon.pieces.some(p => p.id === leafPar.id) && r3.removed.length >= 1 && r3.dungeon.pieces.every(p => !p.links || Object.values(p.links).every(L => r3.dungeon.pieces.some(q => q.id === L.piece))),
     'removed-branch variant: ' + r3.removed.length + ' piece(s) gone, no link points at them');
  const v3v = G.validate(v3, {loadBase: load});
  ok(!v3v.some(i => i.severity === 'error' && i.code === 'link'), 'removed-branch variant validates without dangling links');
  // cycle refusal + depth
  const cyc = {a: {version: 1, extends: 'b'}, b: {version: 1, extends: 'a'}};
  let threw = '';
  try { G.resolveDungeon(cyc.a, (id) => cyc[id]); } catch (e) { threw = e.message; }
  ok(/cycle/.test(threw), 'a variant cycle is refused (' + threw + ')');
  const deep = {}; for (let i = 0; i < 12; i++) deep['d' + i] = {version: 1, extends: i === 11 ? 'base' : 'd' + (i + 1)}; deep.base = base;
  threw = ''; try { G.resolveDungeon(deep.d0, (id) => deep[id]); } catch (e) { threw = e.message; }
  ok(/deeper than/.test(threw), 'a chain deeper than ' + G.VARIANT_MAX_DEPTH + ' is refused');
  // a chain of two resolves to the same as the flattened edits
  const v4 = {version: 1, extends: 'v2', palette: {variant: 'basalt'}};
  bases.v2 = v2;
  const r4 = G.resolveDungeon(v4, load);
  ok(J(r4.chain) === J(['base', 'v2']) && r4.dungeon.pieces.find(p => p.id === pid).params.width === r2.pieces.find(p => p.id === pid).params.width, 'a variant of a variant flattens the chain');
  // staleness: the variant's own bytes are the engine's sourceHash; its resolvedHash moves when the BASE moves
  const t1 = G.compile(v2, {id: 'v2', loadBase: load, sourceText: G.dungeonJsonText(v2)});
  const base2 = JSON.parse(sampleText); base2.pieces[0].params.decay = (base2.pieces[0].params.decay || 0) + 0.1;
  const t2 = G.compile(v2, {id: 'v2', loadBase: (id) => id === 'base' ? base2 : null, sourceText: G.dungeonJsonText(v2)});
  ok(t1.sourceHash === t2.sourceHash && t1.source.resolvedHash !== t2.source.resolvedHash && G.emitShellJson(t1) !== G.emitShellJson(t2),
     'base change: the variant\'s sourceHash (its own bytes) stays, its resolvedHash and shell move -> --check flags it');
  // refs: waynodes re-adopted, refs in removed pieces dropped, patch applied
  const prog3 = G.compile(v3, {id: 'v3', loadBase: load});
  const baseProg = G.compile(base, {id: 'base'});
  const wn = (n) => 'wn_' + n.replace(/\//g, '_');
  const removedPiece = r3.removed[0];
  const inside = removedPiece.waynodes[0].pos.map((v, i) => v - baseProg.entrance.dungeonPos[i]);
  const bRefs = baseProg.waynodes.map(w => ({name: wn(w.name), kind: 'waynode', pos: w.pos, yaw: 0, props: {links: w.links.map(wn)}}))
    .concat([{name: 'chest_gone', kind: 'container', pos: inside, yaw: 0, props: {}}, {name: 'keep_me', kind: 'marker', pos: [0, 0, 0], yaw: 0, props: {tags: ['a']}}]);
  const rr = G.resolveVariantRefs(bRefs, Object.assign({}, v3, {refsPatch: {set: {keep_me: {props: {tags: ['b']}}}, add: [{name: 'new_one', kind: 'marker', pos: [1, 0, 1]}]}}), prog3, r3.removed, prog3.entrance.dungeonPos);
  ok(!rr.refs.some(r => r.name === 'chest_gone') && rr.refs.find(r => r.name === 'keep_me').props.tags[0] === 'b' && rr.refs.some(r => r.name === 'new_one'),
     'variant refs: a ref in a removed piece dropped, refsPatch set + add applied');
  ok(rr.refs.filter(r => r.kind === 'waynode').length === prog3.waynodes.filter(w => !w.pit).length, 'variant refs: the waynodes are re-adopted from the variant\'s own geometry');
  // the shipped example variants are current
  for (const vid of ['shardwell_flooded', 'teeth_caves_ice', 'heather_cairn_overgrown']) {
    const f = join(ROOT, 'assets/dungeons', vid, 'dungeon.json');
    if (!ok(existsSync(f), 'example variant ' + vid + ' exists')) continue;
    const text = readFileSync(f, 'utf8'), vj = JSON.parse(text);
    const loadDisk = (id) => { const p = join(ROOT, 'assets/dungeons', id, 'dungeon.json'); return existsSync(p) ? JSON.parse(readFileSync(p, 'utf8')) : null; };
    const prog = G.compile(vj, {id: vid, sourceText: text, materials: NAMES, loadBase: loadDisk});
    ok(G.isVariant(vj) && prog.errors.length === 0, vid + ': a variant of ' + vj.extends + ', compiles clean');
    ok(readFileSync(join(ROOT, 'assets/dungeons', vid, 'shell.json'), 'utf8').replace(/\r/g, '') === G.emitShellJson(prog), vid + ': shell.json current');
    const vv = G.validate(vj, {materials: NAMES, loadBase: loadDisk});
    ok(!vv.some(i => i.severity === 'error'), vid + ': validates with no errors ' + J(vv.filter(i => i.severity === 'error').slice(0, 2)));
  }
}

console.log('\n' + (fails ? 'FAILED ' + fails + ' of ' + count : 'all ' + count + ' checks passed'));
process.exit(fails ? 1 : 0);
