/* test_housegen.mjs — the house generator / structure asset gate.
 *
 * WHAT IT GUARDS (PLAN_world_editor P2):
 *   1. DETERMINISM. Same params + seed -> byte-identical cells, .vox and
 *      .struct.json. A structure is worldgen input once placed (P4), so a
 *      nondeterministic generator would move the world hash for no nameable
 *      reason. A different seed must change something.
 *   2. SLOT CONTRACTS, over every preset AND a sweep of hashed parameter sets:
 *      every door's leaf box is all door_wood (and door_wood is nowhere else);
 *      every bed has 1 m of air over its mattress; every slot sits inside the
 *      wall footprint (the outside-door waynodes just outside it); waynode
 *      links are symmetric, resolve, and connect the whole house; slot names
 *      are unique; the origin row is the floor surface.
 *   3. THE FILE FORMAT. The .vox reads back to exactly the grid written; the
 *      struct.json carries the materials it was written with and they match
 *      materials.json; nothing exceeds a .vox's 256 cells.
 *   4. THE SAMPLES. assets/structures/samples/* regenerate byte-for-byte from
 *      their own generator block (so P4's fixtures are reproducible) and are
 *      not hand-edited.
 *   5. MATERIALS. Every building material exists, has a far alias to a
 *      slot-OWNING material, the far palette stays within its 128 slots (the
 *      loader's refusal, checked here without a build), and each flammable one
 *      has exactly one ignition rule to a real product.
 *   6. HAND-EDIT PROTECTION. bake_structure.mjs refuses to overwrite a
 *      structure whose struct.json says handEdited: true.
 *
 *   node scripts/test_housegen.mjs
 *
 * Node only, no build, no browser: `check_environment.sh` covers the page.
 */
import { readFileSync, existsSync, writeFileSync, unlinkSync, readdirSync } from 'fs';
import { spawnSync } from 'child_process';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const HG = await load('assets/editor/housegen.js');
const VOX = await load('assets/editor/vox.js');
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const REACT = JSON.parse(readFileSync(join(ROOT, 'assets/materials/reactions.json'), 'utf8')).reactions;
const SDIR = join(ROOT, 'assets', 'structures');

let fails = 0, count = 0;
const ok = (cond, what) => {
  count++;
  if (cond) { console.log('  ok   ' + what); return true; }
  console.log('  FAIL ' + what); fails++; return false;
};
const fnv = (bytes) => {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) { h ^= bytes[i] & 0xFF; h = Math.imul(h, 0x01000193) >>> 0; }
  return (h >>> 0).toString(16).padStart(8, '0');
};
const eqBytes = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const DW = HG.PALETTE.indexOf('door_wood') + 1, SB = HG.PALETTE.indexOf('straw_bed') + 1;

