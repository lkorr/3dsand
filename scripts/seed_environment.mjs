/* seed_environment.mjs — the biome / water-preset files, headlessly.
 *
 *   node scripts/seed_environment.mjs --seed    write the shipped presets to
 *                                               assets/biomes/ and assets/water/
 *                                               (only files that do not exist,
 *                                               unless --force)
 *   node scripts/seed_environment.mjs --sync    rewrite placement.biomes in every
 *                                               assets/trees/<species>.json from
 *                                               the biome files (what the
 *                                               Environment tab does on save)
 *   node scripts/seed_environment.mjs --check   report, change nothing, exit 1
 *                                               if any species is out of sync
 *                                               or any biome fails validation
 *
 * WHY --sync EXISTS. The biome file is where tree weights are EDITED, but the
 * tree atlas is baked one species at a time from that species' own file, so
 * the species file keeps a mirror of its weights (treegen.js BIOME_ORDER words
 * 12..15 of the .svtree header). One direction, one script, and `--check` is
 * the gate that says the mirror is current. After a --sync that changed
 * anything: `node scripts/bake_trees.mjs`, then `--selftest --rebaseline`.
 */
import { readFileSync, writeFileSync, existsSync, readdirSync, mkdirSync } from 'fs';
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
const TREES = join(ROOT, 'assets', 'trees');

const listJson = dir => existsSync(dir)
    ? readdirSync(dir).filter(f => f.endsWith('.json')).map(f => f.slice(0, -5)).sort() : [];
const readJson = p => JSON.parse(readFileSync(p, 'utf8'));
const writeJson = (p, o) => writeFileSync(p, JSON.stringify(o, null, 2) + '\n');

let changed = 0, problems = 0;

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

if (has('--sync') || has('--check')) {
  const mats = new Set(readJson(join(ROOT, 'assets/materials/materials.json')).materials.map(m => m.id));
  const trees = new Set(listJson(TREES));
  const water = new Set(listJson(WATER));
  const biomes = listJson(BIOMES).map(n => BG.normalizeBiome(readJson(join(BIOMES, n + '.json'))));
  if (!biomes.length) { console.log('no biome files in assets/biomes/ — run --seed first'); process.exit(1); }
  for (const b of biomes) {
    const bad = BG.validateBiome(b, { trees, water, materials: mats });
    for (const w of bad) { console.log('  BAD   biomes/' + b.name + ': ' + w); problems++; }
  }
  for (const n of listJson(WATER)) {
    const P = WG.normalizeParams(readJson(join(WATER, n + '.json')));
    const names = [P.fill.material, P.fill.surfaceMaterial, P.bed.shallow, P.bed.deep, P.bed.substrate,
                   P.ground.skin, P.ground.soil, P.ground.rock, P.shore.mudMaterial, P.shore.mossMaterial,
                   P.aquatic.emergent.material, P.aquatic.floating.material, P.aquatic.floating.flower,
                   P.aquatic.submerged.material, ...P.shore.plants.flatMap(pl => [pl.material, pl.head])];
    for (const nm of names)
      if (nm && nm !== 'none' && !mats.has(nm)) { console.log('  BAD   water/' + n + ': unknown material "' + nm + '"'); problems++; }
  }
  for (const sp of trees) {
    const p = join(TREES, sp + '.json');
    const j = readJson(p);
    const want = BG.speciesWeightsFrom(biomes, sp);
    if (BG.speciesWeightsMatch(biomes, sp, j.placement && j.placement.biomes)) continue;
    const have = (j.placement && j.placement.biomes) || {};
    console.log('  ' + (has('--sync') ? 'sync ' : 'STALE') + ' trees/' + sp + '.json  ' +
                JSON.stringify(have) + ' -> ' + JSON.stringify(want));
    if (has('--sync')) {
      j.placement = j.placement || {};
      j.placement.biomes = want;
      writeJson(p, j);
    }
    changed++;
  }
  // A biome naming a species is not enough: an ENGINE biome the species does
  // not list must read as zero, and that is what speciesWeightsFrom produces.
  // A non-engine biome (index -1) is authored but not bakeable; say so once.
  for (const b of biomes)
    if (!BG.ENGINE_BIOMES.includes(b.name))
      console.log('  note  biomes/' + b.name + ' is not an engine biome yet: its stacks are authored, not baked');
}

if (!args.length || has('--help')) {
  console.log('usage: node scripts/seed_environment.mjs --seed [--force] | --sync | --check');
  process.exit(0);
}
if (has('--check')) {
  console.log(problems || changed ? `check: ${problems} problem(s), ${changed} stale species file(s)`
                                  : 'check: biomes valid, every species file in sync');
  process.exit(problems || changed ? 1 : 0);
}
if (has('--sync') && changed)
  console.log('\nnext: node scripts/bake_trees.mjs && bash scripts/build.sh && ' +
              './build/Release/sandvox.exe --selftest --rebaseline');
