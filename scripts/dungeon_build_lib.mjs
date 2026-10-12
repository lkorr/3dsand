/* dungeon_build_lib.mjs — what the example dungeons' build scripts
 * (assets/dungeons/<id>/build.mjs) share. A build script is MAP-FREE: it
 * writes assets/dungeons/<id>/{dungeon.json, shell.json, shell.bin,
 * refs.json} and the schedules its creatures walk, and validates the dungeon
 * ALONE (sockets, reachability, keys, signals, limits, materials, refs).
 * Placing it on a map -- and the real-ground checks (surface breach, water) --
 * is scripts/place_dungeon.mjs's job (PLAN_dungeons "Portability").
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
export const ASSETS = join(ROOT, 'assets');
export const G = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/dungeongen.js')).href);
export const E = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/ui_entities.js')).href);
export const U = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/dgutil.js')).href);
export const MATS = JSON.parse(readFileSync(join(ASSETS, 'materials/materials.json'), 'utf8')).materials.map(m => m.id);

/** The tab's pickers (tuner_server.py _dungeon_catalog), from disk. */
export function catalog() {
  const stems = (p, ext) => existsSync(p) ? readdirSync(p).filter(n => n.endsWith(ext)).map(n => n.slice(0, -ext.length)) : [];
  const mdir = join(ASSETS, 'mobs');
  const subs = readdirSync(mdir).filter(s => !['effects', 'pool'].includes(s) && !s.startsWith('.') && statSync(join(mdir, s)).isDirectory());
  let mobs = stems(mdir, '.vox');
  for (const s of subs) mobs = mobs.concat(stems(join(mdir, s), '.vox'));
  for (const dd of [mdir].concat(subs.map(s => join(mdir, s)))) for (const st of stems(dd, '.json')) {
    try { if ('extends' in JSON.parse(readFileSync(join(dd, st + '.json'), 'utf8'))) mobs.push(st); } catch (e) { /* not a mob sidecar */ }
  }
  const beh = JSON.parse(readFileSync(join(mdir, 'behaviors.json'), 'utf8')).profiles.map(p => p.name).filter(Boolean);
  const ij = JSON.parse(readFileSync(join(ASSETS, 'items/items.json'), 'utf8'));
  const keys = (ij.items || ij).filter(it => it.kind === 'key').map(it => ({id: it.id, unlocks: it.unlocks || it.id}));
  return {loot: stems(join(ASSETS, 'loot'), '.json'), schedules: stems(join(ASSETS, 'schedules'), '.json'), mobs: [...new Set(mobs)].sort(), behaviors: beh, keys};
}