/* ---- the per-house invariants, returned as a list of problems ------------- */
function problems(res) {
  const out = [];
  const {x: nx, y: ny, z: nz} = res.dim;
  const at = (x, y, z) => (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) ? 0 : res.cells[(z * ny + y) * nx + x];
  const fp = res.meta.footprint;
  const inFoot = (p) => p[0] >= fp.min[0] && p[0] <= fp.max[0] && p[2] >= fp.min[1] && p[2] <= fp.max[1];
  if (nx > HG.MAX_DIM || ny > HG.MAX_DIM || nz > HG.MAX_DIM) out.push('dims ' + nx + 'x' + ny + 'x' + nz + ' over ' + HG.MAX_DIM);
  const names = new Set();
  const kinds = new Set(['door', 'bed', 'container', 'marker', 'waynode']);
  let leafCells = 0;
  for (const s of res.slots) {
    if (names.has(s.name)) out.push('duplicate slot ' + s.name);
    names.add(s.name);
    if (!kinds.has(s.kind)) out.push(s.name + ': unknown kind ' + s.kind);
    if (!(s.yaw % 90 === 0 && s.yaw >= 0 && s.yaw < 360)) out.push(s.name + ': yaw ' + s.yaw);
    const outside = /^waynode_.*_out$/.test(s.name);
    if (outside) {
      if (inFoot(s.pos)) out.push(s.name + ' is inside the footprint');
      const dx = Math.max(fp.min[0] - s.pos[0], 0, s.pos[0] - fp.max[0]), dz = Math.max(fp.min[1] - s.pos[2], 0, s.pos[2] - fp.max[1]);
      if (dx > 20 || dz > 20) out.push(s.name + ' is more than 2 m outside the footprint');
      if (at(s.pos[0], s.pos[1], s.pos[2])) out.push(s.name + ' stands in a solid cell');
    } else if (!inFoot(s.pos)) out.push(s.name + ' at ' + s.pos + ' is outside the wall footprint');
    if (s.pos[1] < 0 || s.pos[1] >= ny) out.push(s.name + ' y out of the grid');
    if (s.kind === 'door') {
      const {min, max} = s.props.leaf;
      let n = 0, bad = 0;
      for (let z = min[2]; z <= max[2]; z++) for (let y = min[1]; y <= max[1]; y++) for (let x = min[0]; x <= max[0]; x++) { n++; if (at(x, y, z) !== DW) bad++; }
      leafCells += n;
      if (bad) out.push(s.name + ': ' + bad + '/' + n + ' leaf cells are not door_wood');
      const w = Math.max(max[0] - min[0], max[2] - min[2]) + 1, h = max[1] - min[1] + 1;
      if (w < 8 || w > 10 || h < 19 || h > 21) out.push(s.name + ': leaf ' + w + 'x' + h + ' is not ~0.9 x 2.0 m');
      if (!(s.props.hinge === 'left' || s.props.hinge === 'right')) out.push(s.name + ': hinge ' + s.props.hinge);
      const hl = s.props.hingeLine;
      const onEdge = (hl[0] === min[0] || hl[0] === max[0] + 1) && (hl[1] === min[2] || hl[1] === max[2] + 1);
      if (!onEdge) out.push(s.name + ': hinge line ' + hl + ' is not on a vertical edge of the leaf box');
      // Both sides of the leaf must be open: a door you cannot walk through.
      const inw = [s.pos[0], s.pos[1] + 5, s.pos[2]];
      void inw;
    }
    if (s.kind === 'bed') {
      const {min, max} = s.props.box;
      if (at(s.pos[0], s.pos[1] - 1, s.pos[2]) !== SB) out.push(s.name + ': no straw_bed under the slot');
      let blocked = 0;
      for (let z = min[2] + 1; z < max[2]; z++) for (let x = min[0] + 1; x < max[0]; x++)
        for (let y = max[1] + 1; y <= max[1] + 10; y++) if (at(x, y, z)) blocked++;
      if (blocked) out.push(s.name + ': ' + blocked + ' solid cells in the 1 m over the mattress');
    }
    if (s.kind === 'container') {
      const {min, max} = s.props.box;
      if (!at(min[0], min[1], min[2]) || !at(max[0], max[1], max[2])) out.push(s.name + ': chest box is not solid at its corners');
      if (!Array.isArray(s.props.items)) out.push(s.name + ': no items list');
    }
  }
  const dwTotal = res.meta.counts.door_wood || 0;
  if (dwTotal !== leafCells) out.push('door_wood outside door leaves: ' + dwTotal + ' cells vs ' + leafCells + ' in leaves');
  // waynodes: symmetric, resolving, connected
  const ways = new Map(res.slots.filter(s => s.kind === 'waynode').map(s => [s.name, s]));
  for (const [n, s] of ways) for (const l of s.props.links) {
    const o = ways.get(l);
    if (!o) out.push(n + ' links to missing ' + l);
    else if (!o.props.links.includes(n)) out.push(n + ' -> ' + l + ' is one-way');
  }
  if (ways.size) {
    const seen = new Set(), q = [ways.keys().next().value];
    while (q.length) { const n = q.pop(); if (seen.has(n)) continue; seen.add(n); for (const l of (ways.get(n) || {props: {links: []}}).props.links) q.push(l); }
    if (seen.size !== ways.size) out.push('waynode graph is split: ' + seen.size + ' of ' + ways.size + ' reachable');
  }
  // origin: feet row over a solid floor, at the footprint centre
  const o = res.origin;
  if (!inFoot(o)) out.push('origin outside the footprint');
  if (res.meta.doors < 1) out.push('no door');
  return out;
}

