/* biomegen.js — the biome DATA MODEL.
 *
 * WHAT A BIOME IS HERE. A named region type that owns FEATURE STACKS: which
 * ground cover it wears, which tree species grow in it and how densely, which
 * water bodies appear and how rarely, which cave presets run under it. Every
 * row of every stack carries the same PLACEMENT CHAIN — rarity, then
 * conditions (ground band, slope, distance to water, a patch mask) — which is
 * the model every data-driven worldgen has converged on (Minecraft's
 * placed-feature modifier list is the best-documented; docs/PLAN_biomes.md §2).
 *
 * WHO OWNS WHAT. This is the part that matters for "one authoritative source":
 *
 *   * a TREE SPECIES file (assets/trees/*.json) owns what a tree LOOKS like and
 *     what ground it can physically tolerate (altitude band, treeline, slope).
 *   * a WATER PRESET (assets/water/*.json) owns the shape of a body of water
 *     and what grows in and around it.
 *   * a BIOME file (assets/biomes/*.json) owns WHICH species and presets appear
 *     in it, at what weight/rarity, and under what extra conditions.
 *
 * The per-biome species WEIGHTS live only in the biome files' tree rows: the
 * engine builds the atlas's weight table from them at load (treeatlas.cpp),
 * so there is no copy in the species files and no bake between a weight edit
 * and the world.
 *
 * THE ENGINE READS THE BIOME FILES (src/sim/biomes.cpp loads and validates
 * them; the `biomes` gate). Worldgen reads the packed biome table for skin,
 * relief, cover, trees, caves and water; envlive.js is the per-field manifest
 * of what is read and what is not yet, and the tuner says so on the page.
 *
 * PURE MODULE: no DOM, no fetch, so `scripts/test_environment.mjs` runs all
 * of it under Node.
 *
 * THE SWATCH IS THE ENGINE'S (map-overhaul P7). This file used to compose the
 * biome page's swatch itself — its own ground noise, its own tree stamping,
 * its own cover — which was a picture the engine never makes. The page now
 * asks the voxel server for the real thing (tuner_server.py /api/biomeswatch
 * -> --voxserve SWATCH: a synthetic one-biome map through genChunk), and that
 * composer and its noise are gone.
 */

'use strict';

export const DEFAULT_VOX_PER_M = 10;
// The biome ID SPACE, in id order. Since the world map's P1 this is the list
// of assets/biomes/*.json files by `index` (0..N-1, contiguous), which the
// engine packs into the worldMap buffer in this order; check_invariants.py
// (`biome order`) asserts this list is a prefix of the files' id order. The
// biome page's Save assigns `index` from a name's position here, so a name
// missing from this list would be saved as -1 and the engine would refuse to
// start (ids must be contiguous). Add a biome HERE and as a file together.
export const ENGINE_BIOMES = ['forest', 'meadow', 'pine', 'desert',
                              'tundra', 'swamp', 'alpine', 'ocean'];

// =============================================================================
// the data model
// =============================================================================

/** The placement chain every feature row carries. -1 / 0 = unbounded. */
export function defaultConditions() {
  return {
    minY: -1,            // lowest ground Y (voxels, world) the row tolerates
    maxY: -1,            // highest; a per-row treeline
    maxSlope: 1024,      // Q8 landform slope gate (256 = 45 deg = repose)
    nearWaterMax: -1,    // metres; only within this distance of a water body
    nearWaterMin: 0,     // metres; keep at least this far from water
    patchThreshold: 0,   // 0..255; only where the row's patch noise is above this
    // P-G: the CANOPY band (cover rows only). worldgen's undergrowthSite
    // cover, 0 = open sky .. 255 = deep under overlapping crowns; 0 / 255 =
    // unbounded. What the engine's hard-coded undergrowth / flower chain
    // became: the shade plants author canopyMin 96, the gap flowers
    // canopyMax 95, the woodland-margin rose 40..95.
    canopyMin: 0,
    canopyMax: 255
  };
}

/** The identity relief curve: nine Q14 knots on a uniform grid, exactly the
 *  diagonal, so a biome that authors nothing keeps the map's relief bit for
 *  bit (worldgen.wgsl curveOne). */
export const CURVE_IDENTITY = [-16384, -12288, -8192, -4096, 0, 4096, 8192, 12288, 16384];

