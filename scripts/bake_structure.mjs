/* bake_structure.mjs — author-side bake: house parameters -> a structure asset.
 *
 * Writes assets/structures/<name>.vox + <name>.struct.json (PLAN_world_editor
 * §2.2) with the SAME generator the tuner's Environment -> Structures page
 * runs (assets/editor/housegen.js), so a terminal and the page produce
 * byte-identical files. The page is the human surface; this is the headless
 * one (an agent's build, a fresh checkout's samples, CI).
 *
 *   node scripts/bake_structure.mjs <name> params.json [--seed N]
 *        a params file: either bare schema params, or {"seed": n, "params": {...}}
 *   node scripts/bake_structure.mjs <name> --preset smithy [--seed N]
 *        a housegen.js preset (cottage, longhouse, smithy, alehouse, townhouse)
 *   node scripts/bake_structure.mjs <name>
 *        RE-BAKE from the generator block already in <name>.struct.json
 *   node scripts/bake_structure.mjs --samples
 *        regenerate assets/structures/samples/* (the P4 fixtures)
 *   add --stats to generate and report without writing anything
 *
 * <name> may carry one folder: "samples/smithy", "harrowby_smithy".
 *
 * NEVER OVERWRITES A HAND-EDITED STRUCTURE. If <name>.struct.json says
 * "handEdited": true, the .vox is the truth (PLAN §2.2) and this refuses,
 * naming the next free "<name>_vN" to bake to instead. There is no flag to
 * override it: delete the files by hand if you really mean it.
 *
 * THE SCALE comes from src/sim/world.h (kVoxelMeters), exactly as
 * bake_trees.mjs does; it is recorded in the struct.json as voxelsPerMetre.
 *
 * A structure is worldgen input once a `structure` ref places it (P4), so a
 * re-bake of a PLACED structure moves the world hash: one
 * `--selftest --rebaseline`, a notification and not a regression.
 */
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const DIR = join(ROOT, 'assets', 'structures');
const HG = await import(pathToFileURL(join(ROOT, 'assets/editor/housegen.js')).href);
const VOX = await import(pathToFileURL(join(ROOT, 'assets/editor/vox.js')).href);
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;

function voxelsPerMetreFromWorldH() {
  const src = readFileSync(join(ROOT, 'src/sim/world.h'), 'utf8');
  const m = src.match(/kVoxelMeters\s*=\s*([0-9.]+)f/);
  if (!m) throw new Error('could not find kVoxelMeters in src/sim/world.h');
  const vpm = Math.round(1 / parseFloat(m[1]));
  if (!(vpm > 0)) throw new Error('bad kVoxelMeters');
  return vpm;
}

const args = process.argv.slice(2);
const flag = (f) => args.includes(f);
const opt = (f) => { const i = args.indexOf(f); return i >= 0 ? args[i + 1] : undefined; };
const statsOnly = flag('--stats');
const VPM = opt('--vpm') ? parseInt(opt('--vpm'), 10) : voxelsPerMetreFromWorldH();
const positional = args.filter((a, i) => !a.startsWith('--') && !['--seed', '--preset', '--vpm'].includes(args[i - 1]));

const fileOf = (name, ext) => join(DIR, ...name.split('/')) + ext;

function readStruct(name) {
  const p = fileOf(name, '.struct.json');
  return existsSync(p) ? JSON.parse(readFileSync(p, 'utf8')) : null;
}

function nextFree(name) {
  const base = name.replace(/_v\d+$/, '');
  for (let k = 2; k < 1000; k++) if (!existsSync(fileOf(base + '_v' + k, '.struct.json'))) return base + '_v' + k;
  return base + '_new';
}

function bake(name, params, seed) {
  if (!HG.validName(name)) { console.error('bad structure name "' + name + '" (lowercase words, optionally one folder)'); process.exit(2); }
  const cur = readStruct(name);
  if (cur && cur.handEdited === true && !statsOnly) {
    console.error(`REFUSED: structures/${name} is HAND-EDITED — its .vox is the truth and is never ` +
                  `overwritten by a regenerate (PLAN_world_editor §2.2). Bake to "${nextFree(name)}" instead.`);
    process.exit(1);
  }
  const t0 = Date.now();
  const res = HG.generateHouse(params, seed, {vpm: VPM});
  const files = HG.buildStructureFiles(res, name, MATS, VOX);
  const m = res.meta;
  console.log(name.padEnd(22) + ` ${res.dim.x}x${res.dim.y}x${res.dim.z}  ${String(m.voxels).padStart(7)} vox  ` +
              `${res.slots.length} slots  ${m.rooms} rooms ${m.doors} doors ${m.windows} win ${m.beds} beds ${m.chests} chests` +
              `${m.stair ? ' stair' : ''}${m.loft ? ' loft' : ''}  ${(files.vox.length / 1024).toFixed(0)} KiB  ${Date.now() - t0} ms` +
              (m.warnings.length ? '\n    warnings: ' + m.warnings.join(' | ') : ''));
  if (statsOnly) return;
  mkdirSync(dirname(fileOf(name, '.vox')), {recursive: true});
  writeFileSync(fileOf(name, '.vox'), Buffer.from(files.vox));
  writeFileSync(fileOf(name, '.struct.json'), files.json);
}

if (flag('--samples')) {
  for (const [name, preset] of Object.entries(HG.SAMPLES)) {
    const pr = HG.PRESETS[preset];
    bake(name, pr.params, pr.seed);
  }
} else {
  const name = positional[0];
  if (!name) { console.error('usage: node scripts/bake_structure.mjs <name> [params.json | --preset P] [--seed N] [--stats] | --samples'); process.exit(2); }
  let params, seed;
  if (opt('--preset')) {
    const pr = HG.PRESETS[opt('--preset')];
    if (!pr) { console.error('no preset "' + opt('--preset') + '" (have: ' + HG.PRESET_ORDER.join(', ') + ')'); process.exit(2); }
    params = pr.params; seed = pr.seed;
  } else if (positional[1]) {
    const j = JSON.parse(readFileSync(positional[1], 'utf8'));
    if (j && j.params) { params = j.params; seed = j.seed; } else params = j;
  } else {
    const cur = readStruct(name);
    if (!cur || !cur.generator || cur.generator.tool !== 'housegen') {
      console.error(`structures/${name}.struct.json has no housegen generator block to re-bake from; pass params.json or --preset`);
      process.exit(2);
    }
    params = cur.generator.params; seed = cur.generator.seed;
  }
  if (opt('--seed') !== undefined) seed = parseInt(opt('--seed'), 10);
  bake(name, params, seed | 0);
}
