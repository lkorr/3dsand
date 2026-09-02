/* test_environment.mjs — the biome / water-body data gate.
 *
 * WHAT IT GUARDS. Three pure modules (watergen.js, biomegen.js and the files
 * they read) and the one mirror between them:
 *
 *   1. DETERMINISM. Same preset + same seed -> byte-identical cells, for the
 *      water body and for the composed swatch. Without it a preview is a
 *      screenshot, not a tool, and the day the engine reads these files a
 *      re-bake would move the world hash for no nameable reason.
 *   2. PRESET SANITY. Every shipped water preset generates, holds water (or is
 *      declared dry), stays inside the format's caps, and names only materials
 *      that exist. Every biome file validates against the species, presets and
 *      materials it names, and carries the engine id its name implies.
 *   3. THE MIRROR. Each species file's placement.biomes equals what the biome
 *      files say. This is the one place the two authoring surfaces could
 *      disagree, and the engine reads the SPECIES copy — so a stale mirror is
 *      a biome that silently does not do what its page shows.
 *
 *   node scripts/test_environment.mjs
 *
 * Node-only; the modules are pure, so nothing here needs a browser.
 */
import { readFileSync, existsSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const WG = await load('assets/editor/watergen.js');
const BG = await load('assets/editor/biomegen.js');
const TG = await load('assets/editor/treegen.js');
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8'));
const MATNAMES = new Set(MATS.materials.map(m => m.id));

let fails = 0, count = 0;
const ok = (cond, what) => {
  count++;
  if (cond) { console.log('  ok   ' + what); return true; }
  console.log('  FAIL ' + what); fails++; return false;
};
function fnv(cells) {
  let h = 0x811c9dc5;
  for (let i = 0; i < cells.length; i++) {
    h ^= cells[i] & 0xFF; h = Math.imul(h, 0x01000193) >>> 0;
    h ^= (cells[i] >>> 8) & 0xFF; h = Math.imul(h, 0x01000193) >>> 0;
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}
const list = dir => existsSync(dir) ? readdirSync(dir).filter(f => f.endsWith('.json')).map(f => f.slice(0, -5)).sort() : [];
const readJson = p => JSON.parse(readFileSync(p, 'utf8'));
const WATER = join(ROOT, 'assets', 'water'), BIOMES = join(ROOT, 'assets', 'biomes'), TREES = join(ROOT, 'assets', 'trees');

/* ---- 1. watergen determinism ------------------------------------------------- */
console.log('\n-- watergen determinism --');
{
  const p = WG.PRESETS.marsh;
  const a = WG.generateWaterBody(p, 11), b = WG.generateWaterBody(p, 11);
  ok(fnv(a.cells) === fnv(b.cells) && a.cells.length === b.cells.length, 'marsh seed 11 twice -> ' + fnv(a.cells));
  const c = WG.generateWaterBody(p, 12);
  ok(fnv(a.cells) !== fnv(c.cells), 'a different seed is a different body');
  // Path-keyed randomness: nudging one knob must not reshuffle the shore plants
  // wholesale. Compare plant counts, which should move a little, not reroll.
  const q = JSON.parse(JSON.stringify(WG.normalizeParams(p)));
  q.bathymetry.depth += 0.1;
  const d = WG.generateWaterBody(q, 11);
  // The SHORE plants live on land, which a depth change does not touch; the
  // aquatic bands legitimately move with the floor, so they are not compared.
  const aquatic = [q.aquatic.emergent.material, q.aquatic.floating.material, q.aquatic.submerged.material];
  const shoreKinds = q.shore.plants.map(pl => pl.material).filter(k => a.meta.plants[k] && !aquatic.includes(k));
  const same = shoreKinds.filter(k => d.meta.plants[k] && Math.abs(d.meta.plants[k] - a.meta.plants[k]) / a.meta.plants[k] < 0.1).length;
  ok(same === shoreKinds.length, 'a depth nudge leaves the shore plant counts within 10% (' + same + '/' + shoreKinds.length + ' species)');
  // The profile evaluator is monotone through a monotone point set and pins the ends.
  const pts = WG.sanitizeProfile([[0, 1], [0.6, 0.98], [0.85, 0.7], [1, 0]]);
  let mono = true, prev = 2;
  for (let i = 0; i <= 50; i++) { const v = WG.profileAt(pts, i / 50); if (v > prev + 1e-9) mono = false; prev = v; }
  ok(mono && Math.abs(WG.profileAt(pts, 0) - 1) < 1e-9 && Math.abs(WG.profileAt(pts, 1)) < 1e-9, 'bathtub profile is monotone and pinned at both ends');
  ok(WG.sanitizeProfile([[0.5, 0.5]]).length === 3 && WG.sanitizeProfile(null).length === 2, 'sanitizeProfile repairs a one-point and an empty profile');
}

/* ---- 2. water presets: library and files ------------------------------------- */
console.log('\n-- water presets --');
const presetNames = list(WATER);
ok(presetNames.length >= WG.PRESET_ORDER.length, 'assets/water/ has ' + presetNames.length + ' presets (library has ' + WG.PRESET_ORDER.length + ')');
for (const n of WG.PRESET_ORDER) ok(presetNames.includes(n), 'library preset "' + n + '" has a file');
for (const n of presetNames) {
  const P = WG.normalizeParams(readJson(join(WATER, n + '.json')));
  const r = WG.generateWaterBody(P, 3);
  const m = r.meta;
  const wet = !!P.fill.material && P.fill.material !== 'none';
  const names = [P.fill.material, P.fill.surfaceMaterial, P.bed.shallow, P.bed.deep, P.bed.substrate, P.ground.skin, P.ground.soil,
                 P.ground.rock, P.shore.mudMaterial, P.shore.mossMaterial, P.aquatic.emergent.material, P.aquatic.floating.material,
                 P.aquatic.floating.flower, P.aquatic.submerged.material, ...P.shore.plants.flatMap(pl => [pl.material, pl.head])]
      .filter(x => x && x !== 'none');
  const unknown = names.filter(x => !MATNAMES.has(x));
  ok(unknown.length === 0, n + ': every material resolves' + (unknown.length ? ' (unknown: ' + unknown.join(', ') + ')' : ''));
  ok(!m.clipped, n + ': fits the cap (' + r.dim.x + 'x' + r.dim.y + 'x' + r.dim.z + ')');
  ok(m.voxels > 0 && m.voxels === r.cells.reduce((a, w) => a + (w ? 1 : 0), 0), n + ': ' + m.voxels.toLocaleString() + ' voxels, meta agrees with cells');
  if (wet) {
    ok(m.waterCells > 100 && m.volumeM3 > 0 && m.maxDepthM > 0, n + ': holds water (' + m.waterCells + ' cells, ' + m.volumeM3.toFixed(0) + ' m3, max ' + m.maxDepthM.toFixed(2) + ' m)');
    ok(Math.abs(m.maxDepthM - (P.bathymetry.depth + P.fill.level)) <= P.bathymetry.floorNoise + 0.15,
       n + ': max depth ' + m.maxDepthM.toFixed(2) + ' m tracks authored ' + (P.bathymetry.depth + P.fill.level).toFixed(2) + ' m');
    ok(m.volumeDevelopment > 0.8 && m.volumeDevelopment <= 3.001, n + ': volume development ' + m.volumeDevelopment.toFixed(2) + ' in [0.8, 3]');
    if (P.berm.height > 0) ok(m.bermCells > 0, n + ': berm present (' + m.bermCells + ' columns)');
    // Water never leaks past the berm: no water cell should be adjacent to air at the same Y outside the basin —
    // cheaper proxy: every water column has a bed cell directly under its lowest water cell.
    const {x: nx, y: ny, z: nz} = r.dim;
    let unsupported = 0;
    for (let z = 0; z < nz; z++) for (let x = 0; x < nx; x++) {
      for (let y = 1; y < ny; y++) {
        const w = r.cells[(z * ny + y) * nx + x];
        if (!w) continue;
        const isWater = ((w >>> 12) & 15) === WG.LIQ_FULL && r.names[(w & 0xFFF) - 1] === P.fill.material;
        if (isWater) { const below = r.cells[(z * ny + y - 1) * nx + x]; if (!below) unsupported++; break; }
      }
    }
    ok(unsupported === 0, n + ': every water column stands on a bed (' + unsupported + ' unsupported)');
  } else {
    ok(m.waterCells === 0 && !m.wet, n + ': declared dry and generated dry');
  }
}

/* ---- 3. biome files ------------------------------------------------------------- */
console.log('\n-- biomes --');
const biomeNames = list(BIOMES);
const speciesNames = list(TREES);
ok(biomeNames.length >= 4, 'assets/biomes/ has ' + biomeNames.length + ' biome files');
for (const n of BG.ENGINE_BIOMES) ok(biomeNames.includes(n), 'engine biome "' + n + '" has a file');
ok(JSON.stringify(BG.ENGINE_BIOMES) === JSON.stringify(TG.BIOME_ORDER), 'biomegen.ENGINE_BIOMES == treegen.BIOME_ORDER (the .svtree header order)');
const biomes = biomeNames.map(n => BG.normalizeBiome(readJson(join(BIOMES, n + '.json'))));
const libs = {trees: new Set(speciesNames), water: new Set(presetNames), materials: MATNAMES};
for (const b of biomes) {
  const bad = BG.validateBiome(b, libs);
  ok(bad.length === 0, b.name + ': valid' + (bad.length ? ' — ' + bad.join('; ') : ''));
  const file = b.name;
  ok(b.index === BG.ENGINE_BIOMES.indexOf(file), b.name + ': index ' + b.index + ' matches worldgen id ' + BG.ENGINE_BIOMES.indexOf(file));
  ok(b.trees.species.length > 0, b.name + ': lists ' + b.trees.species.length + ' tree species');
  ok(b.water.features.length > 0, b.name + ': lists ' + b.water.features.length + ' water bodies');
}

/* ---- 4. the mirror: species placement.biomes == biome files ------------------- */
console.log('\n-- species weight mirror --');
for (const sp of speciesNames) {
  const j = readJson(join(TREES, sp + '.json'));
  const want = BG.speciesWeightsFrom(biomes, sp);
  const have = (j.placement && j.placement.biomes) || {};
  ok(BG.speciesWeightsMatch(biomes, sp, have), sp + ': ' + JSON.stringify(have) +
     (BG.speciesWeightsMatch(biomes, sp, have) ? '' : ' != biomes say ' + JSON.stringify(want) + ' — run node scripts/seed_environment.mjs --sync'));
}

/* ---- 5. the swatch --------------------------------------------------------------- */
console.log('\n-- swatch --');
{
  const lib = {water: {}, trees: {}};
  for (const n of presetNames) lib.water[n] = readJson(join(WATER, n + '.json'));
  for (const n of speciesNames) lib.trees[n] = readJson(join(TREES, n + '.json'));
  const cache = new Map();
  const meadow = biomes.find(b => b.name === 'meadow') || biomes[0];
  const a = BG.generateSwatch(meadow, lib, 5, {treeCache: cache, sizeM: 16, showcase: true});
  const b = BG.generateSwatch(meadow, lib, 5, {treeCache: new Map(), sizeM: 16, showcase: true});
  ok(fnv(a.cells) === fnv(b.cells), meadow.name + ' swatch seed 5 twice -> ' + fnv(a.cells) + ' (fresh tree cache)');
  ok(a.meta.waterBodies >= 1, 'showcase composed ' + a.meta.waterBodies + ' water bod(ies): ' + JSON.stringify(a.meta.water));
  ok(Object.keys(a.meta.cover).length > 0, 'ground cover placed: ' + Object.keys(a.meta.cover).length + ' kinds');
  const forest = biomes.find(b => b.name === 'forest');
  if (forest) {
    const f = BG.generateSwatch(forest, lib, 7, {treeCache: cache, sizeM: 24, showcase: true});
    ok(f.meta.treesPlaced > 0, 'forest 24 m swatch placed ' + f.meta.treesPlaced + ' trees: ' + JSON.stringify(f.meta.trees));
    // Gating works: a species row demanding nearWaterMax 0.1 far from any water places nothing.
    const g = BG.normalizeBiome(JSON.parse(JSON.stringify(forest)));
    g.trees.species = [{species: 'oak', weight: 1, conditions: {nearWaterMax: 0.05}}];
    g.water.features = [];
    const h = BG.generateSwatch(g, lib, 7, {treeCache: cache, sizeM: 16, noWater: true});
    ok(h.meta.treesPlaced === 0 && h.meta.skipped.trees > 0, 'a nearWater condition with no water gates every tree out (' + h.meta.skipped.trees + ' skipped)');
  }
  const rs = BG.rarityStats(44.8, 4);
  ok(Math.abs(rs.pct - 25) < 1e-9 && Math.abs(rs.perKm2 - 124.6) < 0.1, 'rarity 1-in-4 of 44.8 m tiles = 25% = ' + rs.perKm2.toFixed(1) + ' per km2');
}

console.log('\n' + (fails ? `${fails} of ${count} checks FAILED` : `all ${count} checks passed`));
process.exit(fails ? 1 : 0);
