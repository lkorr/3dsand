/* envlive.js — the LIVE manifest: which Environment-tab fields the engine
 * actually reads, and which package will flip the rest.
 *
 * WHY THIS FILE EXISTS. docs/PLAN_environment_truth.md §0 item 3: the biome
 * page showed knobs the engine ignores indistinguishably from the ones it
 * reads, and a user halved their forest by editing a tile the engine never
 * looked at. §1: "every field on a page is live or visibly disabled — no third
 * state." This is the one table that says which; every row builder in
 * envui.js / biome.js / water.js consults it (envui.liveMark) and renders an
 * unread field disabled with the package and the reason in the tooltip.
 * scripts/test_environment.mjs walks the data models and fails if a field is
 * missing here, so a new field cannot ship in the third state — and fails if
 * a key here matches no field, so a typo cannot hide one.
 *
 * THE TRUTH IS DERIVED FROM THE ENGINE, NOT FROM MEMORY. "Read" means: packed
 * by worldmap::PackBiomeTable (src/sim/worldmap.cpp) or LoadTreeAtlas
 * (src/sim/treeatlas.cpp) AND consumed by assets/shaders/worldgen.wgsl
 * (grep wmBiome( / wmCover( / WM_B_* / WM_C_* / WM_BF_*). A field the loader
 * parses into BiomeDef but nothing packs (terrain.overrides, climate, every
 * water row, every water preset) is NOT read. Re-verify against those two
 * files when you flip an entry; do not flip it from the plan alone.
 *
 * Entry kinds:
 *   {read: true}                          the engine reads it (why: optional note)
 *   {read: false, package, why}           not read; `package` names the plan
 *                                         package that reads it ('P-D', 'P-E',
 *                                         'P-F', 'P-G', or 'later' when no
 *                                         package claims it yet)
 *   {preview: true, why}                  a preview-only knob that never claims
 *                                         to be about the world; rendered
 *                                         enabled with the note in the tooltip
 *
 * Keys are JSON paths with array indices elided: cover.plants[].chance,
 * trees.species[].conditions.minY. lookup() normalises "[3]" to "[]".
 */

'use strict';

const R = (why) => (why ? {read: true, why} : {read: true});
const N = (pkg, why) => ({read: false, package: pkg, why});
const P = (why) => ({preview: true, why});

// One conditions block, parameterised by what the engine does with it for
// that stack. Cover rows (P-D): all six are enforced in the worldgen.wgsl
// cover block — WM_C_MIN_Y / MAX_Y / MAX_SLOPE (Col.slope) / PATCH_THRESH,
// and nearWaterMax/Min through waterDistAt (WM_C_NEAR_WATER_*). Tree rows
// (P-D): all six are packed per (biome, species) into the atlas's condition
// table (TA_H_COND_TABLE, TA_C_*) and compared in treeInfoAt after the species
// pick, on top of the SPECIES FILE's own band/slope. Water and cave rows: not
// packed at all.
const ALL_CONDS = ['minY', 'maxY', 'maxSlope', 'nearWaterMax', 'nearWaterMin', 'patchThreshold'];
function conditions(base, readSet, pkg, why) {
  const out = {};
  for (const k of ALL_CONDS)
    out[base + '.conditions.' + k] = readSet.includes(k) ? R() : N(pkg, why);
  return out;
}

const COND_TREE = 'P-D: the row’s conditions are packed per (biome, species) into the tree atlas (TA_C_*) and compared in treeInfoAt; a gated-out pick grows nothing.';
const COND_COVER = 'P-D: enforced in the cover block (WM_C_MIN_Y/MAX_Y/MAX_SLOPE/PATCH_THRESH, nearWater via waterDistAt).';
const COND_WATER = 'water rows are not packed at all; P-F drives pond geometry from the presets.';
const COND_CAVE = 'cave rows are read for their threshold only; per-row conditions have no package yet.';

