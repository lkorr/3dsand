/* place_dungeon.mjs — put a dungeon (or a variant) on a map, checked against
 * THAT map's real ground (PLAN_dungeons "Portability").
 *
 *   node scripts/place_dungeon.mjs --map <name> --dungeon <id> --at x,z [--yaw 0|90|180|270] [--yOffset v]
 *        [--id <placement id>] [--name "<name>"] [--ground-y <y>] [--write]
 *        [--set <piece>.<param>=<value> ... --as <variant id>]
 *
 * What it does:
 *   1. reads assets/dungeons/<id>/dungeon.json (a variant is resolved against its base);
 *   2. asks the engine for the map's ground over the dungeon's footprint:
 *      `sandvox.exe --heightmap` with SANDVOX_MAP=<map>, run through scripts/run.sh
 *      (GPU-free; SANDVOX_EXE picks the exe, default build/Release/sandvox.exe);
 *   3. prints, per piece, the least rock over it, and the validation panel the
 *      Dungeons tab shows "in world" (surface breaches, carves under water, plus
 *      everything map-free: sockets, reachability, keys, signals, limits);
 *   4. with --write, adds or updates ONLY that map's dungeons[] entry <id>
 *      (map.json is otherwise left byte for byte).
 *
 * The anchor: map placement puts the entrance socket at (at.x, BareGroundHeight
 * + yOffset, at.z). --heightmap reports the terrain column (roads, sites and
 * pads included), so where a road or a site cuts the anchor column the two
 * differ: pass the bare ground with --ground-y (the Dungeons tab's in-world
 * view, voxserve DUNGEON PLACE, prints it as groundY).
 *
 * Map-tuned sliders (an apron, a cairn's lift, a key-light shaft's height) are
 * piece sliders: --set p3.keyHeight=48 --as teeth_caves_north writes a VARIANT
 * assets/dungeons/teeth_caves_north/dungeon.json (extends the dungeon, those
 * params changed), compiles it, and places the variant. Re-running with more
 * --set edits that variant.
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'fs';
import { spawnSync } from 'child_process';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const ASSETS = join(ROOT, 'assets');
const G = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/dungeongen.js')).href);
const E = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/ui_entities.js')).href);
const MATS = JSON.parse(readFileSync(join(ASSETS, 'materials/materials.json'), 'utf8')).materials.map(m => m.id);

const argv = process.argv.slice(2);
const opt = (k, def) => { const i = argv.indexOf(k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : def; };
const flag = (k) => argv.includes(k);
const sets = []; argv.forEach((a, i) => { if (a === '--set' && i + 1 < argv.length) sets.push(argv[i + 1]); });
const MAP = opt('--map'), DID = opt('--dungeon'), AT = opt('--at');
if (!MAP || !DID || !AT) {
  console.error('usage: node scripts/place_dungeon.mjs --map <name> --dungeon <id> --at x,z [--yaw q] [--yOffset v] [--id pid] [--name n] [--ground-y y] [--write] [--set piece.param=v ... --as <variant>]');
  process.exit(2);
}
const at = AT.split(',').map(Number);
const yaw = Number(opt('--yaw', 0)), yOffset = Number(opt('--yOffset', 0));
if (yaw % 90 !== 0) { console.error('--yaw: a quarter turn'); process.exit(2); }
const mapFile = join(ASSETS, 'worldmap', MAP, 'map.json');
if (!existsSync(mapFile)) { console.error('no map ' + mapFile); process.exit(2); }
const loadBase = (id) => { const f = join(ASSETS, 'dungeons', id, 'dungeon.json'); return existsSync(f) ? JSON.parse(readFileSync(f, 'utf8')) : null; };

// ---- --set ... --as <variant>: write / update a variant of the dungeon -------------
let dungeonId = DID;
if (sets.length) {
  const vid = opt('--as');
  if (!vid || !/^[a-z0-9_-]+$/.test(vid)) { console.error('--set needs --as <variant id> ([a-z0-9_-])'); process.exit(2); }
  const vfile = join(ASSETS, 'dungeons', vid, 'dungeon.json');
  const v = existsSync(vfile) ? JSON.parse(readFileSync(vfile, 'utf8')) : {version: 1, extends: DID, about: 'placed on map ' + MAP + ' by scripts/place_dungeon.mjs'};
  if (v.extends !== DID && vid !== DID) { console.error(vid + ' exists and extends ' + v.extends + ', not ' + DID); process.exit(2); }
  v.pieces = v.pieces || {};
  for (const s of sets) {
    const m = /^([A-Za-z0-9_-]+)\.([A-Za-z0-9_]+)=(.*)$/.exec(s);
    if (!m) { console.error('--set ' + s + ': want <piece>.<param>=<value>'); process.exit(2); }
    const val = m[3] === 'true' ? true : m[3] === 'false' ? false : isFinite(Number(m[3])) && m[3] !== '' ? Number(m[3]) : m[3];
    v.pieces[m[1]] = v.pieces[m[1]] || {};
    v.pieces[m[1]].params = Object.assign({}, v.pieces[m[1]].params || {}, {[m[2]]: val});
  }
  mkdirSync(dirname(vfile), {recursive: true});
  writeFileSync(vfile, G.dungeonJsonText(v));
  const r = spawnSync(process.execPath, [join(ROOT, 'scripts/compile_dungeon.mjs'), vid], {encoding: 'utf8'});
  process.stdout.write(r.stdout); process.stderr.write(r.stderr);
  if (r.status !== 0) process.exit(1);
  dungeonId = vid;
  console.log('variant ' + vid + ' (extends ' + DID + '): ' + sets.join(', '));
}

const raw = loadBase(dungeonId);
if (!raw) { console.error('no assets/dungeons/' + dungeonId + '/dungeon.json'); process.exit(2); }
const d = G.isVariant(raw) ? G.resolveDungeon(raw, loadBase).dungeon : G.normalizeDungeon(raw);
const prog = G.compile(raw, {id: dungeonId, materials: MATS, loadBase});
for (const e of prog.errors) console.log('  COMPILE ERROR ' + (e.piece || '-') + ': ' + e.msg);

// ---- the ground --------------------------------------------------------------------
const q = ((yaw / 90) % 4 + 4) % 4;
const toWorld = (x, z) => { const r = [[x, z], [z, -x], [-x, -z], [-z, x]][q]; return [at[0] + r[0], at[1] + r[1]]; };
/** Both heightmaps (the footprint, the anchor column at one voxel) in ONE
 *  run.sh launch: one wait for the GPU lock, not two. */