/** Write the creatures' schedules (assets/schedules/, shared files). */
export function writeSchedules(SCHED) {
  for (const [name, s] of Object.entries(SCHED)) {
    const L = ['{', '  "about": ' + JSON.stringify(s.about) + ',', '  "rows": ['];
    s.rows.forEach((r, i) => L.push('    ' + JSON.stringify(r).replace(/,"/g, ', "').replace(/":/g, '": ') + (i + 1 < s.rows.length ? ',' : '')));
    L.push('  ]', '}');
    writeFileSync(join(ASSETS, 'schedules', name + '.json'), L.join('\n') + '\n');
  }
}

/** Placed pieces' prims, for open-floor tests. */
export function placedAll(d) { return d.pieces.map(p => G.placePiece(d, p)); }
/** Is the cell column above `pos` (dungeon frame) open for h cells: inside a
 *  carve, clear of every standing fill (not rubble / spoil / light / footing)? */
export function freeAt(placed, pos, h) {
  for (let dy = 1; dy <= (h || 6); dy++) {
    const q = [pos[0] + 0.5, pos[1] + dy + 0.5, pos[2] + 0.5];
    let inCarve = false, blocked = false;
    for (const pc of placed) for (const p of pc.prims) {
      if (p.op === 'carve' && !p.mat && U.primContains(p, q)) inCarve = true;
      if (p.op === 'fill' && !['rubble', 'spoil', 'floor'].includes(p.mat) && !['light', 'footing', 'settle'].includes(p.tag) && U.primContains(p, q)) blocked = true;
    }
    if (!inCarve || blocked) return false;
  }
  return true;
}
/** Drop `pos` onto the floor below it (un-noised JS shapes): the lowest open
 *  cell of the open run that contains (or is just above) pos, searching
 *  `span` cells up and down. A sloped or terraced room's floor is not at its
 *  waynode's height everywhere; this is where a prop actually stands. */
export function dropToFloor(placed, pos, span) {
  span = span || 40;
  const open = (y) => {
    const q = [pos[0] + 0.5, y + 0.5, pos[2] + 0.5];
    let inCarve = false, filled = false;
    for (const pc of placed) for (const p of pc.prims) {
      if (p.op === 'carve' && U.primContains(p, q)) inCarve = !p.mat || inCarve;
      if (p.op === 'fill' && U.primContains(p, q) && !['light', 'footing'].includes(p.tag)) filled = true;
    }
    return inCarve && !filled;
  };
  let y = pos[1];
  if (!open(y)) { let k = 1; while (k <= span && !open(y + k)) k++; if (k > span) return pos.slice(); y += k; }
  while (y > pos[1] - span && open(y - 1)) y--;
  return [pos[0], y, pos[2]];
}

/** `pos` if it is open, else the nearest open cell within `reach` (same y). */
export function openFloor(placed, what, pos, h, reach, verbose) {
  reach = reach || 24;
  for (let r = 0; r <= reach; r++) for (let dx = -r; dx <= r; dx++) for (let dz = -r; dz <= r; dz++) {
    if (Math.max(Math.abs(dx), Math.abs(dz)) !== r) continue;
    const q = [pos[0] + dx, pos[1], pos[2] + dz];
    if (freeAt(placed, q, h)) { if (r && verbose) console.log('  (' + what + ' moved ' + [dx, dz] + ' to open floor)'); return q; }
  }
  throw new Error(what + ' at ' + pos + ': no open floor within ' + reach + ' cells');
}

/** Write dungeon.json + shell + refs.json; returns the compiled program. */
export function writeDungeon(id, d, refs, about) {
  const OUT = join(ASSETS, 'dungeons', id);
  mkdirSync(OUT, {recursive: true});
  d.refs = 'refs.json';
  const text = G.dungeonJsonText(d);
  writeFileSync(join(OUT, 'dungeon.json'), text);
  const prog = G.compile(JSON.parse(text), {id, sourceText: text, materials: MATS});
  writeFileSync(join(OUT, 'shell.json'), G.emitShellJson(prog));
  writeFileSync(join(OUT, 'shell.bin'), G.emitShellBin(prog));
  writeFileSync(join(OUT, 'refs.json'), E.writeRefs(id, about, refs));
  return prog;
}

/** The validation panel the tab shows (map-free): dungeongen.validate + the
 *  refs rules. Prints it and a one-line summary; returns the error count. */
export function report(id, d, refs, prog) {
  const cat = catalog();
  const issues = G.validate(d, {materials: MATS, refs: E.forValidate(refs, cat), keyItems: cat.keys})
    .concat(E.refProblems(refs, cat, new Set(prog.waynodes.map(w => w.name))));
  for (const r of refs) if ((r.kind === 'npc' || r.kind === 'spawner') && r.props.behavior && !cat.behaviors.includes(r.props.behavior))
    issues.push({severity: 'error', code: 'ref', ref: r.name, msg: 'no behaviour ' + r.props.behavior});
  for (const v of issues) if (v.severity !== 'info') console.log('  ' + v.severity.padEnd(5) + ' ' + (v.code || '').padEnd(11) + ' ' + (v.piece || v.ref || '-') + ': ' + v.msg);
  const ext = [0, 1, 2].map(i => prog.bounds.max[i] - prog.bounds.min[i] + 1);
  const nErr = issues.filter(v => v.severity === 'error').length, nWarn = issues.filter(v => v.severity === 'warn').length;
  console.log(id + ': ' + d.pieces.length + ' pieces, ' + prog.prims.length + ' prims, ' + prog.props.instances.length + ' prop instances, ' + refs.length + ' refs; extent ' +
              ext.map(v => (v / 10).toFixed(1) + ' m').join(' x ') + '; worst 64^3 cell ' + prog.limits.primsPerCell + ' prims / ' + prog.limits.instancesPerCell +
              ' instances; ' + nErr + ' errors, ' + nWarn + ' warnings (map-free; ground: node scripts/place_dungeon.mjs)');
  for (const e of prog.errors) console.log('  COMPILE ERROR ' + (e.piece || '-') + ': ' + e.msg);
  return nErr + prog.errors.length;
}

/** Waynode refs: the tab's "Adopt waynodes" (wn_<piece>_<node>), pits left out. */
export function adoptWaynodes(prog, ref) {
  const wn = (n) => 'wn_' + n.replace(/\//g, '_');
  const pits = new Set(prog.waynodes.filter(w => w.pit).map(w => w.name));
  for (const w of prog.waynodes) if (!w.pit) ref(wn(w.name), 'waynode', w.pos, 0, {links: w.links.filter(l => !pits.has(l)).map(wn)});
  return wn;
}