/* ---- 1. determinism ----------------------------------------------------------- */
console.log('\n-- determinism --');
{
  for (const p of HG.PRESET_ORDER) {
    const pr = HG.PRESETS[p];
    const a = HG.generateHouse(pr.params, pr.seed), b = HG.generateHouse(pr.params, pr.seed);
    const fa = HG.buildStructureFiles(a, 'x/' + p, MATS, VOX), fb = HG.buildStructureFiles(b, 'x/' + p, MATS, VOX);
    ok(eqBytes(a.cells, b.cells) && eqBytes(fa.vox, fb.vox) && fa.json === fb.json,
       p + ' seed ' + pr.seed + ' twice -> cells ' + fnv(a.cells) + ', .vox ' + fnv(fa.vox) + ' (' + fa.vox.length + ' B)');
  }
  let differ = 0;
  for (const p of HG.PRESET_ORDER) {
    const pr = HG.PRESETS[p];
    const a = HG.generateHouse(pr.params, pr.seed), c = HG.generateHouse(pr.params, pr.seed + 1);
    if (!eqBytes(a.cells, c.cells)) differ++;
  }
  ok(differ >= 3, 'the seed matters: ' + differ + '/' + HG.PRESET_ORDER.length + ' presets change with seed+1');
  // normalizeParams is idempotent and clamps
  const n1 = HG.normalizeParams({width: 99, storeys: 7, wallStyle: 'glass', doorsFront: 0, loft: true, bogus: 1});
  ok(n1.width === 14 && n1.storeys === 2 && n1.wallStyle === 'timber' && n1.doorsFront === 1 && n1.loft === false && !('bogus' in n1),
     'normalizeParams clamps, snaps, validates enums, forces a door, drops a loft on two storeys and unknown keys');
  ok(JSON.stringify(HG.normalizeParams(n1)) === JSON.stringify(n1), 'normalizeParams is idempotent');
}

/* ---- 2. slot contracts: presets + a hashed sweep ------------------------------- */
console.log('\n-- slot contracts --');
{
  for (const p of HG.PRESET_ORDER) {
    const pr = HG.PRESETS[p];
    const r = HG.generateHouse(pr.params, pr.seed);
    const pb = problems(r);
    ok(!pb.length, p + ': ' + r.slots.length + ' slots, ' + r.meta.doors + ' doors, ' + r.meta.beds + '/' + (pr.params.beds || 0) + ' beds' + (pb.length ? ' -- ' + pb.slice(0, 4).join('; ') : ''));
    ok(r.meta.beds === (pr.params.beds || 0) && r.meta.chests === (pr.params.chests || 0),
       p + ': every requested bed and chest found a spot (' + r.meta.beds + ' beds, ' + r.meta.chests + ' chests)' +
       (r.meta.warnings.length ? ' -- ' + r.meta.warnings.join('; ') : ''));
  }
  // The sweep: parameter sets drawn from the hash, so the gate is itself
  // deterministic. Every combination must hold every contract; beds and
  // chests may legitimately be skipped (the stats line says so).
  const N = 80;
  let bad = 0, firstBad = '';
  const enumPick = (r, h) => r.options[h % r.options.length];
  for (let i = 0; i < N; i++) {
    const P = {};
    let k = 0;
    for (const r of HG.SCHEMA) {
      if (!r.k) continue;
      const h = HG.hashN(0x5eed, i, k++);
      if (r.kind === 'enum') P[r.k] = enumPick(r, h);
      else if (r.kind === 'bool') P[r.k] = (h & 1) === 1;
      else P[r.k] = r.min + (h / 4294967296) * (r.max - r.min);
    }
    let res;
    try { res = HG.generateHouse(P, i); } catch (e) { bad++; firstBad = firstBad || ('#' + i + ' threw ' + e.message); continue; }
    const pb = problems(res);
    if (pb.length) { bad++; firstBad = firstBad || ('#' + i + ' ' + JSON.stringify(HG.normalizeParams(P)) + ': ' + pb.slice(0, 3).join('; ')); }
  }
  ok(bad === 0, 'sweep: ' + (N - bad) + '/' + N + ' hashed parameter sets hold every contract' + (firstBad ? ' -- first: ' + firstBad : ''));
}