function heightmaps(jobs) {
  const exe = process.env.SANDVOX_EXE || join(ROOT, 'build', 'Release', 'sandvox.exe');
  const outs = jobs.map(j => join(ROOT, 'build', 'place_' + MAP + '_' + j.tag + '.bin'));
  mkdirSync(join(ROOT, 'build'), {recursive: true});
  const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";
  const script = jobs.map((j, i) => q(exe) + ' --heightmap ' + [j.cx, j.cz, j.span, j.res].join(',') + ' --heightmap-out ' + q(outs[i])).join(' && ');
  const env = Object.assign({}, process.env, {SANDVOX_ASSET_DIR: ASSETS, SANDVOX_MAP: MAP, SANDVOX_NO_CRASH_DIALOG: '1'});
  const r = spawnSync('bash', [join(ROOT, 'scripts', 'run.sh'), 'bash', '-c', script], {env, encoding: 'utf8'});
  if (r.status !== 0 || outs.some(o => !existsSync(o))) { console.error('heightmap failed: ' + (r.stderr || r.stdout || r.error)); process.exit(1); }
  return outs.map(readMap);
}
function readMap(out) {
  const buf = readFileSync(out);
  const n = buf.readUInt32LE(8), sp = buf.readUInt32LE(12), hx = buf.readInt32LE(16), hz = buf.readInt32LE(20);
  return (x, z) => {
    const i = Math.floor((x - (hx - sp / 2)) / sp * n), j = Math.floor((z - (hz - sp / 2)) / sp * n);
    if (i < 0 || j < 0 || i >= n || j >= n) return null;
    const o = 48 + (j * n + i) * 8, h = buf.readInt32LE(o), w = buf.readInt16LE(o + 4);
    return {ground: h, water: w === -32768 ? null : h + w};
  };
}
const b = prog.bounds;
const cs = [[b.min[0], b.min[2]], [b.max[0], b.min[2]], [b.min[0], b.max[2]], [b.max[0], b.max[2]]].map(c => toWorld(c[0], c[1]));
const xs = cs.map(c => c[0]), zs = cs.map(c => c[1]);
const span = Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...zs) - Math.min(...zs)) + 256;
const [H, HA] = heightmaps([
  {tag: 'area', cx: Math.round((Math.min(...xs) + Math.max(...xs)) / 2), cz: Math.round((Math.min(...zs) + Math.max(...zs)) / 2), span, res: Math.min(1024, Math.ceil(span / 2))},
  {tag: 'anchor', cx: at[0], cz: at[1], span: 16, res: 16}]);
