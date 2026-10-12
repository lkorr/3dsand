/* compile_dungeon.mjs — dungeon.json -> shell.json + shell.bin, PLAN_dungeons
 * §3.4 / §3.4.1 (the engine's schema: D1 amendment).
 *
 *   node scripts/compile_dungeon.mjs <id>          compile assets/dungeons/<id>/
 *   node scripts/compile_dungeon.mjs --all         every assets/dungeons/<id>/dungeon.json
 *   node scripts/compile_dungeon.mjs <id> --check  exit 1 if shell.json / shell.bin are stale (no write)
 *   node scripts/compile_dungeon.mjs <id> --validate   also print the validation panel
 *
 * Uses the SAME assets/editor/dungeon/dungeongen.js the tuner's Dungeons tab
 * runs, so the two produce byte-identical files. The shell's header carries
 * the FNV-1a of the dungeon.json bytes, CRs dropped (sourceHash): the
 * engine's dungeon-format gate compares it to tell a stale shell, and holds
 * shell.bin to shell.json by CONTENT (`sandvox.exe --dungeon-bake <id>`
 * rewrites the .bin with the engine's own writer if the two ever disagree).
 *
 * A dungeon placed on a map is worldgen input: recompiling one moves the
 * world hash (one `--selftest --rebaseline`, a notification, not a regression).
 */
import { readFileSync, writeFileSync, existsSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const DG = await import(pathToFileURL(join(ROOT, 'assets/editor/dungeon/dungeongen.js')).href);
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials.map(m => m.id);
const DIR = join(ROOT, 'assets', 'dungeons');

const args = process.argv.slice(2);
const flag = (f) => args.includes(f);
const ids = flag('--all')
  ? readdirSync(DIR, {withFileTypes: true}).filter(e => e.isDirectory() && existsSync(join(DIR, e.name, 'dungeon.json'))).map(e => e.name).sort()
  : args.filter(a => !a.startsWith('--'));
if (!ids.length) { console.error('usage: node scripts/compile_dungeon.mjs <id>|--all [--check] [--validate]'); process.exit(2); }

let bad = 0;
for (const id of ids) {
  const dir = join(DIR, id), src = join(dir, 'dungeon.json');
  if (!existsSync(src)) { console.error(id + ': no ' + src); bad++; continue; }
  const text = readFileSync(src, 'utf8');
  let d;
  try { d = JSON.parse(text); } catch (e) { console.error(id + ': dungeon.json does not parse: ' + e.message); bad++; continue; }
  const prog = DG.compile(d, {id, sourceText: text, materials: MATS});
  const json = DG.emitShellJson(prog);
  const bin = DG.emitShellBin(prog);
  const out = join(dir, 'shell.json'), outBin = join(dir, 'shell.bin');
  const nScatter = prog.prims.reduce((a, p) => a + (p.scatter ? p.scatter.length : 0), 0);
  const line = id + ': ' + prog.pieces.length + ' pieces, ' + prog.prims.length + ' prims, ' + nScatter + ' scatter rules, ' +
               prog.props.templates.length + ' prop templates / ' + prog.props.instances.length + ' instances, ' + prog.waynodes.length + ' waynodes, ' +
               prog.refs.length + ' ref suggestions; bounds ' + JSON.stringify(prog.bounds) + '; worst 64^3 cell ' + prog.limits.primsPerCell + ' prims / ' +
               prog.limits.instancesPerCell + ' instances; sourceHash ' + prog.sourceHash;
  for (const e of prog.errors) console.error('  ERROR ' + (e.piece || '-') + ': ' + e.msg);
  if (prog.errors.length) bad++;
  if (flag('--check')) {
    const cur = existsSync(out) ? readFileSync(out, 'utf8') : null;
    const curBin = existsSync(outBin) ? readFileSync(outBin) : null;
    const binOk = curBin && Buffer.compare(curBin, Buffer.from(bin)) === 0;
    if (cur !== json || !binOk) { console.error(id + ': shell ' + (cur !== json ? 'json' : 'bin') + ' is STALE (run node scripts/compile_dungeon.mjs ' + id + ')'); bad++; }
    else console.log(id + ': shell.json + shell.bin current');
  } else {
    writeFileSync(out, json);
    writeFileSync(outBin, bin);
    console.log(line + '; shell.bin ' + bin.length + ' B');
  }
  if (flag('--validate')) for (const v of DG.validate(d, {materials: MATS})) console.log('  ' + v.severity.padEnd(5) + ' ' + v.code.padEnd(11) + ' ' + (v.piece || '-') + ': ' + v.msg);
}
process.exit(bad ? 1 : 0);
