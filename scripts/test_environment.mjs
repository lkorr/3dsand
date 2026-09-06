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
 *   4. THE LIVE MANIFEST (section 7). Every editable field of a biome file, a
 *      water preset and the map is listed in assets/editor/envlive.js as read
 *      or not-read-by-package, and every manifest key names a real field. A
 *      field missing from the manifest is the "third state" PLAN_environment_
 *      truth §1 forbids: shown as live, read by nothing.
 *
 *   node scripts/test_environment.mjs
 *
 * Node-only; the modules are pure, so nothing here needs a browser.
 */
import { readFileSync, existsSync, readdirSync, writeFileSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const WG = await load('assets/editor/watergen.js');
const BG = await load('assets/editor/biomegen.js');
const TG = await load('assets/editor/treegen.js');
const LV = await load('assets/editor/envlive.js');
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
// Since the world map's P1 the engine's biome id space is the biome FILES
// (ENGINE_BIOMES, 0..N-1) and the .svtree's baked weight words are not read;
// treegen.js BIOME_ORDER is only the four positional words the bake still
// writes, so it must be a PREFIX of the id space, not equal to it.
ok(JSON.stringify(BG.ENGINE_BIOMES.slice(0, TG.BIOME_ORDER.length)) === JSON.stringify(TG.BIOME_ORDER),
   'treegen.BIOME_ORDER is a prefix of biomegen.ENGINE_BIOMES (the biome id space)');
const biomes = biomeNames.map(n => BG.normalizeBiome(readJson(join(BIOMES, n + '.json'))));
const libs = {trees: new Set(speciesNames), water: new Set(presetNames), materials: MATNAMES};
for (const b of biomes) {
  const bad = BG.validateBiome(b, libs);
  ok(bad.length === 0, b.name + ': valid' + (bad.length ? ' — ' + bad.join('; ') : ''));
  const file = b.name;
  ok(b.index === BG.ENGINE_BIOMES.indexOf(file), b.name + ': index ' + b.index + ' matches worldgen id ' + BG.ENGINE_BIOMES.indexOf(file));
  // A biome with zero tree density (the ocean) legitimately lists nothing:
  // it IS water and grows no trees. Everything else must name at least one
  // species and one body-of-water preset, or the tab is showing an empty stack.
  if (b.trees.density > 0) {
    ok(b.trees.species.length > 0, b.name + ': lists ' + b.trees.species.length + ' tree species');
    ok(b.water.features.length > 0, b.name + ': lists ' + b.water.features.length + ' water bodies');
  } else {
    ok(true, b.name + ': tree density 0 -- ' + b.trees.species.length + ' species, ' + b.water.features.length + ' water bodies (nothing required)');
  }
}

/* ---- 4. the mirror: species placement.biomes vs biome files (informational) --- */
// Since the world map's P1 the engine builds the tree weight table from the
// biome files at load and never reads the .svtree's baked weight words, so
// `placement.biomes` in a species file is a DISPLAY mirror for the Trees page,
// not an authority. A stale mirror is reported, never failed; `node
// scripts/seed_environment.mjs --sync` refreshes it.
console.log('\n-- species weight mirror (informational) --');
for (const sp of speciesNames) {
  const j = readJson(join(TREES, sp + '.json'));
  const want = BG.speciesWeightsFrom(biomes, sp);
  const have = (j.placement && j.placement.biomes) || {};
  const same = BG.speciesWeightsMatch(biomes, sp, have);
  ok(true, sp + ': ' + (same ? 'mirror current' : 'mirror stale (' + JSON.stringify(have) +
     ' vs biomes ' + JSON.stringify(want) + ') -- node scripts/seed_environment.mjs --sync to refresh the Trees page'));
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

  // The scale ladder: every offered size fits MAX_SWATCH at an integer vpm,
  // the 1:1 sizes stay 1:1, and a big swatch composes at its coarser scale
  // with the scale recorded on the result (the page draws it at lod = 10/vpm).
  const ladder = BG.SWATCH_SIZES_M.map(m => m + 'm@' + BG.swatchScale(m, 10));
  ok(BG.SWATCH_SIZES_M.every(m => Math.round(m * BG.swatchScale(m, 10)) <= BG.MAX_SWATCH &&
                                  Number.isInteger(BG.swatchScale(m, 10))),
     'swatch scale ladder: ' + ladder.join(' '));
  ok(BG.swatchScale(24, 10) === 10 && BG.swatchScale(32, 10) === 10 && BG.swatchScale(96, 10) < 10,
     'up to 32 m is 1:1, 96 m bakes coarser (' + BG.swatchScale(96, 10) + ' vpm)');
  if (forest) {
    const v = BG.swatchScale(96, 10);
    const big = BG.generateSwatch(forest, lib, 7, {treeCache: cache, sizeM: 96, showcase: false, vpm: v});
    ok(big.vpm === v && big.dim.x === Math.round(96 * v) && big.dim.x <= BG.MAX_SWATCH,
       '96 m forest at ' + v + ' vpm -> ' + big.dim.x + 'x' + big.dim.y + 'x' + big.dim.z + ', ' + big.meta.treesPlaced + ' trees, ' + big.meta.waterBodies + ' bodies (true rarity)');
    // remapToMaterials is in place, by name, and keeps the state nibble.
    const before = big.cells.slice(), names = big.names.slice();
    const ids = MATS.materials.map(m => m.id);
    const missing = BG.remapToMaterials(big, ids);
    let okRemap = big.remapped === true && missing.length === 0;
    for (let i = 0; okRemap && i < before.length; i += 997) {
      const w = before[i];
      if (!w) { if (big.cells[i] !== 0) okRemap = false; continue; }
      const want = (ids.indexOf(names[(w & 0xFFF) - 1]) + 1) | (w & 0xF000);
      if (big.cells[i] !== want) okRemap = false;
    }
    ok(okRemap, 'remapToMaterials: in place, by name, state nibble kept, nothing missing');
  }
}

/* ---- 6. the world map ---------------------------------------------------------- */
// assets/worldmap/default is what the engine boots on; the World map page
// reads and writes the same two files. Pure checks: the planes parse, agree
// with map.json, name only biomes that have files, and the harness pad is a
// box. src/sim/worldmap.h is the authority for the format.
console.log('\n-- world map --');
{
  const MAP = join(ROOT, 'assets', 'worldmap', 'default');
  ok(existsSync(join(MAP, 'map.json')) && existsSync(join(MAP, 'map.svmap')), 'assets/worldmap/default has map.json + map.svmap');
  if (existsSync(join(MAP, 'map.json')) && existsSync(join(MAP, 'map.svmap'))) {
    const j = readJson(join(MAP, 'map.json'));
    const raw = readFileSync(join(MAP, 'map.svmap'));
    const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    const [w, h] = j.size;
    ok(dv.getUint32(0, true) === 0x504D5653 && dv.getUint32(4, true) === 1, 'map.svmap magic SVMP v1');
    ok(dv.getUint32(8, true) === w && dv.getUint32(12, true) === h, `map.svmap is ${w}x${h} like map.json`);
    ok(raw.length === 16 + w * h * 3, `map.svmap holds three ${w}x${h} planes`);
    const biomeFiles = new Set(readdirSync(join(ROOT, 'assets', 'biomes')).filter(f => f.endsWith('.json') && f[0] !== '_').map(f => f.slice(0, -5)));
    ok(j.biomes.every(n => biomeFiles.has(n)), 'every palette name has a biome file: ' + j.biomes.join(','));
    let maxIdx = 0;
    for (let i = 16; i < 16 + w * h; i++) maxIdx = Math.max(maxIdx, raw[i]);
    ok(maxIdx < j.biomes.length, `biome plane indices < palette size (${maxIdx} < ${j.biomes.length})`);
    ok(j.warpAmpVox >= 0 && j.warpAmpVox <= (1 << j.cellLog2) / 4, `warpAmpVox ${j.warpAmpVox} <= cell/4`);
    const pad = (j.sites || []).find(s => s.kind === 'pad');
    ok(!!pad && pad.min[0] <= pad.max[0] && pad.min[1] <= pad.max[1], 'a harness pad box exists and is a box');
    ok(!!pad && pad.min[0] <= 60 && pad.max[0] >= 420 && pad.min[1] <= 60 && pad.max[1] >= 420, 'the pad covers the fixture columns and the (420,420) tarn');
    // stamp sites and rules (P5): every template named must exist as a .vox,
    // because the engine refuses to start otherwise.
    const prefabs = new Set(existsSync(join(ROOT, 'assets', 'prefabs')) ? readdirSync(join(ROOT, 'assets', 'prefabs')).filter(f => f.endsWith('.vox')).map(f => f.slice(0, -4)) : []);
    const stamps = (j.sites || []).filter(s => s.kind === 'stamp');
    ok(stamps.every(s => typeof s.template === 'string' && Number.isInteger(s.x) && Number.isInteger(s.z)), `${stamps.length} stamp site(s) carry template/x/z`);
    ok(stamps.every(s => prefabs.has(s.template)), 'every stamp site names an existing assets/prefabs/<template>.vox');
    const rules = (j.rules || []);
    ok(rules.every(r => r.kind !== 'stamp' || (typeof r.template === 'string' && j.biomes.includes(r.biome) && r.perKm2 >= 0)), `${rules.length} rule(s) name a template, a palette biome and a perKm2`);
    ok(rules.every(r => r.kind !== 'stamp' || prefabs.has(r.template)), 'every rule names an existing assets/prefabs/<template>.vox');
  }
}

/* ---- 7. the LIVE manifest covers every editable field ---------------------------- */
// Walk the data models the pages edit and require an envlive.js entry for
// every leaf; then walk the manifest and require every key to be a leaf. Arrays
// of rows are walked through their default row (BG.defaultRows / the preset's
// first shore plant) under the "[]" spelling the pages use.
console.log('\n-- live manifest (envlive.js) --');
{
  const leaves = (obj, base, rows) => {
    const out = [];
    for (const k of Object.keys(obj)) {
      const v = obj[k], p = base ? base + '.' + k : k;
      if (Array.isArray(v)) {
        const tpl = rows && rows[p];
        if (tpl && typeof tpl === 'object' && !Array.isArray(tpl)) out.push(...leaves(tpl, p + '[]', rows));
        else out.push(tpl === null ? p : p + '[]');          // a leaf array (a profile, a palette)
      } else if (v && typeof v === 'object') {
        if (rows && rows[p] === null) out.push(p);           // an opaque object edited as one field
        else out.push(...leaves(v, p, rows));
      } else out.push(p);
    }
    return out;
  };
  const check = (scope, paths) => {
    const missing = paths.filter(p => !LV.lookup(scope, p));
    ok(missing.length === 0, scope + ': every editable field is in the LIVE manifest (' + paths.length + ' fields)' +
       (missing.length ? ' — MISSING: ' + missing.join(', ') : ''));
    const have = new Set(paths);
    const stray = Object.keys(LV.LIVE[scope]).filter(k => !have.has(k));
    ok(stray.length === 0, scope + ': every manifest key names a real field' + (stray.length ? ' — STRAY: ' + stray.join(', ') : ''));
    const entries = Object.values(LV.LIVE[scope]);
    const shaped = entries.every(e => e.read === true || e.preview === true ||
                                      (e.read === false && typeof e.package === 'string' && typeof e.why === 'string' && e.why.length > 10));
    ok(shaped, scope + ': every unread entry names its package and why');
    const unread = entries.filter(e => e.read === false).length;
    ok(true, scope + ': ' + (entries.length - unread) + ' live/preview, ' + unread + ' not read' +
       (unread ? ' (' + [...new Set(entries.filter(e => e.read === false).map(e => e.package))].sort().join(', ') + ')' : ''));
  };
  const R = BG.defaultRows();
  const biomeRows = {'cover.plants': R.coverPlant, 'trees.species': R.treeSpecies, 'water.features': R.waterFeature,
                     'caves.features': R.caveFeature, 'terrain.overrides': null};
  check('biome', leaves(BG.defaultBiome(), '', biomeRows));
  const wp = WG.defaultParams();
  check('water', leaves(wp, '', {'shore.plants': wp.shore.plants[0], 'bathymetry.profile': null}));
  const mapJson = readJson(join(ROOT, 'assets', 'worldmap', 'default', 'map.json'));
  const mapRows = {sites: {}, rules: {}, biomes: null, size: null, originCell: null};
  const mapPaths = leaves(mapJson, '', mapRows).filter(p => !/^(sites|rules)\[\]\./.test(p))
      .concat(['sites[]', 'rules[]', 'planes.biome', 'planes.landform', 'planes.moisture']);
  check('map', [...new Set(mapPaths)]);
  // P-D landed: the biome's tile IS read (one lattice, thinned per biome to
  // density × (T/tile)²), and so is every per-row condition on tree and cover
  // rows. The manifest must say so, or the page greys a live field.
  const tile = LV.lookup('biome', 'trees.tile');
  ok(!!tile && tile.read === true, 'trees.tile is marked read (P-D: one lattice, thinned per biome)');
  for (const k of ['minY', 'maxY', 'maxSlope', 'nearWaterMax', 'nearWaterMin', 'patchThreshold'])
    ok(LV.isLive('biome', 'trees.species[].conditions.' + k) && LV.isLive('biome', 'cover.plants[].conditions.' + k),
       'conditions.' + k + ' is live on tree and cover rows (P-D)');
  ok(LV.normalizePath('cover.plants[3].conditions.minY') === 'cover.plants[].conditions.minY', 'lookup elides array indices');
  ok(LV.isLive('biome', 'trees.density') && !LV.isLive('biome', 'climate.moisture'), 'isLive: density yes, a climate coordinate no');
  // P-F landed: the water rows roll (tile / rarity / preset + minY/maxY/maxSlope
  // at the pond centre) and the preset's geometry is carved; the shaped
  // footprint and the floor noise stay preview-only.
  for (const k of ['preset', 'tile', 'rarity', 'conditions.minY', 'conditions.maxSlope'])
    ok(LV.isLive('biome', 'water.features[].' + k), 'water.features[].' + k + ' is live (P-F)');
  for (const k of ['footprint.radius', 'bathymetry.depth', 'bathymetry.profile', 'berm.height', 'shore.band', 'bed.shallow', 'fill.material'])
    ok(LV.isLive('water', k) && LV.lookup('water', k).read === true, 'water preset ' + k + ' is read (P-F)');
  ok(!LV.isLive('water', 'footprint.lobes') && !LV.isLive('water', 'bathymetry.floorNoise'), 'the shaped footprint and floor noise stay preview-only');
  // Round trip: the P-E fields survive normalizeBiome so a save carries them.
  const nb = BG.normalizeBiome({caves: {features: [{preset: 'deep', threshold: 140, mushroomChance: 7, crystalChance: 9}]},
                                cover: {cactusChance: 40, saguaroFraction: 15, cacti: true}});
  ok(nb.caves.features[0].mushroomChance === 7 && nb.caves.features[0].crystalChance === 9 &&
     nb.cover.cactusChance === 40 && nb.cover.saguaroFraction === 15 && nb.cover.cacti === true &&
     nb.cover.groundFlora === true && nb.cover.sandCap === false,
     'normalizeBiome carries the flags and the P-E fields (mushroom 7, crystal 9, cactus 1-in-40, saguaro 15%)');
}

/* ---- 8. the predictions export: what the page prints, for the engine to hold to ---- */
// PLAN_environment_truth P-H. The `env-truth` selftest gate ports densityStats
// and the cover rows' "1 in N" to C++ and MEASURES the GPU worldgen against
// them; this file is how the port is held to the JS. It is COMMITTED
// (tests/env_predictions.json) so the gate runs without node having run, and
// it carries an FNV-1a over assets/biomes/*.json (the same (name, 0, bytes)
// sequence as biomes::HashFileSet) so both the gate and check_invariants.py
// can tell a stale export from a current one. Re-run this script after any
// biome edit; the gate says so when it has to.
console.log('\n-- predictions export (tests/env_predictions.json) --');
{
  const fnvFiles = (dir, ext) => {
    let h = 0x811c9dc5;
    const mix = buf => { for (let i = 0; i < buf.length; i++) { h ^= buf[i]; h = Math.imul(h, 0x01000193) >>> 0; } };
    const names = existsSync(dir) ? readdirSync(dir).filter(f => f.endsWith(ext)).sort() : [];
    for (const n of names) { mix(Buffer.from(n, 'utf8')); mix(Buffer.from([0])); mix(readFileSync(join(dir, n))); }
    return (h >>> 0).toString(16).padStart(8, '0');
  };
  const out = {
    _about: 'GENERATED by scripts/test_environment.mjs section 8 -- do not edit. The biome pages\' nominal numbers ' +
            '(biomegen.js densityStats and the cover rows\' 1-in-N), held to the C++ port in the env-truth gate ' +
            '(src/test/selftest_envtruth.cpp). biomesHash is FNV-1a over assets/biomes/*.json (biomes::HashFileSet); ' +
            'check_invariants.py fails when it no longer matches the files.',
    biomesHash: fnvFiles(BIOMES, '.json'),
    biomes: {},
  };
  for (const b of [...biomes].sort((a, c) => a.name < c.name ? -1 : 1)) {
    const d = BG.densityStats(b.trees.tile, b.trees.density);
    out.biomes[b.name] = {
      index: b.index,
      treeTileM: b.trees.tile,
      treeDensityPct: b.trees.density,
      treesPerHa: d.perHa,
      skin: b.cover.skin,
      subsoil: b.cover.subsoil,
      cover: b.cover.plants.map(r => ({material: r.material, head: r.head || '', chance: r.chance, pct: r.chance > 0 ? 100 / r.chance : 0})),
    };
  }
  const path = join(ROOT, 'tests', 'env_predictions.json');
  const text = JSON.stringify(out, null, 2) + '\n';
  const before = existsSync(path) ? readFileSync(path, 'utf8') : '';
  writeFileSync(path, text);
  ok(true, 'wrote tests/env_predictions.json (' + Object.keys(out.biomes).length + ' biomes, biomesHash ' + out.biomesHash +
     (before === text ? ', unchanged)' : before ? ', CHANGED -- commit it)' : ', new -- commit it)'));
}

console.log('\n' + (fails ? `${fails} of ${count} checks FAILED` : `all ${count} checks passed`));
process.exit(fails ? 1 : 0);