const g0 = HA(at[0], at[1]);
const groundY = opt('--ground-y') !== undefined ? Number(opt('--ground-y')) : g0.ground;
const AY = groundY + yOffset;
console.log(dungeonId + ' on ' + MAP + ' at (' + at.join(', ') + ') yaw ' + yaw + ': anchor y ' + AY + ' (' + (opt('--ground-y') !== undefined ? 'bare ground ' + groundY + ' from --ground-y' : 'terrain column ' + g0.ground + (g0.water !== null ? ', water ' + g0.water : '')) + ', yOffset ' + yOffset + ')');
const groundAt = (x, z) => { const w = toWorld(x + 0.5, z + 0.5); const r = H(w[0], w[1]); return r ? r.ground - AY : Infinity; };
const waterAt = (x, z) => { const w = toWorld(x + 0.5, z + 0.5); const r = H(w[0], w[1]); return r && r.water !== null ? r.water - AY : null; };
for (const pc of prog.pieces) {
  let worst = Infinity, wat = null;
  for (let x = pc.bounds.min[0]; x <= pc.bounds.max[0]; x += 8) for (let z = pc.bounds.min[2]; z <= pc.bounds.max[2]; z += 8) {
    const c = groundAt(x, z) - pc.bounds.max[1];
    if (c < worst) { worst = c; wat = [x, z]; }
  }
  const label = (d.pieces.find(p => p.id === pc.id) || {}).label || pc.type;
  console.log('  ' + pc.id.padEnd(4) + String(label).padEnd(18) + ' y ' + String(pc.bounds.min[1]).padStart(5) + '..' + String(pc.bounds.max[1]).padStart(5) +
              '   least rock over its box top ' + (worst / 10).toFixed(1).padStart(6) + ' m' + (pc.surface ? '  (surface piece)' : '') + '  at ' + wat);
}

// Key-light shafts (a map-tuned slider): where each one comes out against the
// ground over it, and the keyHeight that would make it meet the ground.
for (const pc of d.pieces) {
  if (!pc.params || pc.params.keyLight !== 'shaft') continue;
  const pl = G.placePiece(d, pc);
  const A = prog.entrance.dungeonPos;
  for (const p of pl.prims) {
    if (p.op !== 'carve' || p.tag !== 'apron') continue;
    const top = p.pts ? p.pts[p.pts.length - 1] : [Math.round((p.min[0] + p.max[0]) / 2), p.max[1], Math.round((p.min[2] + p.max[2]) / 2)];
    const ax = top[0] - A[0], ay = top[1] - A[1], az = top[2] - A[2];
    const g = groundAt(ax, az);
    if (!isFinite(g)) continue;
    const want = +(pc.params.keyHeight + (g - ay) / 10 + 1).toFixed(1);
    console.log('  ' + pc.id + ' key-light shaft: top ' + (ay / 10).toFixed(1) + ' m, ground over it ' + (g / 10).toFixed(1) + ' m (' + (ay >= g ? 'open to the sky' : 'ENDS ' + ((g - ay) / 10).toFixed(1) + ' m UNDER the ground: a blind chimney') +
                '); keyHeight ' + pc.params.keyHeight + ' -> ' + want + ' reaches 1 m past the ground');
  }
}