export function defaultBiome() {
  return {
    name: 'biome',
    displayName: 'Biome',
    index: -1,                       // worldgen B_* id; -1 = not an engine biome yet
    climate: {
      temperature: 0.5,              // 0 cold .. 1 hot   (future climate grid)
      moisture: 0.5,                 // 0 arid .. 1 wet
      notes: ''
    },
    terrain: {
      // P-G: the biome's RELIEF, read by the height mirror through the biome
      // record (worldmap.h kB_CurveKnot0..kB_GrainMul), blended over the four
      // map cells around a column so biomes never meet on a cliff.
      curve: CURVE_IDENTITY.slice(),   // 9 Q14 knots over the coarse relief's swing; below the diagonal = flatter
      hill: 256,                       // Q8 multipliers on the map's hill / detail / grain octaves (256 = the map's)
      detail: 256,
      grain: 256
    },
    cover: {
      skin: 'grass',                 // the topmost ground cell
      skinDepth: 1,                  // cells of skin (desert sand is 4)
      subsoil: 'dirt',
      // worldmap.h kB_FirmCover: the SOLID a POWDER cover (a sand cap, or a
      // powder skin like sand or snow) becomes where the ground is too steep
      // to hold loose matter. '' = fall back to the subsoil if that is solid,
      // else stone.
      firmSkin: '',
      patch: {threshold: 0, cellLog2: 5},   // shared patch mask for the plant rows
      // The three kBF_* flags PackBiomeTable packs (src/sim/worldmap.cpp);
      // defaults match the C++ reader's (biomes.cpp LoadBiomeSet).
      groundFlora: true,             // WM_BF_GROUND_FLORA: flowers / undergrowth / tall grass run here
      cacti: false,                  // WM_BF_CACTI: the cactus block runs here
      sandCap: false,                // WM_BF_SAND_CAP: a 4-deep sand cap under the skin
      // P-E reads these per biome (today worldgen.cactusChance / saguaroFraction
      // are global). 1-in-N and percent; 0 = never.
      cactusChance: 0,
      saguaroFraction: 0,
      plants: []                     // [{material, head, chance, height, conditions}]
    },
    trees: {
      tile: 14.4,                    // metres between candidate trunk sites
      density: 40,                   // percent of tiles that grow a tree
      species: []                    // [{species, weight, conditions}]
    },
    water: {
      features: []                   // [{preset, tile, rarity, conditions}]
    },
    caves: {
      features: []                   // [{preset, threshold, rarity, conditions}]  (scaffold)
    },
    // The page's preview size. Kept so existing files round-trip; the swatch
    // is the engine's since P7 and reads only sizeM (the relief keys fed the
    // deleted JS composer).
    swatch: {
      sizeM: 24                      // preview square, metres
    }
  };
}

function isObj(v) { return v && typeof v === 'object' && !Array.isArray(v); }
function merge(dst, src) {
  if (!isObj(src)) return dst;
  for (const k of Object.keys(src)) {
    const v = src[k];
    if (isObj(v) && isObj(dst[k])) merge(dst[k], v);
    else if (v !== undefined) dst[k] = Array.isArray(v) ? JSON.parse(JSON.stringify(v)) : v;
  }
  return dst;
}

export function normalizeConditions(c) {
  const out = merge(defaultConditions(), c || {});
  out.canopyMin = Math.min(255, Math.max(0, out.canopyMin | 0));
  out.canopyMax = Math.min(255, Math.max(0, out.canopyMax | 0));
  return out;
}

/** The biome's terrain block, coerced: nine integer knots in -16384..16384
 *  and three Q8 multipliers in 0..4096, exactly what the loader clamps to. */
export function normalizeTerrain(t) {
  const src = t && typeof t === 'object' ? t : {};
  const curve = Array.isArray(src.curve) && src.curve.length === 9
      ? src.curve.map(k => Math.min(16384, Math.max(-16384, Math.round(+k || 0))))
      : CURVE_IDENTITY.slice();
  const q8 = (v) => Math.min(4096, Math.max(0, Math.round(v === undefined ? 256 : +v || 0)));
  return {curve, hill: q8(src.hill), detail: q8(src.detail), grain: q8(src.grain)};
}
export function terrainIsIdentity(t) {
  const n = normalizeTerrain(t);
  return n.hill === 256 && n.detail === 256 && n.grain === 256 && n.curve.every((k, i) => k === CURVE_IDENTITY[i]);
}

/** One default row per stack — the shape normalizeBiome coerces every row to,
 *  what the biome page's "+ row" buttons push, and what test_environment.mjs
 *  walks to prove every row field is in the LIVE manifest (envlive.js). */
export function defaultRows() {
  return {
    coverPlant: {material: 'grass_tuft', head: '', chance: 12, height: 0.2, conditions: defaultConditions()},
    treeSpecies: {species: 'oak', weight: 10, conditions: defaultConditions()},
    waterFeature: {preset: 'tarn', tile: 44.8, rarity: 4, conditions: defaultConditions()},
    // mushroomChance / crystalChance: 1-in-N of the band's floor (ceiling for
    // crystal) columns, 0 = never. Global knobs today; P-E reads these.
    caveFeature: {preset: 'near_surface', threshold: 150, rarity: 1, mushroomChance: 0, crystalChance: 0,
                  conditions: defaultConditions()}
  };
}

