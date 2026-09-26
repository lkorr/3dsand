/* seed_environment.mjs — the biome / water-preset files, headlessly.
 *
 *   node scripts/seed_environment.mjs --seed    write the shipped presets to
 *                                               assets/biomes/ and assets/water/
 *                                               (only files that do not exist,
 *                                               unless --force)
 *
 * Validation is `node scripts/test_environment.mjs` (the data gate) and the
 * engine's `biomes` gate. The per-species weight mirror this script used to
 * --sync / --check is gone: the engine builds the tree weight table from the
 * biome files at load (treeatlas.cpp), so there is nothing to keep in step.
 */
import { writeFileSync, existsSync, mkdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const BG = await load('assets/editor/biomegen.js');
const WG = await load('assets/editor/watergen.js');

const args = process.argv.slice(2);
const has = f => args.includes(f);
const BIOMES = join(ROOT, 'assets', 'biomes');
const WATER = join(ROOT, 'assets', 'water');

const writeJson = (p, o) => writeFileSync(p, JSON.stringify(o, null, 2) + '\n');

let changed = 0;

if (has('--seed')) {
  mkdirSync(BIOMES, { recursive: true });
  mkdirSync(WATER, { recursive: true });
  for (const n of BG.BIOME_ORDER) {
    const p = join(BIOMES, n + '.json');
    if (existsSync(p) && !has('--force')) { console.log('  keep  biomes/' + n + '.json'); continue; }
    writeJson(p, BG.normalizeBiome(BG.BIOME_PRESETS[n]));
    console.log('  wrote biomes/' + n + '.json'); changed++;
  }
  for (const n of WG.PRESET_ORDER) {
    const p = join(WATER, n + '.json');
    if (existsSync(p) && !has('--force')) { console.log('  keep  water/' + n + '.json'); continue; }
    const P = WG.normalizeParams(WG.PRESETS[n]);
    delete P.vpm;
    writeJson(p, P);
    console.log('  wrote water/' + n + '.json'); changed++;
  }
}

if (!args.length || has('--help')) {
  console.log('usage: node scripts/seed_environment.mjs --seed [--force]');
  process.exit(0);
}
