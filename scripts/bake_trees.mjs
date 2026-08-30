/* bake_trees.mjs — author-side bake: assets/trees/<species>.json -> .svtree
 *
 * The engine reads .svtree and nothing else. This script is what puts one
 * there, and it is the reason a fresh checkout has trees at all: the tuner's
 * "Bake" button calls the SAME `bakeAtlas` from the browser, but a build
 * machine has no browser, so the baked binaries are committed and this is what
 * regenerates them.
 *
 *   node scripts/bake_trees.mjs              # bake every species, report
 *   node scripts/bake_trees.mjs oak birch    # bake a subset
 *   node scripts/bake_trees.mjs --seed       # (re)write the shipped presets
 *                                            # as assets/trees/*.json first
 *   node scripts/bake_trees.mjs --stats      # generate + report, write nothing
 *
 * Editing a species and re-baking MOVES THE WORLD HASH — the atlas is engine
 * input exactly like tuning.json. That is a one-command rebaseline
 * (`--selftest --rebaseline`), and it is the deliberate trade for having a
 * single voxelizer instead of two implementations that must agree.
 */
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const DIR = join(ROOT, 'assets', 'trees');
const T = await import(pathToFileURL(join(ROOT, 'assets/editor/treegen.js')).href);

const args = process.argv.slice(2);
const doSeed = args.includes('--seed');
const statsOnly = args.includes('--stats');
const only = args.filter(a => !a.startsWith('--'));

mkdirSync(DIR, { recursive: true });

if (doSeed) {
  for (const name of T.PRESET_ORDER) {
    const p = join(DIR, name + '.json');
    writeFileSync(p, JSON.stringify(T.PRESETS[name], null, 2) + '\n');
    console.log('seeded ' + p.slice(ROOT.length + 1));
  }
}

const files = readdirSync(DIR).filter(f => f.endsWith('.json'))
    .map(f => f.slice(0, -5))
    .filter(n => !only.length || only.includes(n))
    .sort();

if (!files.length) {
  console.log('no species in assets/trees/ — run with --seed first');
  process.exit(1);
}

let totalBytes = 0, worst = 0;
for (const name of files) {
  const params = JSON.parse(readFileSync(join(DIR, name + '.json'), 'utf8'));
  const t0 = Date.now();
  const r = T.bakeAtlas(params);
  const ms = Date.now() - t0;
  worst = Math.max(worst, ms);
  totalBytes += r.meta.bytes;
  const v = r.meta.per;
  const leaf = Math.round(v.reduce((a, m) => a + m.leafCount, 0) / v.length);
  const wood = Math.round(v.reduce((a, m) => a + m.woodCount, 0) / v.length);
  const stems = Math.round(v.reduce((a, m) => a + m.stems, 0) / v.length);
  const clump = Math.round(v.reduce((a, m) => a + m.clumps, 0) / v.length);
  const drop = Math.round(v.reduce((a, m) => a + m.dropped, 0) / v.length);
  const flag = v.some(m => m.clipped) ? ' CLIPPED' : (v.some(m => m.truncated) ? ' TRUNCATED' : '');
  console.log(
    name.padEnd(12) +
    ` ${r.meta.variants}v  reach ${String(r.meta.reachXZ).padStart(3)}` +
    `  above ${String(r.meta.above).padStart(3)}` +
    `  crown y${String(r.meta.crownY).padStart(3)} r${String(r.meta.crownR).padStart(3)}` +
    `  stems ${String(stems).padStart(4)}  clumps ${String(clump).padStart(4)}` +
    `  wood ${String(wood).padStart(6)}  leaf ${String(leaf).padStart(6)}` +
    `  cut ${String(drop).padStart(5)}` +
    `  ${(r.meta.bytes / 1024).toFixed(0).padStart(5)} KiB  ${String(ms).padStart(5)} ms${flag}`);
  if (!statsOnly) {
    writeFileSync(join(DIR, name + '.svtree'), Buffer.from(r.buf));
  }
}
console.log(`\n${files.length} species, ${(totalBytes / 1048576).toFixed(2)} MiB` +
            ` of atlas, slowest bake ${worst} ms` +
            (statsOnly ? '  (--stats: nothing written)' : ''));