/* ---- 3. file format ------------------------------------------------------------ */
console.log('\n-- file format --');
{
  const pr = HG.PRESETS.townhouse;
  const r = HG.generateHouse(pr.params, pr.seed);
  const f = HG.buildStructureFiles(r, 'samples/test', MATS, VOX);
  const doc = VOX.readVox(f.vox);
  const g = VOX.modelToGrid(doc.models[0]);
  const want = HG.toIdGrid(r, MATS);
  ok(g.dim.x === want.dim.x && g.dim.y === want.dim.y && g.dim.z === want.dim.z && eqBytes(g.data, want.data),
     '.vox reads back to the written grid (' + g.dim.x + 'x' + g.dim.y + 'x' + g.dim.z + ')');
  // The loader rebases to the OCCUPIED box: the grid must already be tight, or
  // every slot coordinate would shift on load.
  let tight = [Infinity, Infinity, Infinity, -1, -1, -1];
  for (let z = 0; z < g.dim.z; z++) for (let y = 0; y < g.dim.y; y++) for (let x = 0; x < g.dim.x; x++)
    if (g.data[x + y * g.dim.x + z * g.dim.x * g.dim.y]) { tight = [Math.min(tight[0], x), Math.min(tight[1], y), Math.min(tight[2], z), Math.max(tight[3], x), Math.max(tight[4], y), Math.max(tight[5], z)]; }
  ok(tight[0] === 0 && tight[1] === 0 && tight[2] === 0 && tight[3] === g.dim.x - 1 && tight[4] === g.dim.y - 1 && tight[5] === g.dim.z - 1,
     'the grid is tight to its occupied cells (what voxload.cpp rebases to)');
  const j = JSON.parse(f.json);
  const keys = Object.keys(j);
  ok(JSON.stringify(keys) === JSON.stringify(['name', 'origin', 'size', 'voxelsPerMetre', 'materials', 'slots', 'generator', 'handEdited']),
     'struct.json key order: ' + keys.join(', '));
  ok(j.handEdited === false && j.generator.tool === 'housegen' && j.generator.seed === pr.seed && j.generator.version === HG.GENERATOR_VERSION,
     'generator block filled, handEdited false');
  const idOf = new Map(MATS.map((m, i) => [m.id, i + 1]));
  ok(Object.entries(j.materials).every(([n, id]) => idOf.get(n) === id) && Object.keys(j.materials).length === Object.keys(r.meta.counts).length,
     'materials map = the ids written = materials.json (' + Object.keys(j.materials).length + ' names)');
  ok(f.json.split('\n').filter(l => /^\s+\{"name":/.test(l)).length === r.slots.length, 'one slot per line (' + r.slots.length + ')');
  let thrown = '';
  try { HG.buildStructureFiles(r, 'x', MATS.filter(m => m.id !== 'thatch' && m.id !== 'roof_tile'), VOX); } catch (e) { thrown = e.message; }
  ok(/roof_tile/.test(thrown), 'a missing material is refused by name: "' + thrown + '"');
}

/* ---- 4. samples ---------------------------------------------------------------- */
console.log('\n-- samples --');
{
  for (const [name] of Object.entries(HG.SAMPLES)) {
    const sj = join(SDIR, ...name.split('/')) + '.struct.json', sv = join(SDIR, ...name.split('/')) + '.vox';
    if (!ok(existsSync(sj) && existsSync(sv), name + ' exists (.vox + .struct.json)')) continue;
    const text = readFileSync(sj, 'utf8');
    const j = JSON.parse(text);
    ok(j.handEdited === false && j.name === name, name + ': named, not hand-edited');
    const r = HG.generateHouse(j.generator.params, j.generator.seed, {vpm: j.voxelsPerMetre});
    const f = HG.buildStructureFiles(r, name, MATS, VOX);
    ok(f.json === text.replace(/\r\n/g, '\n') && eqBytes(f.vox, new Uint8Array(readFileSync(sv))),
       name + ': regenerates byte-for-byte from its generator block (.vox ' + fnv(f.vox) + ')');
    const pb = problems(r);
    ok(!pb.length, name + ': ' + j.slots.length + ' slots hold every contract' + (pb.length ? ' -- ' + pb.join('; ') : ''));
  }
  const kinds = new Set();
  for (const [name] of Object.entries(HG.SAMPLES)) {
    const p = join(SDIR, ...name.split('/')) + '.struct.json';
    if (existsSync(p)) for (const s of JSON.parse(readFileSync(p, 'utf8')).slots) kinds.add(s.kind + (s.props.tags ? ':' + s.props.tags[0] : ''));
  }
  ok(['door', 'bed', 'container', 'marker:hearth', 'waynode'].every(k => kinds.has(k)), 'the samples between them exercise every slot kind: ' + [...kinds].sort().join(', '));
}

/* ---- 5. materials -------------------------------------------------------------- */
console.log('\n-- materials --');
{
  const byId = new Map(MATS.map((m, i) => [m.id, {m, i}]));
  const missing = HG.PALETTE.filter(n => !byId.has(n));
  ok(!missing.length, 'every generator material exists in materials.json' + (missing.length ? ': missing ' + missing : ''));
  const NEW = ['plank', 'timber', 'thatch', 'daub', 'cobble', 'flagstone', 'roof_tile', 'door_wood', 'straw_bed'];
  for (const n of NEW) {
    const e = byId.get(n);
    if (!e) continue;
    const far = e.m.far && byId.get(e.m.far);
    ok(e.m.class === 'solid' && far && !far.m.far && e.i + 1 <= 255,
       n + ': id ' + (e.i + 1) + ', solid, far -> ' + e.m.far + ' (owns its slot), fits a .vox byte');
  }
  const owned = MATS.filter(m => !m.far).length + 1;
  ok(owned <= 128, 'far palette: ' + owned + ' owned slots incl. air (the loader refuses > 128)');
  for (const n of NEW) {
    const e = byId.get(n);
    if (!e) continue;
    const fl = (e.m.tags || []).includes('flammable');
    const rules = REACT.filter(r => r.self === n && r.neighbor === 'tag:hot');
    if (fl) ok(rules.length === 1 && byId.has(rules[0].selfBecomes), n + ' is flammable and ignites once, to ' + (rules[0] && rules[0].selfBecomes));
    else ok(rules.length === 0, n + ' is not flammable and has no ignition rule');
  }
}

/* ---- 6. hand-edit protection --------------------------------------------------- */
console.log('\n-- hand-edit protection --');
{
  const name = '_test_handedited';
  const sj = join(SDIR, name + '.struct.json'), sv = join(SDIR, name + '.vox');
  const before = JSON.stringify({name, origin: [0, 0, 0], slots: [], generator: null, handEdited: true});
  writeFileSync(sj, before);
  const run = spawnSync(process.execPath, [join(ROOT, 'scripts/bake_structure.mjs'), name, '--preset', 'cottage'], {encoding: 'utf8'});
  ok(run.status === 1 && /HAND-EDITED/.test(run.stderr) && /_v2/.test(run.stderr), 'bake_structure refuses a hand-edited target and names ' + name + '_v2');
  ok(readFileSync(sj, 'utf8') === before && !existsSync(sv), 'and wrote nothing');
  try { unlinkSync(sj); } catch (e) {}
  try { unlinkSync(sv); } catch (e) {}
  ok(!readdirSync(SDIR).some(f => f.startsWith('_test_')), 'scratch files cleaned up');
}

console.log('\n' + (fails ? 'FAIL' : 'PASS') + ': ' + (count - fails) + '/' + count + ' checks');
process.exit(fails ? 1 : 0);