export function normalizeBiome(src) {
  const b = merge(defaultBiome(), src || {});
  b.terrain = normalizeTerrain(src && src.terrain);
  b.cover.groundFlora = b.cover.groundFlora !== false;
  b.cover.cacti = !!b.cover.cacti;
  b.cover.sandCap = !!b.cover.sandCap;
  b.cover.firmSkin = String(b.cover.firmSkin || '');
  b.cover.cactusChance = Math.max(0, b.cover.cactusChance | 0);
  b.cover.saguaroFraction = Math.min(100, Math.max(0, b.cover.saguaroFraction | 0));
  b.cover.plants = (b.cover.plants || []).map(p => ({
    material: String(p.material || ''), head: String(p.head || ''),
    chance: Math.max(0, p.chance | 0), height: +p.height || 0.3,
    conditions: normalizeConditions(p.conditions)
  }));
  b.trees.species = (b.trees.species || []).map(s => ({
    species: String(s.species || ''), weight: Math.max(0, s.weight | 0),
    conditions: normalizeConditions(s.conditions)
  }));
  b.water.features = (b.water.features || []).map(f => ({
    preset: String(f.preset || ''), tile: +f.tile || 44.8, rarity: Math.max(0, f.rarity | 0),
    conditions: normalizeConditions(f.conditions)
  }));
  b.caves.features = (b.caves.features || []).map(f => ({
    preset: String(f.preset || 'near_surface'), threshold: f.threshold | 0,
    rarity: Math.max(0, f.rarity | 0),
    mushroomChance: Math.max(0, f.mushroomChance | 0), crystalChance: Math.max(0, f.crystalChance | 0),
    conditions: normalizeConditions(f.conditions)
  }));
  return b;
}

// =============================================================================
// rarity arithmetic — one form authored, the others shown
// =============================================================================

/** For a "1 in N tiles of T metres" row: percent and count per hectare. */
export function rarityStats(tileM, rarity) {
  if (!tileM || !rarity) return {pct: 0, perHa: 0, perKm2: 0};
  const pct = 100 / rarity;
  const tilesPerHa = 10000 / (tileM * tileM);
  return {pct, perHa: tilesPerHa / rarity, perKm2: tilesPerHa * 100 / rarity};
}
/** For a "P percent of tiles of T metres" row. */
export function densityStats(tileM, pct) {
  if (!tileM) return {perHa: 0, oneIn: 0};
  const tilesPerHa = 10000 / (tileM * tileM);
  return {perHa: tilesPerHa * pct / 100, oneIn: pct > 0 ? 100 / pct : 0};
}

// =============================================================================
// validation — the same checks the engine's `biomes` gate makes
// =============================================================================

/**
 * @param biome  normalised biome
 * @param libs   {trees:Set<string>, water:Set<string>, materials:Set<string>}
 * @returns string[] problems (empty = valid)
 */
export function validateBiome(biome, libs) {
  const bad = [];
  const mat = (nm, where) => {
    if (nm && libs.materials && !libs.materials.has(nm)) bad.push(where + ': unknown material "' + nm + '"');
  };
  if (!/^[a-z0-9_]+$/.test(biome.name)) bad.push('name must be [a-z0-9_]: "' + biome.name + '"');
  if (biome.index >= 0 && ENGINE_BIOMES[biome.index] !== biome.name)
    bad.push('index ' + biome.index + ' is worldgen\'s ' + (ENGINE_BIOMES[biome.index] || '(none)') +
             ', not ' + biome.name);
  mat(biome.cover.skin, 'cover.skin'); mat(biome.cover.subsoil, 'cover.subsoil');
  biome.cover.plants.forEach((p, i) => { mat(p.material, 'cover.plants[' + i + ']'); mat(p.head, 'cover.plants[' + i + '].head'); });
  biome.trees.species.forEach((s, i) => {
    if (libs.trees && !libs.trees.has(s.species)) bad.push('trees.species[' + i + ']: no species "' + s.species + '" in assets/trees/');
  });
  biome.water.features.forEach((f, i) => {
    if (libs.water && !libs.water.has(f.preset)) bad.push('water.features[' + i + ']: no preset "' + f.preset + '" in assets/water/');
    if (f.tile <= 0) bad.push('water.features[' + i + ']: tile must be > 0');
  });
  const seen = new Set();
  for (const s of biome.trees.species) {
    if (seen.has(s.species)) bad.push('trees.species: "' + s.species + '" listed twice');
    seen.add(s.species);
  }
  return bad;
}