export const LIVE = {
  // ---- assets/biomes/<name>.json --------------------------------------------
  biome: Object.assign({
    'name': R('identity: the file name and the map palette entry'),
    'displayName': R('identity: label only'),
    'index': R('the biome id the map plane and the packed record are keyed by'),

    'climate.temperature': N('later', 'the painted map decides the biome; climate coordinates select nothing (PLAN_environment_truth §4 keeps climate noise a non-goal)'),
    'climate.moisture': N('later', 'the painted map decides the biome; climate coordinates select nothing (PLAN_environment_truth §4 keeps climate noise a non-goal)'),
    'climate.notes': R('free text, never read; kept enabled as notes'),

    'terrain.overrides': N('P-G', 'parsed into BiomeDef::terrainOverrides and read by nothing; P-G turns it into the fixed hill/grain/detail record the height twin reads'),

    'cover.skin': R('WM_B_SKIN'),
    'cover.skinDepth': R('WM_B_SKIN_DEPTH'),
    'cover.subsoil': R('WM_B_SUBSOIL'),
    'cover.patch.threshold': R('WM_B_PATCH_THRESH'),
    'cover.patch.cellLog2': R('WM_B_PATCH_LOG2'),
    'cover.groundFlora': R('WM_BF_GROUND_FLORA: the flower/undergrowth/tall-grass blocks'),
    'cover.cacti': R('WM_BF_CACTI: the cactus block runs here'),
    'cover.sandCap': R('WM_BF_SAND_CAP: a 4-deep sand cap under the skin'),
    'cover.cactusChance': N('P-E', 'today worldgen.cactusChance is global; P-E reads this per biome'),
    'cover.saguaroFraction': N('P-E', 'today worldgen.saguaroFraction is global; P-E reads this per biome'),
    'cover.plants[].material': R('WM_C_MAT'),
    'cover.plants[].head': R('WM_C_HEAD'),
    'cover.plants[].chance': R('WM_C_CHANCE (1 in N surface columns)'),
    'cover.plants[].height': R('WM_C_HEIGHT'),

    'trees.tile': R('P-D: worldgen scans ONE lattice (the finest tile among tree-growing biomes, TREE_TILE) and thins this biome on it to density × (T/tile)², WM_B_TREE_CHANCE_Q16 — trees/ha are densityStats(tile, density) by construction'),
    'trees.density': R('WM_B_TREE_CHANCE_Q16 (with the tile; WM_B_TREE_DENSITY keeps the authored percent)'),
    'trees.species[].species': R('the atlas biome table is built from these rows at load'),
    'trees.species[].weight': R('the atlas biome table is built from these rows at load'),

    'water.features[].preset': N('P-F', 'no water row is packed; worldgen grows one parabolic tarn per pond tile from worldgen.pond*'),
    'water.features[].tile': N('P-F', 'no water row is packed; worldgen grows one parabolic tarn per pond tile from worldgen.pond*'),
    'water.features[].rarity': N('P-F', 'no water row is packed; worldgen grows one parabolic tarn per pond tile from worldgen.pond*'),

    'caves.features[].preset': R('selects which of the two band thresholds this row sets'),
    'caves.features[].threshold': R('WM_B_CAVE_T1 / WM_B_CAVE_T2'),
    'caves.features[].rarity': N('later', 'only the threshold is packed; a per-region rarity has no package yet (RESEARCH_worldgen stage 8)'),
    'caves.features[].mushroomChance': N('P-E', 'today worldgen.caveMushroomChance is global; P-E reads this per biome'),
    'caves.features[].crystalChance': N('P-E', 'today worldgen.caveCrystalChance is global; P-E reads this per biome'),

    'swatch.sizeM': P('the swatch side; not a world value'),
    'swatch.reliefM': P('ground noise under the swatch; not the engine’s terrain'),
    'swatch.reliefFreq': P('ground noise under the swatch; not the engine’s terrain')
  },
  conditions('cover.plants[]', ALL_CONDS, 'P-D', COND_COVER),
  conditions('trees.species[]', ALL_CONDS, 'P-D', COND_TREE),
  conditions('water.features[]', [], 'P-F', COND_WATER),
  conditions('caves.features[]', [], 'later', COND_CAVE)),

  // ---- assets/water/<name>.json ---------------------------------------------
  // Nothing in a preset reaches worldgen today: src/sim/biomes.cpp parses the
  // files and ValidateBiomeSet checks them, and that is the whole consumer
  // list. Geometry goes live in P-F, the vegetation in P-E.
  water: {
    'name': R('identity: the file name a biome row names'),
    'displayName': R('identity: label only'),
    'kind': P('a label the biome page groups by; no engine meaning'),
    'preview.margin': P('plain ground past the fringe in the preview'),
    'ground.relief': P('preview relief; not part of what the engine will read'),
    'ground.reliefFreq': P('preview relief; not part of what the engine will read')
  },

  // ---- assets/worldmap/<name>/map.json + the three planes ---------------------
  map: {
    'name': R('identity'),
    'about': R('free text'),
    'cellLog2': R('WM_H_CELL_LOG2'),
    'size': R('WM_H_WIDTH / HEIGHT'),
    'originCell': R('WM_H_ORIGIN_X / Z'),
    'seaLevelY': R('WM_H_SEA_LEVEL_Y'),
    'oceanFadeCells': R('WM_H_OCEAN_FADE'),
    'warpAmpVox': R('the biome-edge warp'),
    'biomes': R('the palette: plane byte -> biome file'),
    'sites[]': R('the site table: pad boxes, stamps'),
    'rules[]': R('seeded per-biome stamp placement'),
    'planes.biome': R('mapBiomeAt'),
    'planes.landform': R('mapLandformQ8: owns the continental rung of the height'),
    'planes.moisture': N('later', 'packed (WM_H_MOISTURE_PLANE) and read by nothing; no package claims it yet')
  }
};