// ---- the panel ------------------------------------------------------------------
const refsFile = join(ASSETS, 'dungeons', dungeonId, d.refs || 'refs.json');
const refs = existsSync(refsFile) ? E.readRefs(readFileSync(refsFile, 'utf8')).refs : [];
const ij = JSON.parse(readFileSync(join(ASSETS, 'items/items.json'), 'utf8'));
const keyItems = (ij.items || ij).filter(it => it.kind === 'key').map(it => ({id: it.id, unlocks: it.unlocks || it.id}));
const panel = G.validate(d, {materials: MATS, groundAt, waterAt, refs: E.forValidate(refs, {keys: keyItems}), keyItems});
const nErr = panel.filter(v => v.severity === 'error').length, nWarn = panel.filter(v => v.severity === 'warn').length;
for (const v of panel) if (v.severity !== 'info') console.log('  ' + v.severity.padEnd(5) + ' ' + v.code.padEnd(11) + ' ' + (v.piece || '-') + ': ' + v.msg);
console.log('validation on ' + MAP + ': ' + nErr + ' errors, ' + nWarn + ' warnings');

// ---- map.json dungeons[] ----------------------------------------------------------
if (flag('--write')) {
  if (nErr) { console.error('not written: fix the errors first (or place it elsewhere)'); process.exit(1); }
  const pid = opt('--id', dungeonId);
  const old = (JSON.parse(readFileSync(mapFile, 'utf8')).dungeons || []).find(e => e.id === pid) || {};
  // an existing entry keeps its other fields (an `about`, say); these are set
  const entry = Object.assign({}, old, {id: pid, name: opt('--name', old.name || d.name), dungeon: dungeonId, at, yOffset, yaw});
  let mt = readFileSync(mapFile, 'utf8');
  const eol = mt.includes('\r\n') ? '\r\n' : '\n';
  const j = JSON.parse(mt);
  const line = '    ' + JSON.stringify(entry).replace(/,"/g, ', "').replace(/":/g, '": ').replace(/^\{/, '{ ').replace(/\}$/, ' }');
  const lines = mt.split('\n').map(l => l.replace(/\r$/, ''));
  if (Array.isArray(j.dungeons) && j.dungeons.some(e => e.id === pid)) {
    // Replace the existing entry, whatever its layout (one line or several).
    const open = lines.findIndex(l => /^  "dungeons": \[/.test(l));
    let close = open; while (close < lines.length && !/^  \],?\s*$/.test(lines[close])) close++;
    const others = j.dungeons.map(e => e.id === pid ? entry : e);
    const body = others.map((e, i) => (e === entry ? line : '    ' + JSON.stringify(e).replace(/,"/g, ', "').replace(/":/g, '": ').replace(/^\{/, '{ ').replace(/\}$/, ' }')) + (i + 1 < others.length ? ',' : ''));
    lines.splice(open + 1, close - open - 1, ...body);
  } else if (Array.isArray(j.dungeons)) {
    const open = lines.findIndex(l => /^  "dungeons": \[/.test(l));
    if (/\]\s*,?\s*$/.test(lines[open])) { const tail = lines[open].trimEnd().endsWith(',') ? ',' : ''; lines.splice(open, 1, '  "dungeons": [', line, '  ]' + tail); }
    else { let close = open; while (close < lines.length && !/^  \],?\s*$/.test(lines[close])) close++; lines[close - 1] = lines[close - 1].replace(/\s*$/, '') + ','; lines.splice(close, 0, line); }
  } else {
    const loc = lines.findIndex(l => /^  "locales": \[/.test(l));
    let end = loc; while (end < lines.length && !/^  \],?\s*$/.test(lines[end])) end++;
    lines.splice(end + 1, 0, '  "dungeons": [', line, '  ],');
  }
  mt = lines.join(eol);
  JSON.parse(mt);
  writeFileSync(mapFile, mt);
  console.log('map.json (' + MAP + ') dungeons[] ' + pid + ': ' + line.trim());
}
process.exit(nErr ? 1 : 0);