// =============================================================================
// presets — the first four engine biomes, as seeded. `node
// scripts/seed_environment.mjs --seed` writes them to assets/biomes/ (never
// over an existing file).
// =============================================================================

export const BIOME_PRESETS = {
  forest: {
    name: 'forest', displayName: 'Forest', index: 0,
    climate: {temperature: 0.55, moisture: 0.65,
              notes: 'The default band of the biome noise: between meadowThreshold and pineThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 0, cellLog2: 5},
      plants: [
        {material: 'fern', chance: 16, height: 0.4},
        {material: 'flower_bluebell', chance: 40, height: 0.2},
        {material: 'mushroom_cluster', chance: 90, height: 0.2},
        {material: 'bramble', chance: 60, height: 0.4},
        {material: 'grass_tuft', chance: 9, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 78, species: [
      {species: 'oak', weight: 42}, {species: 'great_oak', weight: 12}, {species: 'birch', weight: 18},
      {species: 'pine', weight: 14}, {species: 'spruce', weight: 4}, {species: 'redwood', weight: 3},
      {species: 'willow', weight: 6, conditions: {nearWaterMax: 6}}, {species: 'eucalyptus', weight: 5},
      {species: 'bush', weight: 16}, {species: 'dead', weight: 4}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'spring_pool', tile: 25.6, rarity: 12, conditions: {maxSlope: 160}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  meadow: {
    name: 'meadow', displayName: 'Meadow', index: 1,
    climate: {temperature: 0.6, moisture: 0.5, notes: 'Biome noise below meadowThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 0, cellLog2: 5},
      plants: [
        {material: 'tall_grass', head: 'tall_grass_head', chance: 6, height: 0.5},
        {material: 'flower_poppy', chance: 30, height: 0.2},
        {material: 'flower_daisy', chance: 26, height: 0.2},
        {material: 'flower_buttercup', chance: 34, height: 0.2},
        {material: 'flower_clover', chance: 20, height: 0.1},
        {material: 'grass_tuft', chance: 7, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 22, species: [
      {species: 'oak', weight: 16}, {species: 'great_oak', weight: 5}, {species: 'birch', weight: 30},
      {species: 'pine', weight: 2}, {species: 'eucalyptus', weight: 12},
      {species: 'willow', weight: 14, conditions: {nearWaterMax: 8}}, {species: 'bush', weight: 40},
      {species: 'dead', weight: 3}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'marsh', tile: 76.8, rarity: 5, conditions: {maxSlope: 32}},
      {preset: 'kettle', tile: 38.4, rarity: 7, conditions: {maxSlope: 64}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  pine: {
    name: 'pine', displayName: 'Pine highland', index: 2,
    climate: {temperature: 0.3, moisture: 0.55, notes: 'Biome noise between pineThreshold and desertThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 120, cellLog2: 5},
      plants: [
        {material: 'heath_shrub', chance: 14, height: 0.4},
        {material: 'fern', chance: 30, height: 0.4},
        {material: 'moss_patch', chance: 40, height: 0.1},
        {material: 'toadstool_pale', chance: 120, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 70, species: [
      {species: 'pine', weight: 60}, {species: 'spruce', weight: 34}, {species: 'redwood', weight: 10},
      {species: 'birch', weight: 8}, {species: 'oak', weight: 4}, {species: 'bush', weight: 12},
      {species: 'dead', weight: 4}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'kettle', tile: 38.4, rarity: 6, conditions: {maxSlope: 64}},
      {preset: 'crater_lake', tile: 102.4, rarity: 14, conditions: {maxSlope: 128}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  desert: {
    name: 'desert', displayName: 'Desert', index: 3,
    climate: {temperature: 0.9, moisture: 0.1, notes: 'Biome noise above desertThreshold.'},
    cover: {
      skin: 'sand', skinDepth: 4, subsoil: 'sand', patch: {threshold: 128, cellLog2: 5},
      plants: [
        {material: 'dry_tussock', chance: 20, height: 0.3},
        {material: 'desert_scrub', chance: 36, height: 0.5},
        {material: 'cactus_flesh', head: 'cactus_bloom', chance: 90, height: 1.2}
      ]
    },
    trees: {tile: 14.4, density: 6, species: [
      {species: 'bush', weight: 26}, {species: 'dead', weight: 10}, {species: 'eucalyptus', weight: 8}
    ]},
    water: {features: [
      {preset: 'oasis', tile: 102.4, rarity: 6, conditions: {maxSlope: 48}},
      {preset: 'playa', tile: 128.0, rarity: 5, conditions: {maxSlope: 24}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  }
};

export const BIOME_ORDER = ['forest', 'meadow', 'pine', 'desert'];