// The water preset's geometry + vegetation, generated: every leaf of
// watergen.defaultParams() that is not in the identity/preview list above.
{
  const geom = (why) => N('P-F', why);
  const veg = (why) => N('P-E', why);
  const G = 'no preset field reaches worldgen; P-F packs footprint / bathymetry / fill / berm / bed / placement into the pond table the height twin reads';
  const V = 'no preset field reaches worldgen; P-E replaces worldgen.shore* / reed* / lily* / kelp* with these rows';
  const add = (base, keys, mk, why) => { for (const k of keys) LIVE.water[base + '.' + k] = mk(why); };
  add('footprint', ['radius', 'radiusV', 'aspect', 'squareness', 'rotation', 'rotationRandom', 'warpAmp', 'warpFreq',
                    'warpOctaves', 'lobes', 'lobeRadius', 'lobeSpread', 'lobeWeld', 'islands', 'islandRadius', 'islandHeight'], geom, G);
  add('bathymetry', ['depth', 'rimDepth', 'profile', 'floorNoise', 'floorNoiseFreq'], geom, G);
  add('fill', ['material', 'level', 'surfaceMaterial'], geom, G);
  add('berm', ['height', 'width', 'coreFrac'], geom, G);
  add('bed', ['shallow', 'deep', 'shallowDepth', 'thickness', 'substrate'], geom, G);
  add('ground', ['skin', 'soil', 'soilDepth', 'rock'], geom, G);
  add('placement', ['tile', 'rarity', 'maxSlope', 'minY', 'maxY'], geom, G);
  add('shore', ['band', 'lift', 'mudWidth', 'mudMaterial', 'mossChance', 'mossMaterial'], veg, V);
  add('shore.plants[]', ['material', 'head', 'chance', 'reach', 'height'], veg, V);
  add('aquatic.emergent', ['material', 'chance', 'minDepth', 'maxDepth', 'height'], veg, V);
  add('aquatic.floating', ['material', 'flower', 'chance', 'flowerChance', 'minDepth', 'maxDepth'], veg, V);
  add('aquatic.submerged', ['material', 'chance', 'minDepth', 'height', 'clearance'], veg, V);
}

/** "cover.plants[3].chance" -> "cover.plants[].chance" */
export function normalizePath(path) {
  return String(path || '').replace(/\[\d+\]/g, '[]');
}

/** The manifest entry for a field, or undefined if the field is not listed
 *  (which test_environment.mjs treats as a failure). */
export function lookup(scope, path) {
  const t = LIVE[scope];
  return t ? t[normalizePath(path)] : undefined;
}

export function isLive(scope, path) {
  const e = lookup(scope, path);
  return !e || e.read === true || e.preview === true;
}

/** The tooltip line for an entry. */
export function describe(entry) {
  if (!entry) return 'NOT IN THE LIVE MANIFEST (assets/editor/envlive.js) — test_environment.mjs fails on this';
  if (entry.preview) return 'PREVIEW ONLY — ' + entry.why;
  if (entry.read) return entry.why ? 'live — ' + entry.why : 'live';
  return 'NOT READ BY THE ENGINE YET — ' + entry.package + ': ' + entry.why;
}

/** Short pill text. */
export function pill(entry) {
  if (!entry) return '?';
  if (entry.preview) return 'preview';
  if (entry.read) return '';
  return entry.package;
}
