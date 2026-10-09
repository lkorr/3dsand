/* map.js — the World map page of the Environment tab: THE ONE FRONT DOOR to
 * the world (PLAN_environment_truth P-I), on the v2 map format
 * (docs/PLAN_map_v2.md, "The v2 map format"; package E1). Paint the authored
 * HEIGHT in metres and WHERE the biomes are, declare the terrain's numbers,
 * place every site (the harness pad, the spawn, authored lakes, trees,
 * clearings, softened ground, stamps), sculpt the fine layer, pick the map
 * and edit layer the game loads, and preview the result as the engine's
 * heights or as real voxels.
 *
 * THE PLANES (maptiles.js owns the model): an absolute HEIGHT plane (i16
 * voxels, a post every 1 << planes.height.log2 voxels -- 6.4 m) and a BIOME
 * plane (u8 palette index, a cell every 1 << planes.biome.log2 -- 25.6 m),
 * stored as 256x256-sample tiles under assets/worldmap/<name>/planes/ that
 * exist only where they differ from the plane's default. The page lists the
 * tiles on load, fetches what is on screen first and the rest in the
 * background, edits tiles in memory, and a save POSTs map.json plus ONLY the
 * dirty tiles (a tile painted back to the default is deleted). Undo is per
 * stroke and stores only the tiles the stroke touched (before), as the sculpt
 * layer always has.
 *
 * THE DISPLAY is a topographic map computed from the authored height plane
 * itself, no engine call: a hypsometric colour ramp over the plane's REAL
 * min..max (sea from `seaLevelY` in blues), a hillshade from the NW, contour
 * lines (10 / 25 / 50 / 100 m, or auto by zoom; every 5th bold; the 0 m
 * contour is the coast) and the biome plane as a coloured overlay at a chosen
 * opacity. One cached image per (plane, tile, zoom level), from a per-tile
 * box-filtered mip pyramid, rebuilt only for tiles that changed -- so a
 * 3137-post plane pans and zooms at screen rate. "Real terrain" lays the
 * engine's own --heightmap of the view (noise + sculpt + sites, the SAVED
 * files) over it at an adjustable opacity; zoomed in it follows the view.
 *
 * THE TERRAIN IS THE MAP'S (P-G): map.json `terrain` carries the seeded noise
 * ladder that rides on top of the authored height, the attenuation, the
 * calm home area, the sediment wedge, the treeline, the reference scale. The
 * Terrain section edits it with the map's root numbers (sea level, edge
 * fade, biome warp).
 *
 * Sites are a list: at most one `kind: "pad"` box (a calm bare region; the
 * `harness` map carries the selftest's over its fixtures), one `kind:
 * "spawn"` column (where the game starts, the centre of the calm home area),
 * `kind: "stamp"` markers, `kind: "water"` AUTHORED LAKES (a water preset at
 * a fixed centre), `kind: "tree"` AUTHORED TREES, `kind: "clearing"` FOREST
 * CLEARINGS (a box no procedural crown reaches over, thinning across its
 * feather) and `kind: "soften"` SOFTENED GROUND (the biome's hills / bumps /
 * grain scaled to a percent inside a box). Every placed kind spells its
 * column `at: [x, z]` in world voxels. v2 has no landform sites and no
 * landform plane: the converter baked them into the height plane, and the
 * Shape tool carries them as stamps that write the plane.
 *
 * THE HEIGHT TOOLS (E2): the Height brush's modes (raise, lower, smooth,
 * flatten, set, terrace, noise, erode) and falloffs; the Shape tool (peak,
 * ridge, basin, plateau, volcano, mesa); the Line tool (RIDGE / VALLEY lines
 * kept in map.json `ridges[]`, baked into the plane, rebaked from this
 * session's pre-bake snapshot); the Area tool, whose rectangle the Height
 * tools panel erodes (erosion.js in a worker, seeded, preview then commit)
 * and exports / imports as 16-bit / RGB PNG (scripts/map_png.py through
 * /api/mappng/*). The maths is heightshapes.js + erosion.js (pure, Node-
 * tested by scripts/test_heighttools.mjs); every tool writes through
 * maptiles.js, so each dirties only its tiles and is one undo step.
 *
 * THE VECTOR LAYERS (E3, mapvector.js): ROADS (map.json `roads[]`, graded
 * ground the engine builds: polyline draw / vertex drag / insert / delete,
 * junction snapping, the inspector, and the ENGINE's baked profile from
 * --mapcheck), REGIONS (`regions[]`, named polygons, data only -- the
 * engine's RegionAt) and MARKERS (`markers[]`, pixel-art map annotations).
 * Locale markers are the locale editor's (E4), not these.
 *
 * LOCALES (E4, maplocale.js): every map.json `locales[]` entry drawn as its
 * bounds (low opacity) and a pixel-art marker; the Locale tool drags out a
 * new one; a double-click OPENS it -- the page becomes the LOCALE PAGE (a
 * breadcrumb back to the world): the view frames the bounds over the engine's
 * own heights of the box, the tools are cut to Sculpt / Clearing / Soften /
 * Tree / Water (all CLIPPED to the bounds: sculptDab skips a sample whose
 * bilinear reach leaves them, a box is clamped, a lake or tree outside is
 * refused) plus Structure (refs through the F8 command layer) and Link
 * (waynodes), and a 3D WorldView under the map edits the locale's OWN voxel
 * layer. One undo stack still: a refs batch is undone by its inverse commands.
 *
 * THE WORLDBUILDER (docs/PLAN_worldbuilder.md K1): every tool is one
 * registerTool({...}) in the registry (maptools.js) -- the built-ins below,
 * and the layers': RANGE (maprange.js: a mountain range from a spline of crest
 * height + half-width knots, live preview, bake = terraingen.rangeBake in the
 * terrain worker, map.json ranges[]), the GENERATE panel (mapgen.js:
 * Azgaar-style templates over the map or the Area, landforms by the Area),
 * MEASURE and the overlays (mapoverlay.js: slope bands, flow, lakes, km grid,
 * minimap), RIVER + LAKE (mapwater.js, E2: rivers[] / lakes[] drawn with the
 * engine's own bake ported to JS in riverbake.js, derive, meander, basin
 * flood). Whole-window work runs as terrainjobs.js jobs in
 * terrain_worker.js (one main job with the progress bar under the toolbar,
 * quiet preview jobs beside it); a generator's result waits in the plane as a
 * PENDING preview (Commit = one undo step, Discard). The Height brush takes a
 * MASK (land / sea / height band / slope band) and the Erode panel has the
 * stream-power passes (landscape / incise) beside the droplet one.
 *
 * Input: left-drag paints / places with the active tool, right- or
 * middle-drag pans, wheel zooms about the cursor, [ ] resize the brush,
 * Alt+click with the height or biome brush picks the value under the cursor,
 * Ctrl+Z / Ctrl+Y undo / redo. Every save moves the world hash (the map is
 * an input beside the seed); a running game picks a save up on F7 / Apply.
 */
import * as MT from './maptiles.js';
import * as HS from './heightshapes.js';
import * as VEC from './mapvector.js';
import * as LOC from './maplocale.js';
import {buildLegend, buildToolGuide} from './maplegend.js';
import {createToolRegistry} from './maptools.js';
import * as TG from './terraingen.js';
import {createRangeLayer} from './maprange.js';
import {createGenLayer} from './mapgen.js';
import {createOverlayLayer} from './mapoverlay.js';
import {createClimateLayer} from './mapclimate.js';
import {createCoastLayer} from './mapcoast.js';
import {BIOME_COLORS} from './climate.js';
import {createWaterLayer} from './mapwater.js';

// Display colours by biome NAME (the table lives in climate.js, pure, so the
// Node previews draw the same; worldbuilder B1's fourteen are in it); anything
// unknown gets a stable hash colour.
const COLOR = BIOME_COLORS;
function colorOf(name) {
  if (COLOR[name]) return COLOR[name];
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return `hsl(${h % 360} 45% 55%)`;
}

let H = null;
let els = {};
let map = null;            // {name, json, H: height plane, B: biome plane} (maptiles.js planes)
let dirty = false;
let tool = 'height';       // the active tool: a registry key (REG.keys(): site, height, shape, line, range, area, biome, sculpt, road, ...)
let VL = null;             // the vector layers (mapvector.js): roads, regions, markers
let LC = null;             // the locales (maplocale.js): markers + bounds, and the locale page
let RL = null;             // the mountain ranges (maprange.js): ranges[] splines, preview, bake
let GL = null;             // the generators (mapgen.js): templates + landforms over the map / the Area
let OL = null;             // the overlays (mapoverlay.js): slope / flow / lakes / grid / minimap + the Measure tool
let CLM = null;            // the climate panel (mapclimate.js): climate inputs, overlays, Generate biomes, the Lock tool
let CST = null;            // the coast tools (mapcoast.js): wobble / beach / cliff / smooth
let WL = null;             // the rivers + lakes (mapwater.js): rivers[] / lakes[], the River + Lake tools, the live bake
let previewPending = false; // the preview pane opens in its voxel view on the page's first activate()
// THE TOOL REGISTRY (maptools.js): every tool, built-in or a layer's, is one
// registerTool({...}) call; `locale: true` puts it on the LOCALE page (a
// locale open), `world: false` keeps it off the world map.
const REG = createToolRegistry();
export const registerTool = t => REG.registerTool(t);
let savedJson = null;      // map.json as last loaded / saved: the profile chart says when a road differs from its bake
// P-G: the terrain block map.json carries. Its DEFAULTS (what a map missing a
// key gets) have one copy, src/sim/worldmap.h TerrainParams, which the tuner
// server reads and serves (/api/worldmap/terrain-defaults, via
// scripts/map_terrain.py). null until fetched: a map whose terrain block is
// complete does not need them.
let TERRAIN_DEFAULTS = null;
async function loadTerrainDefaults() {
  const r = await fetch('/api/worldmap/terrain-defaults', {cache: 'no-store'});
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || 'HTTP ' + r.status);
  TERRAIN_DEFAULTS = j.terrain;
}
// [path, label, min, max, step, unit, description]; the path is into terrain,
// or into map.json's root when it starts with '@'.
const TERRAIN_ROWS = [
  ['@seaLevelY', 'sea level', -32000, 32000, 4, 'y vox', 'The sea surface (absolute Y). Ground below it is under water; the map draws it as the 0 m contour and every height readout is also given above it.'],
  ['@edgeFadeVox', 'edge fade', 0, 65536, 256, 'vox', 'Outside the extent the authored height eases from the edge posts to the height plane’s default over this distance.'],
  ['@warpAmpVox', 'biome warp', 0, 4096, 4, 'vox', 'The seeded wobble of biome boundaries; at most a quarter of a biome cell.'],
  ['baseHeight', 'noise datum', 0, 1024, 4, 'vox', 'The level the seeded range octave is centred on, for whatever still reads it; the authored height plane is absolute and is never scaled.'],
  ['rangeAmplitude', 'range height', 0, 1024, 8, 'vox', 'Full swing of the seeded range octave: ridges and valleys on top of the authored height. The biome curve reshapes this noise only; it never overrides the plane.'],
  ['rangeLog2', 'range size (log2)', 3, 15, 1, 'log2', 'Range noise cell as a power of two: 9 = 512 voxels = 51.2 m.'],
  ['hillAmplitude', 'hill height', 0, 512, 4, 'vox', 'Full swing of the broad hill octave (each biome scales it by its terrain.hill).'],
  ['hillLog2', 'hill size (log2)', 3, 15, 1, 'log2', 'Broad-hill noise cell as a power of two: 7 = 128 voxels = 12.8 m.'],
  ['detailAmplitude', 'detail height', 0, 128, 1, 'vox', 'Full swing of the detail octave (scaled per biome by terrain.detail).'],
  ['detailLog2', 'detail size (log2)', 3, 15, 1, 'log2', 'Detail noise cell: 5 = 32 voxels = 3.2 m.'],
  ['grainAmplitude', 'grain height', 0, 32, 1, 'vox', 'Full swing of the finest octave (scaled per biome by terrain.grain).'],
  ['grainLog2', 'grain size (log2)', 3, 15, 1, 'log2', 'Grain noise cell: 3 = 8 voxels = 0.8 m.'],
  ['fbmAtten', 'slope attenuation (Q8)', 0, 256, 8, 'Q8', 'iq’s derivative attenuation: each octave is divided by 1 + atten·|slope|², the slope including the authored height’s. 0 is plain fBm; 256 is the textbook form. A rule-2 mechanism, not a look knob.'],
  ['homeArea.radius', 'home area radius', 0, 4096, 16, 'vox', 'Chebyshev radius around the spawn site held calm.'],
  ['homeArea.fade', 'home area fade', 1, 8192, 64, 'vox', 'How far past the radius the noise ramps back to full. LOAD-BEARING: a short fade builds a cliff at exactly the boundary.'],
  ['sedAbove', 'sediment ceiling (relative)', -1024, 1024, 4, 'vox', 'The sediment wedge: where the noise sits below the AUTHORED ground (height plane + sculpt) + this, the ground carries loose dirt over gravel, thickness (this - noise) × fraction/256 - strip, slope-gated, clamped to max. Relative, so a high valley fills like a low one.'],
  ['sedFraction', 'sediment fraction (Q8)', 0, 256, 4, 'Q8', ''],
  ['sedStrip', 'sediment strip', 0, 64, 1, 'vox', ''],
  ['sedSlope', 'sediment slope gate (Q8)', 0, 512, 8, 'Q8', 'Dirt and gravel are POWDERS: the wedge thins to nothing as the slope approaches this, or the CA never sleeps. 0 = no wedge.'],
  ['sedMax', 'sediment max', 0, 40, 1, 'vox', 'Clamped under the cave shell (36 vox) at load.'],
  ['sedTopsoil', 'topsoil depth', 0, 40, 1, 'vox', 'The top of the wedge that wears the biome’s subsoil material.'],
  ['treeline', 'treeline', -32000, 32000, 4, 'y vox', 'The map’s treeline (absolute ground Y): snow and no trees at or above it. A column’s own is sea + (this − sea) × its biome’s treelineScale, ± the jitter.'],
  ['treelineJitter', 'treeline jitter', 0, 4096, 4, 'vox', 'The treeline wobbles ± this much (25.6 m noise) so it is not a ruled contour.'],
  ['rockSlope', 'steep rock slope (Q8)', 0, 4096, 16, 'Q8', 'A column at least this steep (landform slope; 256 = 1:1) skins as its biome’s rock with no snow and no plants: powder on a cliff never sleeps. 0 = off.'],
];
let selected = null;       // the site the sites panel has selected (any kind)
let heights = null;        // the decoded --heightmap backdrop, or null
let showHeights = false;
let savedGen = 0;          // bumps on every map save: heights.gen < savedGen = stale
let mapLayerWanted = '';   // world.mapLayer as the selector has it
let tuningTouched = false;
const SPAWN_DEFAULT = [140, 140];   // worldmap.cpp's default when a map names no spawn
// The water tool (P-F): an AUTHORED LAKE is `{kind: "water", preset, at: [x, z],
// radius?}`; `presets` is assets/water/ by name; `radiusM` 0 = the preset's own
// radius; `geom` caches each preset's radius/band in voxels for drawing.
let water = {preset: '', radiusM: 0, presets: [], geom: {}};
// The stamp tool: a template picked from what EXISTS under assets/prefabs/.
let stamp = {template: '', rot: 0, templates: []};
// The tree tool (P6): a species from what EXISTS under assets/trees/*.svtree.
let trees = {species: '', variant: -1, rot: -1, list: []};
// The map's LOAD WARNINGS (P6), as `sandvox --mapcheck` reports them for the
// SAVED files: {ok, warnings[], error} or null until fetched.
let check = null;
let waterDrag = null;      // {site, dx, dz}: dragging any site's `at`
// THE PLANE BRUSHES (height and biome), in METRES: one radius for both, a log
// slider from 1 m to 5 km; dabs every `spacing` % of the radius along a drag,
// so a stroke's weight does not depend on mouse speed. Height `strength` is a
// percent: raise / lower move the centre by strength% of radius/10 per dab (a
// 1 km brush at 50% = 50 m a dab); set / smooth go strength% of the way.
let brush = {radiusM: 200, spacing: 25, strength: 30, mode: 'raise', targetM: 0, index: 0,
             // the MASK every height-brush mode honours (maptiles.maskWeights): lo / hi / soft
             // are metres above the sea for a height band, degrees for a slope band
             mask: {kind: 'none', lo: 0, hi: 600, soft: 10}};
const R_MIN_M = 1, R_MAX_M = 5000;
// THE DISPLAY: topographic layer on/off, hillshade, contours (interval in
// metres, 0 = auto by zoom), the biome overlay's opacity (1 when the topo
// layer is off), the real-terrain backdrop's opacity.
let disp = {topo: true, shade: true, contours: true, interval: 0, biomeAlpha: 0.35, realAlpha: 0.6};
let view = {cx: 0, cz: 0, scale: 0.01};   // world-voxel centre + screen pixels per voxel
let undo = [];
let redo = [];
let stroke = null;         // the stroke in progress, pushed at pointerup if anything changed
let padDrag = null;
let clearing = {feather: 64};
let soften = {feather: 96, hills: 25, bumps: 0, grain: 0, paths: 10};
let clearDrag = null;
let lastDab = null;        // the last plane-brush dab's world column (spacing)
let stats = {tileImages: 0, contourPx: 0, skipped: 0};   // test + status counters
/* ---- THE HEIGHT TOOLS (PLAN_map_v2 E2) --------------------------------------------------
 * The Height brush's modes beyond raise / lower / set / smooth (flatten to the
 * height where the stroke starts, terrace / cliff, seeded noise, erode), its
 * falloff, and three tools that write the height plane in one go:
 *   Shape  the v1 landform kinds as stamps (peak, ridge, basin, plateau,
 *          volcano, mesa) -- height, radius, aspect, rotation; one click, one
 *          undo step.
 *   Line   RIDGE / VALLEY lines: a polyline with a crest / floor height per
 *          vertex, a half-width and a cross-section (V, U, round, sharp).
 *          The line lives in map.json `ridges[]` (the engine never reads it:
 *          it reads the baked plane) and BAKE writes it into the plane.
 *   Area   a rectangle the Height tools panel erodes (hydraulic + thermal, in
 *          a worker, seeded, preview then commit) and exports / imports as a
 *          16-bit PNG (the height) or an RGB PNG (the biomes).
 * Every one of them writes through maptiles.js's writeWindow, so it dirties
 * only the tiles it touches and undoes per operation like a stroke. */
let hb = {falloff: 'smooth', stepM: 25, sharp: 70, ampM: 60, sizeM: 800, seed: 1, style: 'ridged'};
let shape = {kind: 'peak', heightM: 400, mode: 'rise', radiusM: 1500, aspect: 1, rot: 0};
// The line tool: the selected line (an object inside map.json.ridges), whether
// clicks append to it, the defaults a NEW line / vertex takes, a vertex drag.
let lines = {sel: null, drawing: false, kind: 'ridge', profile: 'round', widthM: 600, yMode: 'sea', yM: 800, dyM: 150, drag: null};
// Bake records of this session, by line id: {before, after} tiles of the
// last bake (rebake = put `before` back, keep later edits as offsets, bake again).
let bakes = new Map();
let area = null;           // the Area rectangle, world voxels [x0, z0, x1, z1], or null
let areaDrag = null;
// method: 'land' = the landscape-evolution stream-power pass (valleys grown into
// the land, its heights kept), 'incise' = stream power cutting the ground as it
// is, 'droplet' = particle water + thermal scree (an Area only)
let erosion = {method: 'land', iterations: 6, strength: 50, talusDeg: 38, seed: 1, hydraulic: true, thermal: true,
               running: null, pending: null, progress: 0};
let pngIn = null;          // the decoded PNG waiting in the import dialog
/** The box site (clearing / soften) whose BOX holds world column (wx, wz), the last drawn first. */
function clearingAt(wx, wz, kind = 'clearing') {
  const ss = (map.json.sites || []).filter(s => s.kind === kind && Array.isArray(s.min) && Array.isArray(s.max));
  for (let i = ss.length - 1; i >= 0; i--) {
    const s = ss[i];
    if (wx >= s.min[0] && wx <= s.max[0] && wz >= s.min[1] && wz <= s.max[1]) return s;
  }
  return null;
}
/** A box site's centre (pad / clearing), world voxels. */
function boxCentre(s) { return [Math.round((s.min[0] + s.max[0]) / 2), Math.round((s.min[1] + s.max[1]) / 2)]; }

/* ---- THE SCULPT LAYER (P5): the author's own fine height offset --------------------
 * assets/worldmap/<name>/sculpt.svsculpt; src/sim/worldmap.h is the format's
 * authority. A signed offset in voxels, one sample every 8 voxels, bilinear
 * between them, stored in 256-voxel tiles (32 x 32 samples) that exist only
 * where someone sculpted. The engine adds it to the ground AFTER the authored
 * height and the seeded noise, BEFORE the sediment wedge, ponds, sea and site
 * pads, so it is not scaled by noise and everything else reacts to it. It is
 * the LOCALE-scale height tool; the height plane is the world-scale one.
 *
 * Here a tile is a Float32Array(1024) while editing and is rounded to i16 on
 * save; a tile that rounds to all zeros is not written, and a layer with no
 * tiles deletes the file. */
const SC_SP = 8, SC_T = 32, SC_TILE = SC_SP * SC_T;
const SC_MAGIC = 0x43535653, SC_VERSION = 1;   // 'SVSC'
const SC_MODES = ['raise', 'lower', 'smooth', 'flatten', 'erase'];
let sculpt = {
  tiles: new Map(),    // key -> Float32Array(1024): the layer being edited
  saved: new Map(),    // key -> Float32Array(1024): the layer on disk
  dirty: false,
  version: 0,
  mode: 'raise', radius: 96, strength: 30,
  target: null,        // flatten: the ground height sampled at the stroke's start
  last: null,          // the last dab's world column (dab spacing)
  show: true,
  dirtyRect: null,     // world-voxel rect the live heights preview must reshade
  tver: new Map(),     // key -> edit count: the overlay rebuilds a tile's image on change
  over: new Map(),     // key -> {ver, canvas}: the overlay's tile images
};
const scKey = (tx, tz) => (tx + 32768) * 65536 + (tz + 32768);
function scSample(tiles, sx, sz) {
  const t = tiles.get(scKey(sx >> 5, sz >> 5));
  return t ? t[(sz & 31) * SC_T + (sx & 31)] : 0;
}
/** The layer at a world column: the engine's bilinear (in floats). */
function scAt(tiles, x, z) {
  if (!tiles.size) return 0;
  const sx = Math.floor(x / SC_SP), sz = Math.floor(z / SC_SP);
  const fx = x / SC_SP - sx, fz = z / SC_SP - sz;
  const s00 = scSample(tiles, sx, sz), s10 = scSample(tiles, sx + 1, sz);
  const s01 = scSample(tiles, sx, sz + 1), s11 = scSample(tiles, sx + 1, sz + 1);
  const a = s00 + (s10 - s00) * fx, b = s01 + (s11 - s01) * fx;
  return a + (b - a) * fz;
}
export function parseSculpt(buf) {
  const tiles = new Map();
  if (!buf || !buf.byteLength) return tiles;
  const d = new DataView(buf);
  if (d.getUint32(0, true) !== SC_MAGIC || d.getUint32(4, true) !== SC_VERSION) throw new Error('not an SVSC v1 sculpt layer');
  if (d.getUint32(8, true) !== 3 || d.getUint32(12, true) !== 5) throw new Error('sculpt layer geometry is not 8-voxel samples in 32-sample tiles');
  const n = d.getUint32(16, true);
  if (buf.byteLength !== 32 + n * 2056) throw new Error('sculpt layer size does not match its tile count');
  for (let i = 0; i < n; i++) {
    const o = 32 + i * 2056;
    const tx = d.getInt32(o, true), tz = d.getInt32(o + 4, true);
    const t = new Float32Array(SC_T * SC_T);
    for (let k = 0; k < SC_T * SC_T; k++) t[k] = d.getInt16(o + 8 + k * 2, true);
    tiles.set(scKey(tx, tz), t);
  }
  return tiles;
}
/** The file's bytes, or null for an empty layer (the caller deletes the file). */
export function serializeSculpt(tiles) {
  const keep = [];
  for (const [k, t] of tiles) {
    const q = new Int16Array(SC_T * SC_T);
    let any = false;
    for (let i = 0; i < q.length; i++) { q[i] = Math.max(-32767, Math.min(32767, Math.round(t[i]))); any = any || q[i] !== 0; }
    if (any) keep.push({tx: Math.floor(k / 65536) - 32768, tz: (k % 65536) - 32768, q});
  }
  if (!keep.length) return null;
  keep.sort((a, b) => a.tz - b.tz || a.tx - b.tx);   // canonical order: same layer, same bytes
  const out = new Uint8Array(32 + keep.length * 2056);
  const d = new DataView(out.buffer);
  d.setUint32(0, SC_MAGIC, true); d.setUint32(4, SC_VERSION, true);
  d.setUint32(8, 3, true); d.setUint32(12, 5, true); d.setUint32(16, keep.length, true);
  keep.forEach((t, i) => {
    const o = 32 + i * 2056;
    d.setInt32(o, t.tx, true); d.setInt32(o + 4, t.tz, true);
    for (let k = 0; k < t.q.length; k++) d.setInt16(o + 8 + k * 2, t.q[k], true);
  });
  return out;
}
function scClone(tiles) { const m = new Map(); for (const [k, t] of tiles) m.set(k, new Float32Array(t)); return m; }
function scRound(tiles) {
  const m = new Map();
  for (const [k, t] of tiles) {
    const r = new Float32Array(t.length);
    let any = false;
    for (let i = 0; i < t.length; i++) { r[i] = Math.max(-32767, Math.min(32767, Math.round(t[i]))); any = any || r[i] !== 0; }
    if (any) m.set(k, r);
  }
  return m;
}
/** The engine's height at a world column as far as the page knows it: the
 *  --heightmap backdrop with the layer it was rendered with taken out and the
 *  one being edited put in. null where no heights are loaded. */
function heightsAt(x, z) {
  if (!heights || !heights.D) return null;
  const D = heights.D, step = D.span / D.res;
  const fi = (x - (D.cx - D.span / 2)) / step - 0.5, fj = (z - (D.cz - D.span / 2)) / step - 0.5;
  if (fi < 0 || fj < 0 || fi > D.res - 1 || fj > D.res - 1) return null;
  const i = Math.min(D.res - 2, Math.floor(fi)), j = Math.min(D.res - 2, Math.floor(fj));
  const u = fi - i, v = fj - j;
  const h = (a, b) => D.h[b * D.res + a];
  const a = h(i, j) + (h(i + 1, j) - h(i, j)) * u, b = h(i, j + 1) + (h(i + 1, j + 1) - h(i, j + 1)) * u;
  return a + (b - a) * v;
}
function groundAt(x, z) {
  const hb = heightsAt(x, z);
  return hb == null ? null : hb - scAt(heights.base, x, z) + scAt(sculpt.tiles, x, z);
}
/** One dab of the sculpt brush at world column (wx, wz). Returns true if the
 *  layer changed. `before` collects each touched tile's prior state for undo. */
function sculptDab(wx, wz, before) {
  const r = Math.max(4, sculpt.radius), mode = sculpt.mode;
  const k = Math.max(0.01, Math.min(1, sculpt.strength / 100));
  const amt = sculpt.strength / 5;
  const sx0 = Math.floor((wx - r) / SC_SP), sx1 = Math.ceil((wx + r) / SC_SP);
  const sz0 = Math.floor((wz - r) / SC_SP), sz1 = Math.ceil((wz + r) / SC_SP);
  let smoothSrc = null;
  if (mode === 'smooth') {
    smoothSrc = new Map();
    for (let sz = sz0 - 1; sz <= sz1 + 1; sz++)
      for (let sx = sx0 - 1; sx <= sx1 + 1; sx++) {
        const g = groundAt(sx * SC_SP, sz * SC_SP);
        smoothSrc.set(sx + ',' + sz, g == null ? scSample(sculpt.tiles, sx, sz) : g);
      }
  }
  let changed = false;
  for (let sz = sz0; sz <= sz1; sz++)
    for (let sx = sx0; sx <= sx1; sx++) {
      const d = Math.hypot(sx * SC_SP - wx, sz * SC_SP - wz);
      if (d >= r) continue;
      // a locale open: only samples whose bilinear reach stays inside its bounds
      if (LC && !LC.sampleAllowed(sx, sz)) continue;
      const w = 0.5 * (1 + Math.cos(Math.PI * d / r));
      const key = scKey(sx >> 5, sz >> 5);
      const cur = scSample(sculpt.tiles, sx, sz);
      let next = cur;
      if (mode === 'raise') next = cur + amt * w;
      else if (mode === 'lower') next = cur - amt * w;
      else if (mode === 'erase') next = cur - cur * k * w;
      else if (mode === 'smooth') {
        let s = 0;
        for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) s += smoothSrc.get((sx + dx) + ',' + (sz + dz));
        const here = smoothSrc.get(sx + ',' + sz);
        next = cur + (s / 9 - here) * k * w;
      } else if (mode === 'flatten') {
        if (sculpt.target == null) continue;
        const g = groundAt(sx * SC_SP, sz * SC_SP);
        if (g == null) continue;
        next = cur + (sculpt.target - g) * k * w;
      }
      if (Math.abs(next - cur) < 1e-4) continue;
      let t = sculpt.tiles.get(key);
      if (!t) {
        if (before && !before.has(key)) before.set(key, null);
        t = new Float32Array(SC_T * SC_T);
        sculpt.tiles.set(key, t);
      } else if (before && !before.has(key)) before.set(key, new Float32Array(t));
      t[(sz & 31) * SC_T + (sx & 31)] = next;
      sculpt.tver.set(key, (sculpt.tver.get(key) || 0) + 1);
      changed = true;
    }
  if (changed) {
    sculpt.version++;
    const R = sculpt.dirtyRect, x0 = wx - r - SC_SP, x1 = wx + r + SC_SP, z0 = wz - r - SC_SP, z1 = wz + r + SC_SP;
    sculpt.dirtyRect = R ? [Math.min(R[0], x0), Math.min(R[1], z0), Math.max(R[2], x1), Math.max(R[3], z1)] : [x0, z0, x1, z1];
  }
  return changed;
}
/** Swap the tiles named in `m` (key -> Float32Array | null) into the layer;
 *  returns the same keys' current state, for the opposite stack. */
function sculptSwap(m) {
  const back = new Map();
  for (const [k, t] of m) {
    const cur = sculpt.tiles.get(k);
    back.set(k, cur ? new Float32Array(cur) : null);
    if (t) sculpt.tiles.set(k, new Float32Array(t)); else sculpt.tiles.delete(k);
    sculpt.tver.set(k, (sculpt.tver.get(k) || 0) + 1);
  }
  sculpt.version++;
  sculpt.dirty = true;
  sculpt.dirtyRect = null;
  if (heights && heights.D) reshadeHeights(null);
  return back;
}
async function loadSculpt(name) {
  const r = await fetch('/api/worldmap/sculpt?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (r.status === 404) return new Map();
  if (!r.ok) throw new Error('sculpt.svsculpt: HTTP ' + r.status);
  return parseSculpt(await r.arrayBuffer());
}

/* ---- helpers ---------------------------------------------------------------- */
function toast(m, bad) { if (H && H.toast) H.toast(m, bad); }
function markDirty(d = true) {
  dirty = d;
  if (H && H.onDirty) H.onDirty(d);
  if (els.save) els.save.disabled = !d;
}
/** A site's column, world voxels: `at: [x, z]`, or a pre-P6 stamp's x / z. */
function siteXZ(s) {
  if (Array.isArray(s.at)) return s.at;
  return [s.x | 0, s.z | 0];
}
const vpm = () => (map && map.json.terrain && map.json.terrain.refVoxelsPerMetre) || 10;
const seaY = () => (map ? map.json.seaLevelY | 0 : 0);
const fmtM = m => (Math.abs(m) >= 1000 ? (m / 1000).toFixed(2) + ' km' : (Math.abs(m) >= 100 ? m.toFixed(0) : m.toFixed(1)) + ' m');
/** The map.json state an undo step restores (everything but the planes). */
function snapshot() { return {json: JSON.stringify(map.json)}; }
function restore(s) {
  const id = selected && selected.id, lid = lines.sel && lines.sel.id;
  map.json = JSON.parse(s.json);
  selected = id ? (map.json.sites || []).find(x => x.id === id) || null : null;
  lines.sel = lid ? (map.json.ridges || []).find(x => x.id === lid) || null : null;
  if (!lines.sel) lines.drawing = false;
  syncTerrain(); syncSites(); syncEditSel(); publishTerrain(); syncLines();
  if (VL) VL.afterRestore();
  if (RL) RL.afterRestore();
  if (WL) WL.afterRestore();
  if (LC) LC.afterRestore();
}
function pushUndo(e) { undo.push(e); if (undo.length > 60) undo.shift(); redo = []; }
/** The map's terrain block, created from the defaults if the file has none. */
function terrainOf() {
  const j = map.json;
  if (!TERRAIN_DEFAULTS) {
    if (!j.terrain || typeof j.terrain !== 'object') j.terrain = {};
    if (!j.terrain.homeArea || typeof j.terrain.homeArea !== 'object') j.terrain.homeArea = {};
    return j.terrain;
  }
  if (!j.terrain || typeof j.terrain !== 'object') j.terrain = JSON.parse(JSON.stringify(TERRAIN_DEFAULTS));
  if (!j.terrain.homeArea || typeof j.terrain.homeArea !== 'object') j.terrain.homeArea = {...TERRAIN_DEFAULTS.homeArea};
  for (const k of Object.keys(TERRAIN_DEFAULTS))
    if (k !== 'homeArea' && k !== 'landformRangeVox' && typeof j.terrain[k] !== 'number') j.terrain[k] = TERRAIN_DEFAULTS[k];
  for (const k of Object.keys(TERRAIN_DEFAULTS.homeArea))
    if (typeof j.terrain.homeArea[k] !== 'number') j.terrain.homeArea[k] = TERRAIN_DEFAULTS.homeArea[k];
  return j.terrain;
}
function rowGet(path) { return path[0] === '@' ? map.json[path.slice(1)] : getPath(terrainOf(), path); }
function rowSet(path, v) { if (path[0] === '@') map.json[path.slice(1)] = v; else setPath(terrainOf(), path, v); }
function getPath(o, path) { return path.split('.').reduce((a, k) => (a == null ? a : a[k]), o); }
function setPath(o, path, v) {
  const ks = path.split('.');
  let a = o;
  for (let i = 0; i < ks.length - 1; i++) a = a[ks[i]];
  a[ks[ks.length - 1]] = v;
}
// The preview pane (the heightmap / voxel views in tuner.html) reads the
// treeline and the home height for its colours and its camera.
function publishTerrain() {
  if (!map) return;
  const t = terrainOf();
  const sp = (map.json.sites || []).find(s => s.kind === 'spawn');
  const at = sp && Array.isArray(sp.at) ? sp.at : SPAWN_DEFAULT;
  // homeY: the AUTHORED ground at the spawn (map v2 A1 retired homeArea.y).
  const homeY = map.H ? Math.round(MT.heightAt(map.H, at[0], at[1])) : t.baseHeight;
  window.wgTerrain = {treeline: t.treeline, homeY, homeR: t.homeArea.radius, homeFade: t.homeArea.fade,
                      seaLevelY: seaY(), spawnX: at[0], spawnZ: at[1]};
}

/* ---- server ------------------------------------------------------------------ */
async function listMaps() {
  const r = await fetch('/api/worldmaps', {cache: 'no-store'});
  const j = await r.json();
  return (j.maps || []).map(m => m.name);
}
async function listTiles(name, plane) {
  const r = await fetch('/api/worldmap/tiles?name=' + encodeURIComponent(name) + '&plane=' + plane, {cache: 'no-store'});
  const j = await r.json();
  if (!j.ok) throw new Error(plane + ' tiles: ' + (j.error || 'HTTP ' + r.status));
  return j.tiles || [];
}
/** Load map `name` (v2 only). A v1 map is refused with the converter's name. */
async function loadMap(name) {
  const rj = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (!rj.ok) throw new Error('map.json: HTTP ' + rj.status);
  const json = await rj.json();
  if (json.version !== 2) {
    const e = new Error(`worldmap/${name} is a v1 map (cellLog2 planes); this page edits v2 maps -- convert it with python scripts/convert_map_v2.py ${name}`);
    e.v1 = true;
    throw e;
  }
  if (!json.extent || !Array.isArray(json.extent.min) || !Array.isArray(json.extent.size) || !json.planes)
    throw new Error(`worldmap/${name}: map.json v2 needs extent and planes`);
  const m = {name, json, H: MT.makePlane('height', json), B: MT.makePlane('biome', json)};
  const saved = JSON.parse(JSON.stringify(json));
  const [lh, lb] = await Promise.all([listTiles(name, 'height'), listTiles(name, 'biome')]);
  for (const t of lh) m.H.onDisk.add(MT.tileKey(t.tx, t.tz));
  for (const t of lb) m.B.onDisk.add(MT.tileKey(t.tx, t.tz));
  const tiles = await loadSculpt(name);
  map = m;
  if (els.mapSel && [...els.mapSel.options].some(o => o.value === name)) els.mapSel.value = name;
  loads.queue = []; loads.want.clear(); loads.gen++;
  imgCache.clear();
  sculpt.tiles = tiles; sculpt.saved = scClone(tiles); sculpt.dirty = false; sculpt.version++; sculpt.dirtyRect = null;
  undo = []; redo = [];
  selected = null; heights = null; showHeights = false;
  if (erosion.running) erosion.running.cancel();
  bakes = new Map(); area = null; erosion.pending = null; pngIn = null;
  lines.sel = null; lines.drawing = false; lines.drag = null;
  savedJson = saved;
  if (VL) VL.reset();
  if (RL) RL.reset();
  if (GL) GL.reset();
  if (OL) OL.reset();
  if (CLM) CLM.reset();
  if (CST) CST.reset();
  if (WL) WL.reset();
  preview = null;
  if (LC) LC.reset();
  if (els.heightsBtn) els.heightsBtn.classList.remove('on');
  brush.index = Math.min(brush.index, json.biomes.length - 1);
  range.vals = null;
  markDirty(false);
  syncEditSel();
  migrateEditLayer();
  rebuildPalette();
  terrainOf();
  syncTerrain(); syncSites(); publishTerrain(); syncLines(); syncHeightTools();
  check = null; syncCheck(); fetchCheck();
  fit();
  // every listed tile, in the background, after what is on screen
  for (const P of [m.H, m.B]) for (const k of P.onDisk) loads.queue.push([P, k]);
  paint();
  const [sx, sz] = json.extent.size, v = vpm();
  status(`loaded worldmap/${name}: ${(sx / v / 1000).toFixed(1)} x ${(sz / v / 1000).toFixed(1)} km, height ${m.H.posts[0]}² posts every ${(m.H.sp / v).toFixed(1)} m ` +
         `(${m.H.onDisk.size} tiles on disk), biome ${m.B.posts[0]}² cells of ${(m.B.sp / v).toFixed(1)} m (${m.B.onDisk.size} tiles)` +
         (tiles.size ? `, sculpt layer ${tiles.size} tiles` : ''));
}
/* Tile loading: visible tiles first (paint() puts them at the front), the
 * rest in the background, a few requests in flight. A tile that arrives after
 * the map changed is dropped. */
const loads = {queue: [], want: new Set(), active: 0, max: 4, gen: 0, failed: 0};
function wantTiles(P, keys) {
  const front = [];
  for (const k of keys) if (MT.blocked(P, k) && !loads.want.has(P.name + k)) front.push([P, k]);
  if (front.length) loads.queue.unshift(...front);
  pumpLoads();
}
function pumpLoads() {
  while (loads.active < loads.max && loads.queue.length) {
    const [P, k] = loads.queue.shift();
    const id = P.name + k;
    if (!MT.blocked(P, k) || loads.want.has(id)) continue;
    loads.want.add(id);
    loads.active++;
    const gen = loads.gen, name = map.name, [tx, tz] = MT.tileXZ(k);
    fetch(`/api/worldmap/tile?name=${encodeURIComponent(name)}&plane=${P.name}&tx=${tx}&tz=${tz}`, {cache: 'no-store'})
      .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.arrayBuffer(); })
      .then(buf => {
        if (gen !== loads.gen) return;
        const t = MT.decodeTile(P, buf, tx, tz);
        if (!P.tiles.has(k)) { P.tiles.set(k, t); MT.touchRect(P, tx * MT.TILE, tz * MT.TILE, tx * MT.TILE + 255, tz * MT.TILE + 255); }
        if (!stroke) range.vals = null;
        schedulePaint();
      })
      .catch(e => { if (gen === loads.gen) { loads.failed++; status(`${P.name} tile (${tx},${tz}) failed to load: ${e.message} (it stays uneditable)`); } })
      .finally(() => { loads.active--; if (gen === loads.gen) { loads.want.delete(id); pumpLoads(); } });
  }
}
// The water presets for the Water tool (P-F): the names under assets/water/
// and, per preset, the radius and shore/berm band in voxels the engine
// derives (worldmap.cpp WaterGeomOf: 10 voxels to the metre, band =
// max(shore.band, berm.width)) so a lake's footprint draws to scale.
async function loadWaterPresets() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const names = [...new Set((j.files || [])
      .filter(f => f.dir === 'water' && f.name.endsWith('.json') && f.name[0] !== '_')
      .map(f => f.name.slice(0, -5)))].sort();
  water.presets = names;
  for (const n of names) {
    try {
      const rp = await fetch('/api/model?path=water/' + encodeURIComponent(n + '.json'), {cache: 'no-store'});
      const p = await rp.json();
      const fp = p.footprint || {}, sh = p.shore || {}, be = p.berm || {};
      water.geom[n] = {
        radius: Math.max(4, Math.round((fp.radius || 0) * 10)),
        band: Math.max(1, Math.round(Math.max(sh.band || 0, be.width || 0) * 10))
      };
    } catch (e) { water.geom[n] = {radius: 60, band: 24}; }
  }
  if (els.waterPreset) {
    els.waterPreset.replaceChildren(...names.map(n => H.el('option', {value: n}, n)));
    if (!water.preset && names.length) water.preset = names.includes('tarn') ? 'tarn' : names[0];
    els.waterPreset.value = water.preset;
  }
}
async function loadStampTemplates() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  stamp.templates = [...new Set((j.files || [])
      .filter(f => f.dir === 'prefabs' && f.name.toLowerCase().endsWith('.vox'))
      .map(f => f.name.slice(0, -4)))].sort();
  if (!stamp.templates.includes(stamp.template)) stamp.template = stamp.templates[0] || '';
  if (els.stampTpl) {
    els.stampTpl.replaceChildren(...(stamp.templates.length
      ? stamp.templates.map(n => H.el('option', {value: n}, n))
      : [H.el('option', {value: ''}, '(no .vox in prefabs/)')]));
    els.stampTpl.value = stamp.template;
  }
}
async function loadTreeSpecies() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  trees.list = [...new Set((j.files || [])
      .filter(f => f.dir === 'trees' && f.name.endsWith('.svtree'))
      .map(f => f.name.slice(0, -7)))].sort();
  if (!trees.list.includes(trees.species)) trees.species = trees.list.includes('oak') ? 'oak' : (trees.list[0] || '');
  if (els.treeSp) {
    els.treeSp.replaceChildren(...(trees.list.length
      ? trees.list.map(n => H.el('option', {value: n}, n))
      : [H.el('option', {value: ''}, '(no .svtree in trees/)')]));
    els.treeSp.value = trees.species;
  }
}
/* ---- the LOAD WARNINGS panel (P6) ----------------------------------------------
 * What the engine's loader says about the SAVED map: `sandvox --mapcheck
 * <name>` through /api/worldmap/check. Fetched on load and after every save,
 * never per edit: the answer is about the files. */
async function fetchCheck() {
  if (!map) return;
  const name = map.name;
  let j;
  try {
    const r = await fetch('/api/worldmap/check?name=' + encodeURIComponent(name), {cache: 'no-store'});
    j = await r.json();
    if (!r.ok && !j.checked) j = {unavailable: true, error: j.error || ('HTTP ' + r.status)};
  } catch (e) { j = {unavailable: true, error: e.message}; }
  if (!map || map.name !== name) return;
  check = j;
  syncCheck();
}
function syncCheck() {
  if (!els.checkList) return;
  const el = H.el;
  els.checkList.innerHTML = '';
  const c = check;
  let head;
  if (!c) head = 'not checked yet';
  else if (c.unavailable) head = 'the engine could not check this map: ' + (c.error || '?');
  else if (!c.ok) head = 'the engine REFUSES this map';
  else head = (c.warnings || []).length ? (c.warnings.length + ' warning' + (c.warnings.length === 1 ? '' : 's') + ' (the saved files)') : 'no warnings (the saved files)';
  if (els.checkSummary) {
    els.checkSummary.textContent = 'Load check — ' + (c && !c.unavailable ? (!c.ok ? 'REFUSED' : ((c.warnings || []).length + ' warning' + ((c.warnings || []).length === 1 ? '' : 's'))) : '?');
    els.checkSummary.classList.toggle('bad', !!(c && !c.unavailable && (!c.ok || (c.warnings || []).length)));
  }
  els.checkList.append(el('div', {class: 'mapnote'}, head));
  const lines = c && !c.unavailable ? (c.ok ? (c.warnings || []) : [c.error || '?']) : [];
  for (const w of lines) {
    const row = el('div', {class: 'mapwarn' + (c.ok ? '' : ' err')}, w);
    const ids = [...String(w).matchAll(/site "([^"]+)"/g)].map(m => m[1]);
    const hit = map && (map.json.sites || []).find(s => ids.includes(s.id));
    if (hit) {
      row.classList.add('link');
      row.title = 'select ' + hit.id;
      row.addEventListener('click', () => {
        selected = hit;
        const [x, z] = siteCentre(hit);
        view.cx = x; view.cz = z;
        syncSites(); paint();
      });
    }
    const vm = !hit && map && String(w).match(/\b(road|region|marker) "([^"]+)"/);
    if (vm && VL && (map.json[vm[1] + 's'] || []).some(o => o.id === vm[2])) {
      row.classList.add('link');
      row.title = 'select ' + vm[1] + ' ' + vm[2];
      row.addEventListener('click', () => { tool = vm[1]; syncControls(); VL.selectById(vm[1], vm[2], true); });
    }
    const wm = !hit && !vm && map && String(w).match(/\b(river|lake) "([^"]+)"/);
    if (wm && WL && (map.json[wm[1] + 's'] || []).some(o => o.id === wm[2])) {
      row.classList.add('link');
      row.title = 'select ' + wm[1] + ' ' + wm[2];
      row.addEventListener('click', () => { tool = wm[1]; syncControls(); WL.selectById(wm[1], wm[2], true); });
    }
    const lm = !hit && !vm && !wm && map && String(w).match(/\blocale "([^"]+)"/);
    if (lm && (map.json.locales || []).some(l => l.id === lm[1])) {
      row.classList.add('link');
      row.title = 'select locale ' + lm[1];
      row.addEventListener('click', () => { if (!LC.open()) { LC.st.sel = lm[1]; tool = 'locale'; syncControls(); LC.sync(); paint(); } });
    }
    els.checkList.append(row);
  }
  if (VL) VL.sync();
  if (LC) LC.sync();
  if (WL) WL.sync();
}
function buildCheckPanel() {
  const el = H.el;
  const det = el('details', {class: 'mapcheck', open: true});
  els.checkSummary = el('summary', {}, 'Load check');
  els.checkList = el('div', {class: 'mapcheck-list'});
  const again = el('button', {title: 'ask the engine again (sandvox --mapcheck on the saved files)'}, 'Re-check');
  again.addEventListener('click', () => fetchCheck());
  det.append(els.checkSummary, els.checkList, again);
  return det;
}
/** What a save would POST for the planes: {height: [...], biome: [...]}, each
 *  entry {tx, tz, key, bytes | null} (null = delete the tile file). */
function planePayload() { return {height: MT.dirtyPayload(map.H), biome: MT.dirtyPayload(map.B)}; }
async function saveMap() {
  if (!map) return;
  const name = map.name;
  // The engine REFUSES the whole map over a road with one point, a region
  // with two, an id twice: never write one.
  const vp = [...((VL && VL.problems()) || []), ...((LC && LC.problems()) || []), ...((WL && WL.problems()) || [])];
  if (vp.length) { toast('not saved: ' + vp.join('; ') + ' (the engine would refuse the whole map)', true); return false; }
  const body = JSON.stringify(map.json, null, 2) + '\n';
  let r = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {method: 'POST', body});
  let j = await r.json();
  if (!j.ok) { toast('save failed: ' + (j.error || '?'), true); return false; }
  // Only the dirty tiles; a tile painted back to the default is deleted.
  const pay = planePayload();
  let sent = 0, deleted = 0;
  for (const [pn, P] of [['height', map.H], ['biome', map.B]]) {
    const done = [];
    for (const e of pay[pn]) {
      r = await fetch(`/api/worldmap/tile?name=${encodeURIComponent(name)}&plane=${pn}&tx=${e.tx}&tz=${e.tz}`,
                      {method: 'POST', body: e.bytes || new Uint8Array(0)});
      j = await r.json();
      if (!j.ok) { MT.markSaved(P, done); toast(`${pn} tile (${e.tx},${e.tz}) save failed: ${j.error || '?'}`, true); return false; }
      done.push(e);
      if (e.bytes) sent++; else deleted++;
    }
    MT.markSaved(P, done);
  }
  if (sculpt.dirty) {
    const sb = serializeSculpt(sculpt.tiles);
    r = await fetch('/api/worldmap/sculpt?name=' + encodeURIComponent(name), {method: 'POST', body: sb || new Uint8Array(0)});
    j = await r.json();
    if (!j.ok) { toast('sculpt save failed: ' + (j.error || '?'), true); return false; }
    sculpt.tiles = scRound(sculpt.tiles);
    sculpt.saved = scClone(sculpt.tiles);
    sculpt.dirty = false;
  }
  markDirty(false);
  savedJson = JSON.parse(body);
  savedGen++;
  range.vals = null;
  fetchCheck();
  if (H.saved) H.saved();
  if (showHeights) fetchHeights().catch(e => status('heights failed: ' + e.message));
  paint();
  toast(`saved worldmap/${name} (${sent} tile${sent === 1 ? '' : 's'} written, ${deleted} deleted) — press Apply to game (or F7 in the game) to see it; every map edit moves the world hash`);
  return true;
}

/* ---- the topographic display ------------------------------------------------------------ */
// Hypsometric ramp for LAND, sea level (t 0) to the plane's real max (t 1):
// lowland green, upland tan, brown, rock grey, snow white. WATER: shallow to
// the plane's real min, light to deep blue.
const LAND_STOPS = [[0, [79, 138, 74]], [0.12, [128, 170, 88]], [0.3, [208, 200, 128]], [0.5, [184, 140, 86]],
                    [0.72, [140, 124, 112]], [0.88, [190, 188, 186]], [1, [250, 250, 252]]];
const SEA_STOPS = [[0, [128, 180, 222]], [1, [24, 52, 104]]];
function stopsLut(stops) {
  const lut = new Uint8Array(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = i / 255;
    let k = 0;
    while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
    const [t0, a] = stops[k], [t1, b] = stops[k + 1], u = Math.max(0, Math.min(1, (t - t0) / (t1 - t0)));
    for (let c = 0; c < 3; c++) lut[i * 3 + c] = Math.round(a[c] + (b[c] - a[c]) * u);
  }
  return lut;
}
const LAND_LUT = stopsLut(LAND_STOPS), SEA_LUT = stopsLut(SEA_STOPS);
// the slope-band overlay's colours: walkable, steep, cliff (maplegend.js draws the same)
const SLOPE_COL = [[92, 160, 80], [214, 180, 72], [200, 80, 60]];
// The ramp's range: the height plane's real min / max (plus the backdrop's,
// when shown). Recomputed when it is null and no stroke is running, so a
// stroke that climbs past the max does not rebuild every tile per dab.
let range = {vals: null, ver: 0};
function rampRange() {
  if (!range.vals && !stroke) {
    let [lo, hi] = MT.planeRange(map.H);
    if (showHeights && heights && heights.D) { lo = Math.min(lo, heights.D.hMin); hi = Math.max(hi, heights.D.hMax); }
    const v = [lo, hi];
    if (!range.last || range.last[0] !== lo || range.last[1] !== hi) range.ver++;
    range.last = v;
    range.vals = v;
  }
  return range.last || [seaY() - 100, seaY() + 100];
}
/** The colour of height h (voxels) on the ramp, into out[0..2]. */
function rampColor(h, lo, hi, sea, out) {
  if (h < sea) {
    const t = Math.min(1, (sea - h) / Math.max(1, sea - lo));
    const i = (t * 255) | 0;
    out[0] = SEA_LUT[i * 3]; out[1] = SEA_LUT[i * 3 + 1]; out[2] = SEA_LUT[i * 3 + 2];
  } else {
    const t = Math.min(1, (h - sea) / Math.max(1, hi - sea));
    const i = (t * 255) | 0;
    out[0] = LAND_LUT[i * 3]; out[1] = LAND_LUT[i * 3 + 1]; out[2] = LAND_LUT[i * 3 + 2];
  }
}
/** Contour interval in VOXELS for the current zoom (disp.interval 0 = auto:
 *  lines no closer than ~3 screen pixels on a 45-degree slope). */
function contourInterval() {
  const v = vpm();
  if (disp.interval) return disp.interval * v;
  const mPerPx = 1 / view.scale / v;
  for (const m of [10, 25, 50, 100, 250, 500, 1000]) if (m >= mPerPx * 3) return m * v;
  return 1000 * v;
}
// 'h|k|L' -> {stamp, canvas}, least recently used first (a Map keeps insertion
// order; a hit re-inserts). A level-0 tile image is 256 KB, so the cap bounds
// the cache near 100 MB however far a session pans at full zoom.
const imgCache = new Map(), IMG_CACHE_MAX = 400;
function cacheGet(id, stampKey, build) {
  let e = imgCache.get(id);
  if (e) { imgCache.delete(id); imgCache.set(id, e); }
  if (e && e.stamp === stampKey) return e.canvas;
  const canvas = build(e ? e.canvas : null);
  if (!e && imgCache.size >= IMG_CACHE_MAX) imgCache.delete(imgCache.keys().next().value);
  imgCache.set(id, {stamp: stampKey, canvas});
  stats.tileImages++;
  return canvas;
}
function tileCanvas(old, n) {
  const c = old || document.createElement('canvas');
  if (c.width !== n || c.height !== n) { c.width = n; c.height = n; }
  return c;
}
/** The topographic image of height tile (tx, tz) at mip level L. */
function heightImage(P, tx, tz, L, old, style) {
  const n = MT.TILE >> L, k = MT.tileKey(tx, tz);
  const own = MT.mipLevel(P, k, L);
  // the level-L lattice and its clamp (edge tiles hang past the plane)
  const NX = Math.ceil(P.posts[0] / (1 << L)), NZ = Math.ceil(P.posts[1] / (1 << L));
  const gs = (gi, gj) => {
    gi = gi < 0 ? 0 : gi >= NX ? NX - 1 : gi; gj = gj < 0 ? 0 : gj >= NZ ? NZ - 1 : gj;
    const ti = (gi / n) | 0, tj = (gj / n) | 0;
    const m = ti === tx && tj === tz ? own : MT.mipLevel(P, MT.tileKey(ti, tj), L);
    return m[(gj - tj * n) * n + (gi - ti * n)];
  };
  // (n + 2)^2 with a one-sample apron from the neighbours
  const W = n + 2, g = new Float32Array(W * W);
  const bx = tx * n, bz = tz * n;
  const vx = Math.min(n, NX - bx), vz = Math.min(n, NZ - bz);
  for (let j = -1; j <= n; j++)
    for (let i = -1; i <= n; i++) {
      const inside = i >= 0 && j >= 0 && i < vx && j < vz;
      g[(j + 1) * W + i + 1] = inside ? own[j * n + i] : gs(bx + Math.min(i, vx), bz + Math.min(j, vz));
    }
  const c = tileCanvas(old, n), ctx = c.getContext('2d');
  const im = ctx.createImageData(n, n), d = im.data;
  const {lo, hi, sea, I, shade, contours, slope} = style;
  const sp2 = 2 * (P.sp << L), ex = 1.5;
  const col = [0, 0, 0];
  let cpx = 0;
  for (let j = 0; j < vz; j++)
    for (let i = 0; i < vx; i++) {
      const o = (j + 1) * W + i + 1, h = g[o];
      if (slope && h >= sea) {
        // THE SLOPE BANDS (mapoverlay.js): walkable / steep / cliff by the game's limits
        const gx = (g[o + 1] - g[o - 1]) / sp2, gz = (g[o + W] - g[o - W]) / sp2;
        const deg = Math.atan(Math.sqrt(gx * gx + gz * gz)) * 57.29577951308232;
        const c = deg >= slope[1] ? SLOPE_COL[2] : deg >= slope[0] ? SLOPE_COL[1] : SLOPE_COL[0];
        col[0] = c[0]; col[1] = c[1]; col[2] = c[2];
      } else rampColor(h, lo, hi, sea, col);
      let k2 = 1;
      if (shade) {
        // normal of (x, h, z) with exaggeration ex, lit from the north-west, 45 degrees up
        const dx = (g[o + 1] - g[o - 1]) / sp2 * ex, dz = (g[o + W] - g[o - W]) / sp2 * ex;
        const s = (dx * 0.5 + 0.7071 + dz * 0.5) / Math.sqrt(dx * dx + 1 + dz * dz);
        k2 = 0.3 + 0.95 * Math.max(0, s);
      }
      if (contours) {
        const a = Math.floor((h - sea) / I), b = Math.floor((g[o + 1] - sea) / I), e = Math.floor((g[o + W] - sea) / I);
        if (a !== b || a !== e) {
          const top = Math.max(a, b, e), bot = Math.min(a, b, e);
          const bold = Math.floor(top / 5) * 5 > bot;   // a multiple-of-5 level lies between
          k2 *= bold ? 0.32 : 0.62;
          cpx++;
        }
      }
      const p = (j * n + i) * 4;
      d[p] = Math.min(255, col[0] * k2); d[p + 1] = Math.min(255, col[1] * k2); d[p + 2] = Math.min(255, col[2] * k2); d[p + 3] = 255;
    }
  stats.contourPx += cpx;
  ctx.putImageData(im, 0, 0);
  return c;
}
/** The biome colours of biome tile (tx, tz) at level L (nearest: a cell's
 *  centre sample, no blending -- a biome is a category). */
function biomeImage(P, tx, tz, L, old) {
  const n = MT.TILE >> L, t = P.tiles.get(MT.tileKey(tx, tz));
  const [sx, sz] = MT.tileSpan(P, tx, tz);
  const pal = palette();
  const c = tileCanvas(old, n), ctx = c.getContext('2d');
  const im = ctx.createImageData(n, n), d = im.data, half = (1 << L) >> 1;
  for (let j = 0; j < n; j++) {
    const sj = Math.min((j << L) + half, MT.TILE - 1);
    if ((j << L) >= sz) break;
    for (let i = 0; i < n; i++) {
      const si = Math.min((i << L) + half, MT.TILE - 1);
      if ((i << L) >= sx) break;
      const v = t ? t[sj * MT.TILE + si] : P.def;
      const p = pal[v] || pal[0], o = (j * n + i) * 4;
      d[o] = p[0]; d[o + 1] = p[1]; d[o + 2] = p[2]; d[o + 3] = 255;
    }
  }
  ctx.putImageData(im, 0, 0);
  return c;
}
let palCache = {key: '', pal: []};
function palette() {
  const key = map.json.biomes.join('\n');
  if (palCache.key === key) return palCache.pal;
  const tmp = document.createElement('canvas').getContext('2d', {willReadFrequently: true});
  const pal = map.json.biomes.map(n => {
    tmp.clearRect(0, 0, 1, 1);
    tmp.fillStyle = colorOf(n); tmp.fillRect(0, 0, 1, 1);
    return Array.from(tmp.getImageData(0, 0, 1, 1).data);
  });
  palCache = {key, pal};
  imgCache.clear();
  return pal;
}
/** Screen <-> world (voxels). */
const SX = x => els.canvas.clientWidth / 2 + (x - view.cx) * view.scale;
const SZ = z => els.canvas.clientHeight / 2 + (z - view.cz) * view.scale;
/** The mip level whose samples are at least one screen pixel. */
function levelFor(sp) { const ppp = sp * view.scale; return ppp >= 1 ? 0 : Math.min(8, Math.ceil(Math.log2(1 / ppp))); }
/** Visible tiles of plane P: [tx0, tz0, tx1, tz1] (inclusive), clamped. */
function visibleTiles(P, centred) {
  const W = els.canvas.clientWidth, Hh = els.canvas.clientHeight, span = MT.TILE * P.sp;
  const x0 = view.cx - W / 2 / view.scale, x1 = view.cx + W / 2 / view.scale;
  const z0 = view.cz - Hh / 2 / view.scale, z1 = view.cz + Hh / 2 / view.scale;
  const o = centred ? 0 : P.sp / 2;
  return [Math.max(0, Math.floor((x0 - P.min[0] + o) / span)), Math.max(0, Math.floor((z0 - P.min[1] + o) / span)),
          Math.min(P.tilesX - 1, Math.floor((x1 - P.min[0] + o) / span)), Math.min(P.tilesZ - 1, Math.floor((z1 - P.min[1] + o) / span))];
}
/** Draw a plane's visible tiles; `post` = a post lattice (images sit half a
 *  spacing west / north of the first post). Returns the listed tiles not loaded. */
function drawPlane(ctx, P, post, imgFor) {
  const [tx0, tz0, tx1, tz1] = visibleTiles(P, !post);
  const span = MT.TILE * P.sp, o = post ? P.sp / 2 : 0;
  const L = levelFor(P.sp), missing = [];
  for (let tz = tz0; tz <= tz1; tz++)
    for (let tx = tx0; tx <= tx1; tx++) {
      const k = MT.tileKey(tx, tz);
      if (MT.blocked(P, k)) missing.push(k);
      const wx = P.min[0] + tx * span - o, wz = P.min[1] + tz * span - o;
      const x = Math.floor(SX(wx)), z = Math.floor(SZ(wz)), x2 = Math.floor(SX(wx + span)), z2 = Math.floor(SZ(wz + span));
      if (x2 <= x || z2 <= z) continue;
      ctx.drawImage(imgFor(tx, tz, k, L), x, z, x2 - x, z2 - z);
    }
  return missing;
}
let paintQueued = false;
function schedulePaint() {
  if (paintQueued) return;
  paintQueued = true;
  requestAnimationFrame(() => { paintQueued = false; paint(); });
}
function paint() {
  if (!els.canvas) return;
  const c = els.canvas, ctx = c.getContext('2d');
  const W = c.clientWidth, Hh = c.clientHeight;
  if (c.width !== W || c.height !== Hh) { c.width = W; c.height = Hh; }
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#0e1219'; ctx.fillRect(0, 0, W, Hh);
  if (!map) return;
  ctx.imageSmoothingEnabled = false;
  const [lo, hi] = rampRange();
  const sea = seaY(), I = contourInterval();
  const slope = disp.overlay === 'slope' ? (disp.slopeBands || [35, 68]) : null;
  const style = {lo, hi, sea, I, shade: disp.shade, contours: disp.contours, slope};
  const styleKey = [range.ver, lo, hi, sea, I, disp.shade, disp.contours, slope ? slope.join('/') : ''].join(',');
  let missing = [];
  if (disp.topo) {
    const P = map.H;
    missing = drawPlane(ctx, P, true, (tx, tz, k, L) =>
      cacheGet('h|' + k + '|' + L, (P.ver.get(k) || 0) + '|' + styleKey, old => heightImage(P, tx, tz, L, old, style)));
    if (missing.length) wantTiles(P, missing);
  }
  if (disp.biomeAlpha > 0 || !disp.topo) {
    const P = map.B, pk = palCache.key;
    ctx.globalAlpha = disp.topo ? disp.biomeAlpha : 1;
    const mb = drawPlane(ctx, P, false, (tx, tz, k, L) =>
      cacheGet('b|' + k + '|' + L, (P.ver.get(k) || 0) + '|' + pk, old => biomeImage(P, tx, tz, L, old)));
    ctx.globalAlpha = 1;
    if (mb.length) wantTiles(P, mb);
  }
  if (showHeights && heights && heights.img && disp.realAlpha > 0) {
    // The engine's own heights (World::TerrainColumn over the SAVED files:
    // authored + noise + sculpt + sites), on the same ramp, over the plane.
    const hi2 = els.heightsOff || (els.heightsOff = document.createElement('canvas'));
    if (hi2.width !== heights.res || hi2.height !== heights.res) { hi2.width = heights.res; hi2.height = heights.res; }
    if (heights.drawn !== heights.ver) { hi2.getContext('2d').putImageData(heights.img, 0, 0); heights.drawn = heights.ver; }
    const D = heights.D, x0 = D.cx - D.span / 2, z0 = D.cz - D.span / 2;
    ctx.globalAlpha = disp.realAlpha;
    ctx.drawImage(hi2, SX(x0), SZ(z0), D.span * view.scale, D.span * view.scale);
    ctx.globalAlpha = 1;
  }
  paintSculpt(ctx);
  paintPreview(ctx);
  paintFrame(ctx);
  paintLines(ctx);
  REG.each('paint', [ctx]);
  if (VL) VL.paint(ctx);
  if (LC) LC.paint(ctx);
  paintArea(ctx);
  paintSites(ctx);
  paintCursor(ctx);
}
/** The extent (solid) and the edge fade (dashed), the origin crosshair. */
function paintFrame(ctx) {
  const e = map.json.extent, f = map.json.edgeFadeVox | 0;
  ctx.strokeStyle = '#ffd866'; ctx.lineWidth = 1;
  ctx.strokeRect(SX(e.min[0]) + 0.5, SZ(e.min[1]) + 0.5, e.size[0] * view.scale, e.size[1] * view.scale);
  if (f > 0) {
    ctx.setLineDash([4, 4]); ctx.strokeStyle = 'rgba(255,216,102,0.45)';
    ctx.strokeRect(SX(e.min[0] - f) + 0.5, SZ(e.min[1] - f) + 0.5, (e.size[0] + 2 * f) * view.scale, (e.size[1] + 2 * f) * view.scale);
    ctx.setLineDash([]);
  }
  const ox = SX(0), oz = SZ(0);
  ctx.strokeStyle = '#ffd866';
  ctx.beginPath(); ctx.moveTo(ox - 8, oz); ctx.lineTo(ox + 8, oz); ctx.moveTo(ox, oz - 8); ctx.lineTo(ox, oz + 8); ctx.stroke();
}
function paintSites(ctx) {
  const sites = map.json.sites || [];
  ctx.font = '11px monospace';
  for (const s of sites) {
    if (s.kind !== 'pad' || !Array.isArray(s.min) || !Array.isArray(s.max)) continue;
    const x0 = SX(s.min[0]), z0 = SZ(s.min[1]), x1 = SX(s.max[0] + 1), z1 = SZ(s.max[1] + 1);
    ctx.strokeStyle = tool === 'pad' ? '#ff7a7a' : '#ffb454'; ctx.lineWidth = 2;
    ctx.strokeRect(x0, z0, Math.max(1, x1 - x0), Math.max(1, z1 - z0));
    ctx.fillStyle = '#ffb454';
    ctx.fillText(s.id || 'pad', x0 + 3, z0 - 3);
  }
  // clearings (lime) and softened ground (sky blue): the box over a faint
  // fill, the feather band as a dashed ring round it
  for (const s of sites) {
    if ((s.kind !== 'clearing' && s.kind !== 'soften') || !Array.isArray(s.min) || !Array.isArray(s.max)) continue;
    const soft = s.kind === 'soften', colr = soft ? '#7ec8ff' : '#c6f06a';
    const f = s.feather == null ? (soft ? 96 : 64) : s.feather;
    const x0 = SX(s.min[0]), z0 = SZ(s.min[1]), x1 = SX(s.max[0] + 1), z1 = SZ(s.max[1] + 1);
    const sel = selected === s;
    ctx.fillStyle = soft ? 'rgba(126,200,255,0.10)' : 'rgba(198,240,106,0.12)';
    ctx.fillRect(x0, z0, Math.max(1, x1 - x0), Math.max(1, z1 - z0));
    ctx.strokeStyle = sel ? '#ffffff' : (tool === s.kind ? '#ff7a7a' : colr); ctx.lineWidth = sel ? 3 : 2;
    ctx.strokeRect(x0, z0, Math.max(1, x1 - x0), Math.max(1, z1 - z0));
    if (f > 0) {
      const fp = f * view.scale;
      ctx.setLineDash([4, 3]); ctx.lineWidth = 1; ctx.strokeStyle = colr;
      ctx.strokeRect(x0 - fp, z0 - fp, (x1 - x0) + 2 * fp, (z1 - z0) + 2 * fp);
      ctx.setLineDash([]);
    }
    ctx.fillStyle = colr;
    if (soft) {
      const pc = (k, d) => (s[k] == null ? d : s[k]) + '%';
      ctx.fillText((s.id || 'soften') + ' (hills ' + pc('hills', 40) + ' bumps ' + pc('bumps', 20) + ' grain ' + pc('grain', 0) +
                   ' paths ' + pc('paths', 10) + ')', x0 + 3, z1 + 12);
    } else ctx.fillText((s.id || 'clearing') + ' (feather ' + f + ')', x0 + 3, z0 - 3);
  }
  // the spawn: a diamond at the column, dim if the map names none
  {
    const s = sites.find(s => s.kind === 'spawn');
    const at = s && Array.isArray(s.at) ? s.at : SPAWN_DEFAULT;
    const sx = SX(at[0]), sz = SZ(at[1]), r = 6;
    ctx.strokeStyle = tool === 'spawn' ? '#ff7a7a' : (s ? '#8dff9a' : '#6f8f75'); ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(sx, sz - r); ctx.lineTo(sx + r, sz); ctx.lineTo(sx, sz + r); ctx.lineTo(sx - r, sz); ctx.closePath();
    ctx.stroke();
    ctx.fillStyle = s ? '#8dff9a' : '#6f8f75';
    ctx.fillText(s ? `spawn (${at[0]},${at[1]})` : `spawn (default ${at[0]},${at[1]})`, sx + r + 3, sz + 4);
  }
  for (const s of sites) {
    if (s.kind !== 'stamp') continue;
    const [wx, wz] = siteXZ(s);
    const sx = SX(wx), sz = SZ(wz), rr = Math.max(3, (s.radius || 16) * view.scale);
    ctx.strokeStyle = tool === 'stamp' ? '#ff7a7a' : '#7fd4ff'; ctx.lineWidth = 2;
    ctx.strokeRect(sx - rr, sz - rr, rr * 2, rr * 2);
    ctx.fillStyle = '#7fd4ff';
    ctx.fillText((s.id || 'site') + ' (' + (s.template || '?') + (s.rot ? ' r' + s.rot : '') + ')', sx + rr + 3, sz + 4);
  }
  for (const s of sites) {
    if (s.kind !== 'water') continue;
    const at = Array.isArray(s.at) ? s.at : [0, 0];
    const sx = SX(at[0]), sz = SZ(at[1]);
    const g = water.geom[s.preset] || {radius: 60, band: 24};
    const rv = s.radius || g.radius;
    const rr = Math.max(3, rv * view.scale), rb = Math.max(rr + 1, (rv + g.band) * view.scale);
    ctx.strokeStyle = tool === 'water' ? '#ff7a7a' : '#5fc8ff'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(sx, sz, rr, 0, Math.PI * 2); ctx.stroke();
    ctx.setLineDash([3, 3]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(sx, sz, rb, 0, Math.PI * 2); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#5fc8ff';
    ctx.fillText((s.id || 'lake') + ' (' + (s.preset || '?') + ' r' + (rv / 10).toFixed(1) + 'm)', sx + rr + 3, sz + 4);
  }
  for (const s of sites) {
    if (s.kind !== 'tree') continue;
    const [wx, wz] = siteXZ(s);
    const sx = SX(wx), sz = SZ(wz);
    ctx.strokeStyle = tool === 'tree' ? '#ff7a7a' : '#6fe07a'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(sx, sz - 7); ctx.lineTo(sx + 5, sz + 4); ctx.lineTo(sx - 5, sz + 4); ctx.closePath(); ctx.stroke();
    if (view.scale >= 0.002) {
      ctx.fillStyle = '#6fe07a';
      ctx.fillText((s.id || 'tree') + ' (' + (s.species || '?') + (Number.isInteger(s.variant) ? ' v' + s.variant : '') + ')', sx + 8, sz + 4);
    }
  }
  if (selected && selected.kind !== 'clearing' && selected.kind !== 'soften' && selected.kind !== 'pad') {
    const at = siteXZ(selected);
    ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.arc(SX(at[0]), SZ(at[1]), 10, 0, Math.PI * 2); ctx.stroke();
  }
}
/** A brush outline at the cursor: the radius circle, and a dashed half-radius
 *  ring for the falloff brushes. */
function brushCursor(ctx, wx, wz, r, colr, inner) {
  const sx = SX(wx), sz = SZ(wz), rp = Math.max(2, r * view.scale);
  ctx.strokeStyle = colr; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(sx, sz, rp, 0, Math.PI * 2); ctx.stroke();
  if (inner) { ctx.setLineDash([2, 3]); ctx.beginPath(); ctx.arc(sx, sz, Math.max(1, rp * 0.5), 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]); }
}
/** The active tool's cursor at the hover column (the registry's `cursor`). */
function paintCursor(ctx) {
  if (!els.hoverW) return;
  const [wx, wz] = els.hoverW;
  REG.call(tool, 'cursor', ctx, wx, wz);
}
function fit() {
  if (!map || !els.canvas) return;
  const e = map.json.extent;
  view.cx = e.min[0] + e.size[0] / 2; view.cz = e.min[1] + e.size[1] / 2;
  view.scale = 0.95 * Math.min((els.canvas.clientWidth || 600) / e.size[0], (els.canvas.clientHeight || 400) / e.size[1]);
}
function status(t) { if (els.status) els.status.textContent = t; }
/** The hover readout: world x / z in metres, the authored height in metres
 *  (and above the sea), the biome, the tile. */
function readout(wx, wz) {
  const v = vpm(), P = map.H;
  const inside = MT.inExtent(P, wx, wz);
  let s = `x ${fmtM(wx / v)}  z ${fmtM(wz / v)}`;
  if (!inside) return s + '  — outside the extent (the height eases to the default over the edge fade)';
  const h = MT.heightAt(P, wx, wz), b = MT.biomeAt(map.B, wx, wz);
  const ti = Math.floor((wx - P.min[0]) / P.sp + 0.5) >> 8, tj = Math.floor((wz - P.min[1]) / P.sp + 0.5) >> 8;
  s += `  ·  height ${fmtM(h / v)} (y ${Math.round(h)}, ${h >= seaY() ? '+' : ''}${fmtM((h - seaY()) / v)} above sea)` +
       `  ·  biome ${map.json.biomes[b] || '?'}  ·  tile ${ti},${tj}` +
       (MT.blocked(P, MT.tileKey(ti, tj)) ? ' (loading)' : '');
  const g = groundAt(wx, wz);
  if (g != null) s += `  ·  real ground ${fmtM(g / v)}`;
  return s;
}

/* ---- input --------------------------------------------------------------------- */
/** The world column under the cursor, fractional. */
function worldAtF(ev) {
  // the CONTENT box: the bounding rect includes the canvas's CSS border
  const c = els.canvas, r = c.getBoundingClientRect();
  return [(ev.clientX - r.left - c.clientLeft - c.clientWidth / 2) / view.scale + view.cx,
          (ev.clientY - r.top - c.clientTop - c.clientHeight / 2) / view.scale + view.cz];
}
/** The world column under the cursor, whole voxels. */
function worldColumnAt(ev) { const [x, z] = worldAtF(ev); return [Math.round(x), Math.round(z)]; }
const brushVox = () => brush.radiusM * vpm();
/** The brush mask in the plane's units (maptiles.maskWeights): heights in
 *  absolute voxels, slopes in degrees; null = no mask. */
function brushMask() {
  const m = brush.mask, v = vpm();
  if (!m || m.kind === 'none') return null;
  if (m.kind === 'slope') return {kind: 'slope', lo: m.lo, hi: m.hi, soft: Math.max(0.5, m.soft)};
  return {kind: m.kind, seaY: seaY(), lo: seaY() + m.lo * v, hi: seaY() + m.hi * v, soft: Math.max(1, m.soft * v)};
}
/** One dab of the active plane brush at world (wx, wz); returns true if it changed anything. */
function planeDab(wx, wz) {
  let r;
  if (tool === 'height') {
    const v = vpm(), k = brush.strength / 100;
    const o = {mode: brush.mode, radius: brushVox(), falloff: hb.falloff, amount: k * brushVox() / 10, k,
               target: brush.mode === 'flatten' ? stroke.flatT : seaY() + brush.targetM * v,
               step: hb.stepM * v, sharp: hb.sharp / 100, datum: seaY(),
               amp: hb.ampM * v, size: hb.sizeM * v, seed: hb.seed, style: hb.style,
               erosion: {iterations: 2, strength: 0.8, seed: hb.seed, talusDeg: erosion.talusDeg}, stroke: stroke.ctx,
               mask: brushMask()};
    if (!stroke.tiles.height) stroke.tiles.height = new Map();
    r = MT.heightDab(map.H, wx, wz, o, stroke.tiles.height);
    if (r.tooBig) status('the erode BRUSH works up to about ' + fmtM(Math.sqrt(MT.ERODE_BRUSH_MAX_POSTS) * map.H.sp / 2 / v) +
                         ' of radius; for a larger patch use the Area tool and Erode in the Height tools panel');
  } else {
    if (!stroke.tiles.biome) stroke.tiles.biome = new Map();
    r = MT.biomeDab(map.B, wx, wz, brushVox(), brush.index, stroke.tiles.biome);
    if (CLM) CLM.biomePainted(wx, wz, brushVox(), stroke);   // E2: lock what the brush paints
  }
  stats.skipped += r.skipped;
  if (r.skipped && !r.changed) status('those tiles are still loading from disk — the brush skipped them; stroke again in a moment');
  return r.changed > 0;
}
/** Swap a plane-stroke undo entry in; returns the redo entry. */
function swapPlanes(e) {
  const back = {tiles: {}};
  if (e.tiles.height) back.tiles.height = MT.swapTiles(map.H, e.tiles.height);
  if (e.tiles.biome) back.tiles.biome = MT.swapTiles(map.B, e.tiles.biome);
  range.vals = null;
  return back;
}
function siteCentre(s) {
  if ((s.kind === 'pad' || s.kind === 'clearing' || s.kind === 'soften') && Array.isArray(s.min) && Array.isArray(s.max)) return boxCentre(s);
  return siteXZ(s);
}
/* ---- THE TOOLS (maptools.js registry): every tool is declared in ONE place,
 * registerTool({key, label, group, opts, down, move, up, cursor, key, ...});
 * the toolbar, the options strip, the pointer / key / double-click routing,
 * the cursor and the tool guide all come from the registry. A layer
 * (mapvector, maplocale, maprange, mapgen, mapoverlay) registers its own
 * through the context it is handed (C.registerTool). */
/** The plane brushes' drag: dabs every `spacing` % of the radius. */
function planeMove(fx, fz) {
  if (!stroke || !stroke.tiles) return;
  const P = tool === 'height' ? map.H : map.B;
  const gap = Math.max(P.sp * 0.5, Math.max(brushVox(), tool === 'height' ? P.sp : 0) * brush.spacing / 100);
  let [lx, lz] = lastDab;
  let d = Math.hypot(fx - lx, fz - lz);
  while (d >= gap) {
    lx += (fx - lx) * gap / d; lz += (fz - lz) * gap / d;
    if (planeDab(lx, lz)) stroke.changed = true;
    d = Math.hypot(fx - lx, fz - lz);
  }
  lastDab = [lx, lz];
}
function planeDown(ev, wx, wz) {
  if (ev.altKey) {
    // pick: the height (into the set target) or the biome under the cursor
    if (tool === 'height') { brush.targetM = Math.round((MT.heightAt(map.H, wx, wz) - seaY()) / vpm() * 10) / 10; brush.mode = 'set'; }
    else brush.index = MT.biomeAt(map.B, wx, wz);
    syncControls(); rebuildPalette(); paint();
    return true;
  }
  stroke = {tiles: {}, changed: false, ctx: {}, flatT: MT.heightAt(map.H, wx, wz)};
  lastDab = [wx, wz];
  if (planeDab(wx, wz)) stroke.changed = true;
  paint();
  return true;
}
const sitesOf = () => map.json.sites || (map.json.sites = []);
function registerBuiltinTools() {
  const R = REG.registerTool;
  R({key: 'site', label: 'Select', group: 'select', order: 0, locale: true,
     title: 'click a site marker of any kind to select it in the Sites panel',
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       selected = siteAt(wx, wz);
       syncSites(); paint();
       return true;
     }});
  R({key: 'height', label: 'Height', group: 'height', order: 10,
     title: 'paint the AUTHORED HEIGHT plane in metres: raise / lower / smooth / flatten / set / terrace / noise / erode (left-drag; Alt+click picks the height under the cursor into Set); the mask limits it to land, sea, a height band or a slope band',
     opts: () => els.optHeight,
     down: planeDown, move: (ev, fx, fz) => planeMove(fx, fz),
     cursor: (ctx, wx, wz) => brushCursor(ctx, wx, wz, Math.max(brushVox(), map.H.sp),
       brush.mode === 'lower' ? 'rgba(120,180,255,0.9)' : brush.mode === 'raise' ? 'rgba(255,170,90,0.9)' : 'rgba(255,255,255,0.8)', true),
     resize: f => { brush.radiusM = Math.max(R_MIN_M, Math.min(R_MAX_M, brush.radiusM * f)); }});
  R({key: 'shape', label: 'Shape', group: 'height', order: 20,
     title: 'click: stamp a landform into the height plane -- peak, ridge, basin, plateau, volcano, mesa ([ ] radius, , . rotate)',
     opts: () => els.optShape,
     down(ev, wx, wz) { stampShape(wx, wz); paint(); return true; },
     cursor: paintShapeCursor,
     resize: f => { shape.radiusM = Math.max(10, Math.min(R_MAX_M, Math.round(shape.radiusM * f))); },
     onKey(ev) {
       if (ev.key !== ',' && ev.key !== '.') return false;
       shape.rot = ((shape.rot + (ev.key === '.' ? 15 : -15)) % 360 + 360) % 360;
       syncControls(); paint();
       return true;
     }});
  R({key: 'line', label: 'Line', group: 'height', order: 30,
     title: 'RIDGE / VALLEY lines: click to draw (double-click / Enter ends), drag a vertex, click a segment to insert, shift+click a vertex to delete; Bake in the Height tools panel',
     opts: () => els.optLine,
     down(ev) { linePointerDown(ev); paint(); return true; },
     move(ev) {
       if (!lines.drag) return;
       const [wx, wz] = worldColumnAt(ev);
       const p = lines.drag.line.points[lines.drag.k];
       p[0] = wx; p[1] = wz;
       lines.drag.moved = true;
       lineEdited(lines.drag.line);
       syncLines();
     },
     up() {
       if (!lines.drag) return;
       if (lines.drag.moved && lines.drag.snap) pushUndo(lines.drag.snap);
       if (lines.drag.moved) markDirty(true);
       lines.drag = null;
       syncLines();
     },
     resize: f => { lines.widthM = Math.max(5, Math.min(R_MAX_M, Math.round(lines.widthM * f))); },
     onKey(ev) {
       if ((ev.key !== 'Enter' && ev.key !== 'Escape') || !lines.drawing) return false;
       lines.drawing = false;
       syncLines(); paint();
       return true;
     },
     dblclick() {
       // a double click ends the line being drawn (its second click added a
       // vertex on top of the first: drop it)
       if (!lines.drawing || !lines.sel) return false;
       const pts = lines.sel.points;
       if (pts.length >= 2) {
         const a = pts[pts.length - 1], b = pts[pts.length - 2];
         if (Math.hypot((a[0] - b[0]) * view.scale, (a[1] - b[1]) * view.scale) < 4) pts.pop();
       }
       lines.drawing = false;
       syncLines(); paint();
       return true;
     }});
  R({key: 'area', label: 'Area', group: 'height', order: 40,
     title: 'drag: the AREA the Height tools / Generate panels erode, generate into and export / import as PNG; shift+click clears it',
     opts: () => els.optArea,
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       if (ev.shiftKey) { area = null; syncHeightTools(); paint(); return true; }
       areaDrag = {x0: wx, z0: wz};
       area = [wx, wz, wx, wz];
       return true;
     },
     move(ev) {
       if (!areaDrag) return;
       const [wx, wz] = worldColumnAt(ev);
       area = [Math.min(areaDrag.x0, wx), Math.min(areaDrag.z0, wz), Math.max(areaDrag.x0, wx), Math.max(areaDrag.z0, wz)];
     },
     up() {
       if (!areaDrag) return;
       areaDrag = null;
       // a click is not an area
       if (area && (area[2] - area[0] < map.H.sp || area[3] - area[1] < map.H.sp)) area = null;
       syncHeightTools();
     }});
  R({key: 'biome', label: 'Biome brush', group: 'paint', order: 10,
     title: 'paint the selected biome (left-drag; Alt+click picks the biome under the cursor)',
     opts: () => els.optBiome,
     down: planeDown, move: (ev, fx, fz) => planeMove(fx, fz),
     cursor(ctx, wx, wz) {
       brushCursor(ctx, wx, wz, brushVox(), 'rgba(255,255,255,0.8)', false);
       // the biome brush paints whole cells: outline the one under the cursor
       const P = map.B, i = Math.floor((wx - P.min[0]) / P.sp), j = Math.floor((wz - P.min[1]) / P.sp);
       ctx.strokeRect(SX(P.min[0] + i * P.sp), SZ(P.min[1] + j * P.sp), P.sp * view.scale, P.sp * view.scale);
     },
     resize: f => { brush.radiusM = Math.max(R_MIN_M, Math.min(R_MAX_M, brush.radiusM * f)); }});
  R({key: 'sculpt', label: 'Sculpt', group: 'paint', order: 20, locale: true,
     title: 'SCULPT the fine layer: raise / lower / smooth / flatten / erase the map’s height-offset layer (sculpt.svsculpt, 0.8 m samples), added after everything else',
     opts: () => els.optSculpt,
     down(ev, wx, wz) {
       // THE SCULPT BRUSH (P5): the fine height-offset layer, not the plane.
       if (sculpt.mode === 'flatten' && groundAt(wx, wz) == null) {
         toast('Flatten levels to the ground under the cursor, so it needs the engine’s heights here: fetching them for this view — stroke again when they are drawn');
         fetchHeights().catch(e => toast('heights: ' + e.message, true));
         return true;
       }
       stroke = {sculptBefore: new Map(), changed: false};
       sculpt.target = sculpt.mode === 'flatten' ? groundAt(wx, wz) : null;
       sculpt.last = [wx, wz];
       if (sculptDab(wx, wz, stroke.sculptBefore)) { stroke.changed = true; scheduleSculptPreview(); }
       paint();
       return true;
     },
     move(ev, fx, fz) {
       if (!stroke || !stroke.sculptBefore) return;
       const gap = Math.max(2, sculpt.radius * 0.25);
       let [lx, lz] = sculpt.last;
       let d = Math.hypot(fx - lx, fz - lz);
       while (d >= gap) {
         lx += (fx - lx) * gap / d; lz += (fz - lz) * gap / d;
         if (sculptDab(lx, lz, stroke.sculptBefore)) stroke.changed = true;
         d = Math.hypot(fx - lx, fz - lz);
       }
       sculpt.last = [lx, lz];
       scheduleSculptPreview();
     },
     statusAt(fx, fz) {
       const g = groundAt(fx, fz);
       return `x ${fmtM(fx / vpm())}  z ${fmtM(fz / vpm())}  sculpt ${scAt(sculpt.tiles, fx, fz) >= 0 ? '+' : ''}${scAt(sculpt.tiles, fx, fz).toFixed(1)} vox` +
              (g == null ? '  (no heights here: click Real terrain for the engine’s ground)' : `  ground ~y${Math.round(g)}`) +
              `  layer: ${sculpt.tiles.size} tiles${sculpt.dirty ? ' (unsaved)' : ''}`;
     },
     cursor: (ctx, wx, wz) => brushCursor(ctx, wx, wz, sculpt.radius,
       sculpt.mode === 'lower' ? 'rgba(120,180,255,0.9)' : sculpt.mode === 'raise' ? 'rgba(255,170,90,0.9)' : 'rgba(255,255,255,0.8)', true),
     resize: f => { sculpt.radius = Math.max(8, Math.min(4096, Math.round(sculpt.radius * f))); }});
  // the vector layers (mapvector.js) and the locales (maplocale.js) take their
  // pointer, key and double-click events before the registry; they register
  // here for the toolbar, the options strip and the guide
  const vo = VL.toolOpts(), lo = LC.toolOpts();
  R({key: 'road', label: 'Road', group: 'vector', order: 10, opts: () => vo.road,
     title: 'ROADS (map.json roads[]: graded ground the engine builds): click to draw (double-click / Enter ends), drag a vertex, click the selected road to insert one, shift+click a vertex to delete; vertices SNAP onto other roads (junctions); Ctrl+click on a road starts a branch there; Alt = no snap'});
  R({key: 'region', label: 'Region', group: 'vector', order: 20, opts: () => vo.region,
     title: 'REGIONS (map.json regions[]: named polygons, data only): click out a polygon, click its first vertex to close; drag / insert / shift+click delete vertices like roads'});
  R({key: 'marker', label: 'Marker', group: 'vector', order: 30, opts: () => vo.marker,
     title: 'MARKERS (map.json markers[]: annotations with a pixel-art icon): click to place, drag to move, shift+click to delete'});
  R({key: 'locale', label: 'Locale', group: 'locale', order: 10, opts: () => lo.locale,
     title: 'LOCALES (map.json locales[]: hand-made places that ride their ground): drag out a new one’s bounds with the icon picked beside it; click one to select it; DOUBLE-CLICK opens it (the locale page)'});
  R({key: 'structure', label: 'Structure', group: 'locale', order: 20, world: false, locale: true, opts: () => lo.structure,
     title: 'STRUCTURES of this locale (structure refs, written through the F8 command layer): click places the picked blueprint, drag one to move it, R turns it, shift+click deletes it'});
  R({key: 'waylink', label: 'Link', group: 'locale', order: 30, world: false, locale: true, opts: () => lo.waylink,
     title: 'WAYNODE LINKS: click a waynode, then another, to join them (Shift or a second time parts them); click open ground for a new waynode joined to the last'});
  R({key: 'pad', label: 'Pad box', group: 'site', order: 10, title: 'drag the calm pad box (world voxels)',
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       stroke = snapshot();
       padDrag = {x0: wx, z0: wz};
       return true;
     },
     move(ev) {
       if (!padDrag) return;
       const [wx, wz] = worldColumnAt(ev);
       const s = (map.json.sites || []).find(s => s.kind === 'pad');
       const box = {min: [Math.min(padDrag.x0, wx), Math.min(padDrag.z0, wz)],
                    max: [Math.max(padDrag.x0, wx) - 1, Math.max(padDrag.z0, wz) - 1]};
       if (s) { s.min = box.min; s.max = box.max; }
       else map.json.sites = [...(map.json.sites || []), {id: 'harness', kind: 'pad', ...box}];
       stroke.changed = true;
     },
     up() { padDrag = null; }});
  R({key: 'spawn', label: 'Spawn', group: 'site', order: 20,
     title: 'click: put the spawn site there (where the game starts; the calm home area centres on it). One per map.',
     down(ev) {
       // ONE per map (the loader refuses two), so this moves it
       const [wx, wz] = worldColumnAt(ev);
       const sites = sitesOf();
       stroke = snapshot();
       const s = sites.find(s => s.kind === 'spawn');
       if (s) s.at = [wx, wz];
       else sites.push({id: 'spawn', kind: 'spawn', at: [wx, wz]});
       stroke.changed = true;
       publishTerrain();
       paint();
       return true;
     }});
  R({key: 'stamp', label: 'Stamp site', group: 'site', order: 30, opts: () => els.optStamp,
     title: 'click: place a stamp site (the template picked beside it, from assets/prefabs/); shift+click a marker: delete it',
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       const sites = sitesOf();
       const grab = Math.max(6 / view.scale, 1);
       const hit = sites.find(s => s.kind === 'stamp' && Math.abs(siteXZ(s)[0] - wx) <= Math.max(s.radius || 16, grab) &&
                                   Math.abs(siteXZ(s)[1] - wz) <= Math.max(s.radius || 16, grab));
       stroke = snapshot();
       if (ev.shiftKey && hit) {
         sites.splice(sites.indexOf(hit), 1);
         stroke.changed = true;
       } else if (!hit) {
         if (!stamp.template) {
           toast(stamp.templates.length ? 'pick a stamp template first' : 'no .vox under assets/prefabs/ to stamp', true);
           stroke = null; return true;
         }
         const t = stamp.template, rot = stamp.rot & 3;
         sites.push({id: t + '_' + sites.length, kind: 'stamp', template: t, at: [wx, wz], rot, padMargin: 8, salt: sites.length});
         stroke.changed = true;
       }
       paint();
       return true;
     }});
  R({key: 'water', label: 'Water', group: 'site', order: 40, locale: true, opts: () => els.optWater,
     title: 'click: place an AUTHORED LAKE (a water preset at that column, same on every seed); drag a lake to move it; shift+click: delete it',
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       const sites = sitesOf();
       const hit = sites.find(s => {
         if (s.kind !== 'water' || !Array.isArray(s.at)) return false;
         const g = water.geom[s.preset] || {radius: 60};
         if (!LC.circleAllowed(s.at[0], s.at[1], (s.radius || g.radius) + (g.band || 0))) return false;
         const r = Math.max(s.radius || g.radius, 6 / view.scale);
         const dx = s.at[0] - wx, dz = s.at[1] - wz;
         return dx * dx + dz * dz <= r * r;
       });
       if (!hit && !ev.shiftKey && LC.clipping()) {
         const g = water.geom[water.preset] || {radius: 60, band: 24};
         const rv = water.radiusM > 0 ? Math.round(water.radiusM * 10) : g.radius;
         if (!LC.circleAllowed(wx, wz, rv + g.band)) { status('the lake and its shore would reach past the locale\'s bounds: place it further in, or make it smaller'); return true; }
       }
       stroke = snapshot();
       if (ev.shiftKey && hit) {
         sites.splice(sites.indexOf(hit), 1);
         stroke.changed = true;
       } else if (hit) {
         waterDrag = {site: hit, dx: hit.at[0] - wx, dz: hit.at[1] - wz};
       } else {
         if (!water.preset) { toast('pick a water preset first', true); stroke = null; return true; }
         const n = sites.filter(s => s.kind === 'water').length;
         const s = {id: water.preset + '_' + n, kind: 'water', preset: water.preset, at: [wx, wz]};
         if (water.radiusM > 0) s.radius = Math.round(water.radiusM * 10);
         sites.push(s);
         stroke.changed = true;
       }
       paint();
       return true;
     },
     move: siteDragMove, up: () => { waterDrag = null; }});
  R({key: 'tree', label: 'Tree', group: 'site', order: 50, locale: true, opts: () => els.optTree,
     title: 'click: place an AUTHORED TREE on the ground at that column; drag a tree to move it; shift+click: delete it',
     down(ev) {
       const [wx, wz] = worldColumnAt(ev);
       const sites = sitesOf();
       const grab = Math.max(6, 8 / view.scale);   // ~8 px of marker
       const hit = sites.find(s => s.kind === 'tree' && Array.isArray(s.at) && LC.pointAllowed(s.at[0], s.at[1]) &&
                                   Math.hypot(s.at[0] - wx, s.at[1] - wz) <= grab);
       if (!hit && !LC.pointAllowed(wx, wz)) { status('outside the locale: a tree goes inside its bounds'); return true; }
       stroke = snapshot();
       if (ev.shiftKey && hit) {
         sites.splice(sites.indexOf(hit), 1);
         if (selected === hit) selected = null;
         stroke.changed = true;
       } else if (hit) {
         selected = hit;
         waterDrag = {site: hit, dx: hit.at[0] - wx, dz: hit.at[1] - wz};
       } else {
         if (!trees.species) { toast(trees.list.length ? 'pick a tree species first' : 'no .svtree under assets/trees/', true); stroke = null; return true; }
         const n = sites.filter(s => s.kind === 'tree').length;
         const s = {id: trees.species + '_' + n, kind: 'tree', species: trees.species, at: [wx, wz]};
         if (trees.variant >= 0) s.variant = trees.variant;
         if (trees.rot >= 0) s.rot = trees.rot;
         sites.push(s);
         selected = s;
         stroke.changed = true;
       }
       syncSites();
       paint();
       return true;
     },
     move: siteDragMove, up: () => { waterDrag = null; }});
  for (const kind of ['clearing', 'soften']) {
    R({key: kind, label: kind === 'clearing' ? 'Clearing' : 'Soften', group: 'site', order: kind === 'clearing' ? 60 : 70, locale: true,
       opts: () => (kind === 'clearing' ? els.optClearing : null),
       title: kind === 'clearing'
         ? 'drag: draw a FOREST CLEARING (a box no tree crown reaches over); click one to select it, drag to move it; shift+click inside one: delete it'
         : 'drag: draw SOFTENED GROUND (a box where the seeded hills, bumps and grain are kept at a percent -- a village’s ground); shift+click inside one: delete it',
       down(ev) { boxSiteDown(ev, kind); return true; },
       move: boxSiteMove, up: boxSiteUp});
  }
}
/** Drag of a placed site (a lake, a tree): its `at` follows, clipped to an open locale. */
function siteDragMove(ev) {
  if (!waterDrag) return;
  const [wx, wz] = worldColumnAt(ev);
  const at = [wx + waterDrag.dx, wz + waterDrag.dz], s = waterDrag.site;
  const g = water.geom[s.preset] || {radius: 60, band: 24};
  const ok = s.kind === 'water' ? LC.circleAllowed(at[0], at[1], (s.radius || g.radius) + g.band) : LC.pointAllowed(at[0], at[1]);
  if (ok) { s.at = at; stroke.changed = true; }
}
/** Clearing / Soften: drag on open map draws a new box; click inside one
 *  selects it and a drag slides it; shift+click inside one deletes it. */
function boxSiteDown(ev, kind) {
  const [wx, wz] = worldColumnAt(ev);
  const sites = sitesOf();
  // a locale open: only boxes wholly inside it are its to edit
  let hit = clearingAt(wx, wz, kind);
  if (hit && !LC.boxEditable(hit)) hit = null;
  if (!hit && !LC.pointAllowed(wx, wz)) { status('outside the locale: its tools edit inside its bounds only'); return; }
  stroke = snapshot();
  if (ev.shiftKey && hit) {
    sites.splice(sites.indexOf(hit), 1);
    if (selected === hit) selected = null;
    stroke.changed = true;
  } else if (hit) {
    selected = hit;
    clearDrag = {mode: 'move', site: hit, dx: hit.min[0] - wx, dz: hit.min[1] - wz,
                 w: hit.max[0] - hit.min[0], h: hit.max[1] - hit.min[1]};
  } else {
    const n = sites.filter(s => s.kind === kind).length;
    let id = kind + '_' + n;
    for (let k = n; sites.some(s => s.id === id); k++) id = kind + '_' + (k + 1);
    const s = kind === 'soften'
      ? {id, kind, min: [wx, wz], max: [wx, wz], feather: Math.round(soften.feather), hills: soften.hills,
         bumps: soften.bumps, grain: soften.grain, paths: soften.paths}
      : {id, kind: 'clearing', min: [wx, wz], max: [wx, wz], feather: Math.round(clearing.feather)};
    sites.push(s);
    selected = s;
    clearDrag = {mode: 'new', site: s, x0: wx, z0: wz};
  }
  syncSites();
  paint();
}
function boxSiteMove(ev) {
  if (!clearDrag) return;
  const [wx, wz] = worldColumnAt(ev);
  const s = clearDrag.site;
  if (clearDrag.mode === 'new') {
    s.min = [Math.min(clearDrag.x0, wx), Math.min(clearDrag.z0, wz)];
    s.max = [Math.max(clearDrag.x0, wx), Math.max(clearDrag.z0, wz)];
  } else {
    s.min = [wx + clearDrag.dx, wz + clearDrag.dz];
    s.max = [s.min[0] + clearDrag.w, s.min[1] + clearDrag.h];
  }
  // a locale open: the box (and its feather) stays inside the bounds
  LC.clipBox(s, s.kind === 'soften' ? 96 : 64);
  stroke.changed = true;
  syncSites();
}
function boxSiteUp() {
  if (clearDrag && clearDrag.mode === 'new') {
    // A click without a drag is not a box: take the empty one back.
    const s = clearDrag.site;
    if (s.max[0] - s.min[0] < 8 || s.max[1] - s.min[1] < 8) {
      const sites = map.json.sites || [];
      if (sites.includes(s)) sites.splice(sites.indexOf(s), 1);
      if (selected === s) selected = null;
      if (stroke) stroke.changed = false;
      toast('drag to draw a ' + (s.kind === 'soften' ? 'softened box' : 'clearing') + ' (at least 8 voxels a side; zoom in for voxel precision)');
      syncSites();
    } else if (stroke) stroke.changed = true;
  }
  clearDrag = null;
}
function wire() {
  const c = els.canvas;
  let panning = null;
  c.addEventListener('contextmenu', e => e.preventDefault());
  c.addEventListener('pointerdown', ev => {
    if (!map) return;
    c.setPointerCapture(ev.pointerId);
    if (ev.button === 1 || ev.button === 2) { panning = {x: ev.clientX, y: ev.clientY, cx: view.cx, cz: view.cz}; return; }
    // an erosion / generate preview is committed by the next edit of any kind
    if (erosion.pending) commitErosion();
    // a press any tool yields first (the minimap)
    if (REG.each('early', [ev], true)) return;
    if (VL.pointerDown(ev)) return;
    if (LC.pointerDown(ev)) return;
    const [wx, wz] = worldAtF(ev);
    REG.call(tool, 'down', ev, wx, wz);
  });
  c.addEventListener('pointermove', ev => {
    if (!map) return;
    els.hoverW = worldAtF(ev);
    const [fx, fz] = els.hoverW;
    if (panning) {
      view.cx = panning.cx - (ev.clientX - panning.x) / view.scale;
      view.cz = panning.cz - (ev.clientY - panning.y) / view.scale;
      scheduleRealRefresh();
    } else if (VL.dragging() && ev.buttons & 1) {
      VL.pointerMove(ev);
    } else if (LC.dragging() && ev.buttons & 1) {
      LC.pointerMove(ev);
    } else if (ev.buttons & 1) {
      REG.call(tool, 'move', ev, fx, fz);
    } else REG.call(tool, 'hover', ev, fx, fz);
    const st = REG.call(tool, 'statusAt', fx, fz);
    status(st != null ? st : readout(fx, fz));
    schedulePaint();
  });
  const up = () => {
    panning = null;
    VL.pointerUp();
    LC.pointerUp();
    // every tool's release: each ends only the drag it started
    REG.each('up');
    if (stroke) {
      const st = stroke;
      stroke = null;
      if (st.changed) {
        if (st.sculptBefore) { sculpt.dirty = true; pushUndo({sculptBefore: st.sculptBefore}); }
        else if (st.tiles) { pushUndo(st.custom ? {tiles: st.tiles, custom: st.custom} : {tiles: st.tiles}); range.vals = null; }
        else pushUndo(st);
        markDirty(true);
      }
    }
    paint();
  };
  c.addEventListener('pointerup', up);
  c.addEventListener('pointercancel', up);
  c.addEventListener('pointerleave', () => { els.hoverW = null; paint(); });
  c.addEventListener('wheel', ev => {
    if (!map) return;
    ev.preventDefault();
    const [wx, wz] = worldAtF(ev);
    const f = ev.deltaY < 0 ? 1.25 : 0.8;
    const e = map.json.extent;
    const lo = 0.2 * Math.min((c.clientWidth || 600) / e.size[0], (c.clientHeight || 400) / e.size[1]);
    const ns = Math.max(lo, Math.min(16, view.scale * f));
    // zoom about the cursor: keep the world column under it fixed
    view.cx = wx + (view.cx - wx) * (view.scale / ns);
    view.cz = wz + (view.cz - wz) * (view.scale / ns);
    view.scale = ns;
    scheduleRealRefresh();
    paint();
  }, {passive: false});
  window.addEventListener('keydown', ev => {
    if (!H.isVisible || !H.isVisible() || !map) return;
    const inField = /input|select|textarea/i.test(document.activeElement && document.activeElement.tagName);
    if (inField) return;
    if ((ev.ctrlKey || ev.metaKey) && ev.key === 'z') { ev.preventDefault(); doUndo(); }
    else if ((ev.ctrlKey || ev.metaKey) && ev.key === 'y') { ev.preventDefault(); doRedo(); }
    else if (LC.key(ev)) ev.preventDefault();
    else if ((tool === 'road' || tool === 'region' || tool === 'marker') && VL.key(ev)) ev.preventDefault();
    else if (REG.call(tool, 'onKey', ev)) ev.preventDefault();
    else if (REG.each('anyKey', [ev], true)) ev.preventDefault();
    else if (ev.key === '[' || ev.key === ']') {
      const f = ev.key === ']' ? 1.25 : 0.8;
      if (REG.get(tool) && REG.get(tool).resize) REG.call(tool, 'resize', f);
      else brush.radiusM = Math.max(R_MIN_M, Math.min(R_MAX_M, brush.radiusM * f));
      syncControls(); paint();
    }
  });
  c.addEventListener('dblclick', ev => {
    if ((tool === 'road' || tool === 'region') && VL.dblclick()) return;
    // a locale's marker (any tool) or its bounds (Locale / Select): open it
    if (LC.dblclick(ev)) return;
    REG.call(tool, 'dblclick', ev);
  });
}
// An undo entry is one of: {tiles: {height?, biome?}} (plane tiles before the
// stroke), {sculptBefore} (sculpt tiles), or {json} (a map.json snapshot: sites,
// terrain, root numbers). Swapping one in returns the opposite stack's entry.
// A height-tool entry may carry several: a line bake is {tiles, json, bake}
// (the plane, the line's `baked` flag, and the session's bake record).
function swapEntry(e) {
  if (e.sculptBefore) return {sculptBefore: sculptSwap(e.sculptBefore)};
  // E4: a refs batch (written by the engine at once): run its inverse
  // commands; the opposite stack gets the forward ones
  if (e.refs) { LC.replay(e.refs.undo, e.refs.label); return {refs: {undo: e.refs.redo, redo: e.refs.undo, label: e.refs.label}}; }
  const back = {};
  if (e.tiles) Object.assign(back, swapPlanes(e));
  if (e.json) { back.json = snapshot().json; restore(e); }
  // a layer's own state (a range's bake record, ...): `custom.swap()` puts it
  // in and returns the entry that puts it back
  if (e.custom) back.custom = e.custom.swap();
  if (e.bake) {
    back.bake = {id: e.bake.id, rec: bakes.get(e.bake.id) || null};
    if (e.bake.rec) bakes.set(e.bake.id, e.bake.rec); else bakes.delete(e.bake.id);
    syncLines();
  }
  return back;
}
function doUndo() {
  if (!undo.length) return;
  const e = undo.pop();
  redo.push(swapEntry(e));
  if (!e.refs) markDirty(true);   // refs are on disk already: the map is no dirtier
  paint();
}
function doRedo() {
  if (!redo.length) return;
  const e = redo.pop();
  undo.push(swapEntry(e));
  if (!e.refs) markDirty(true);
  paint();
}
/** The nearest site marker of any kind within a generous radius, or null. */
function siteAt(wx, wz) {
  const reach = 14 / view.scale;
  let best = null, bd = Infinity;
  for (const s of (map.json.sites || [])) {
    let at = Array.isArray(s.at) ? s.at : null;
    if (s.kind === 'stamp') at = siteXZ(s);
    if (s.kind === 'pad' && Array.isArray(s.min) && Array.isArray(s.max)) at = boxCentre(s);
    if ((s.kind === 'clearing' || s.kind === 'soften') && Array.isArray(s.min) && Array.isArray(s.max)) {
      const c = boxCentre(s);
      if (wx >= s.min[0] && wx <= s.max[0] && wz >= s.min[1] && wz <= s.max[1]) {
        const d = Math.hypot(c[0] - wx, c[1] - wz);
        if (d < bd) { bd = d; best = s; }
        continue;
      }
      at = c;
    }
    if (!at) continue;
    const d = Math.hypot(at[0] - wx, at[1] - wz);
    const r = s.kind === 'water' ? ((s.radius || (water.geom[s.preset] || {}).radius || 60)) : reach;
    if (d <= Math.max(r, reach) && d < bd) { bd = d; best = s; }
  }
  return best;
}

/* ---- the Terrain section (P-G) --------------------------------------------------------- */
function buildTerrainSection() {
  const el = H.el;
  const det = el('details', {class: 'mapterrain'});
  det.append(el('summary', {}, 'Terrain — the sea, the edge and the seeded noise on top of the authored height (map.json)'));
  const grid = el('div', {class: 'mapterrain-grid'});
  els.terrainInputs = {};
  for (const [path, label, min, max, step, unit, desc] of TERRAIN_ROWS) {
    const inp = el('input', {type: 'number', min, max, step, style: 'width:80px'});
    inp.addEventListener('change', () => {
      if (!map) return;
      const v = Math.max(min, Math.min(max, Math.round(+inp.value || 0)));
      if (v === rowGet(path)) { inp.value = v; return; }
      pushUndo(snapshot());
      rowSet(path, v);
      inp.value = v;
      if (path === '@seaLevelY') range.vals = null;
      markDirty(true); publishTerrain(); syncTerrain(); paint();
    });
    els.terrainInputs[path] = inp;
    grid.append(el('label', {title: desc}, label, ' ', inp, el('span', {class: 'mapunit'}, unit)));
  }
  const ref = el('span', {class: 'mapnote'});
  els.terrainRef = ref;
  det.append(grid, ref,
    el('div', {class: 'mapnote'}, 'Lengths are voxels at refVoxelsPerMetre (10 = decimetres). Saving writes map.json; F7 in the game reloads it. ' +
       'Per-biome relief (the curve and the hill / detail / grain multipliers) is on each biome page. Every one of these moves the world hash.'));
  return det;
}
function syncTerrain() {
  if (!map || !els.terrainInputs) return;
  for (const [path] of TERRAIN_ROWS) { const inp = els.terrainInputs[path]; if (inp) inp.value = rowGet(path) ?? ''; }
  const t = terrainOf(), v = vpm();
  if (els.terrainRef) els.terrainRef.textContent = 'refVoxelsPerMetre ' + v + ' · sea level y' + seaY() +
      ' · THE SCALES: the authored height plane is exact at its posts (every ' + (map.H.sp / v).toFixed(1) + ' m) and never scaled; ' +
      'the seeded range octave adds ±' + Math.round((t.rangeAmplitude || 0) / 2 / v) + ' m over ' + ((1 << (t.rangeLog2 || 9)) / v).toFixed(0) +
      ' m on top of it, the hills ±' + Math.round((t.hillAmplitude || 0) / 2 / v) + ' m; the Sculpt tool’s offset (every 0.8 m) is added after all of it, unscaled.';
}

/* ---- the SITES panel (P-I): every site by kind, select / rename / delete ---------------- */
function buildSitesPanel() {
  const el = H.el;
  const det = el('details', {class: 'mapsites', open: true});
  det.append(el('summary', {}, 'Sites — everything declared on this map (pad, clearing, soften, spawn, water, stamp, tree)'));
  els.sitesList = el('div', {class: 'mapsites-list'});
  els.sitePanel = el('div', {class: 'mapsites-panel'});
  det.append(els.sitesList, els.sitePanel);
  return det;
}
function siteWhere(s) {
  if ((s.kind === 'pad' || s.kind === 'clearing' || s.kind === 'soften') && Array.isArray(s.min)) return '(' + s.min.join(',') + ')..(' + s.max.join(',') + ')';
  if (s.kind === 'stamp') return '(' + siteXZ(s).join(',') + ')';
  return Array.isArray(s.at) ? '(' + s.at.join(',') + ')' : '';
}
function editSite(fn) {
  pushUndo(snapshot());
  fn();
  markDirty(true); syncSites(); paint();
}
function syncSites() {
  if (!map || !els.sitesList) return;
  const el = H.el;
  const sites = map.json.sites || (map.json.sites = []);
  els.sitesList.innerHTML = '';
  const order = {pad: 0, clearing: 1, soften: 1, spawn: 2, water: 3, stamp: 5, tree: 6};
  const sorted = sites.slice().sort((a, b) => (order[a.kind] ?? 9) - (order[b.kind] ?? 9));
  for (const s of sorted) {
    const row = el('div', {class: 'mapsite' + (selected === s ? ' on' : '')});
    const kind = el('span', {class: 'mapkind mapkind-' + s.kind}, s.kind);
    const id = el('input', {type: 'text', value: s.id || '', class: 'mapid', title: 'rename'});
    id.addEventListener('change', () => editSite(() => { s.id = id.value.trim() || s.kind; }));
    const where = el('span', {class: 'mapwhere'}, siteWhere(s) +
      (s.kind === 'water' ? ' ' + (s.preset || '?') : s.kind === 'stamp' ? ' ' + (s.template || '?') : s.kind === 'tree' ? ' ' + (s.species || '?') : ''));
    const sel = el('button', {title: 'select and centre on the map'}, selected === s ? 'selected' : 'select');
    sel.addEventListener('click', () => {
      selected = s;
      const [x, z] = siteCentre(s);
      view.cx = x; view.cz = z;
      syncSites(); paint();
    });
    const del = el('button', {title: 'delete this site'}, '×');
    del.addEventListener('click', () => {
      if (s.kind === 'pad' && !confirm(map.name === 'harness' ? 'Delete the harness pad? The selftest fixtures stand on its ground.' : 'Delete the pad box?')) return;
      editSite(() => { sites.splice(sites.indexOf(s), 1); if (selected === s) selected = null; });
    });
    row.append(kind, id, where, sel, del);
    els.sitesList.append(row);
  }
  if (!sorted.length) els.sitesList.append(el('div', {class: 'mapnote'}, 'no sites'));
  const pnl = els.sitePanel;
  pnl.innerHTML = '';
  if (!selected) { pnl.append(el('div', {class: 'mapnote'}, 'Select a site to edit it.')); return; }
  const s = selected;
  const num = (label, key, min, max, step, title) => {
    const inp = el('input', {type: 'number', min, max, step, value: s[key] == null ? '' : s[key], style: 'width:80px', title});
    inp.addEventListener('change', () => editSite(() => { s[key] = Math.max(min, Math.min(max, Math.round(+inp.value || 0))); }));
    return el('label', {title}, label, ' ', inp);
  };
  pnl.append(el('b', {}, s.kind + ' “' + (s.id || '') + '”'));
  if (s.kind === 'water') {
    pnl.append(el('span', {class: 'mapnote'}, 'preset ' + (s.preset || '?') + (s.radius ? ', radius ' + s.radius + ' vox' : ', the preset’s radius') + ' — drag it with the Water tool.'));
  } else if (s.kind === 'spawn') {
    pnl.append(el('span', {class: 'mapnote'}, 'Where the game starts and the centre of the calm home area (Terrain → home area). Move it with the Spawn tool; the spawn-site gate checks it is on land, off the pad and off any lake.'));
  } else if (s.kind === 'pad') {
    pnl.append(el('span', {class: 'mapnote'}, 'The calm pad box: no trunks, crowns, tarns or cover inside. Drag it with the Pad box tool.'));
  } else if (s.kind === 'clearing' || s.kind === 'soften') {
    const corner = (label, arr, i, title) => {
      const inp = el('input', {type: 'number', step: 1, value: s[arr][i], style: 'width:80px', title});
      inp.addEventListener('change', () => editSite(() => {
        s[arr][i] = Math.round(+inp.value || 0);
        const lo = Math.min(s.min[i], s.max[i]), hi = Math.max(s.min[i], s.max[i]);
        s.min[i] = lo; s.max[i] = hi;
      }));
      return el('label', {title}, label, ' ', inp);
    };
    if (s.feather == null) s.feather = s.kind === 'soften' ? 96 : 64;
    const w = s.max[0] - s.min[0] + 1, h = s.max[1] - s.min[1] + 1;
    const corners = [corner('min x', 'min', 0, 'west edge, world voxels (inclusive)'),
                     corner('min z', 'min', 1, 'north edge, world voxels (inclusive)'),
                     corner('max x', 'max', 0, 'east edge, world voxels (inclusive)'),
                     corner('max z', 'max', 1, 'south edge, world voxels (inclusive)')];
    if (s.kind === 'soften') {
      for (const [k, d] of [['hills', 40], ['bumps', 20], ['grain', 0], ['paths', 10]]) if (s[k] == null) s[k] = d;
      pnl.append(...corners,
                 num('feather (vox)', 'feather', 0, 512, 8, 'How far past the box the ground eases back to the biome’s own (a smooth S-curve); 0 = a hard edge.'),
                 num('hills %', 'hills', 0, 100, 5, 'How much of the biome’s rolling swells the ground keeps inside the box. 100 = untouched. A village: 20-40.'),
                 num('bumps %', 'bumps', 0, 100, 5, 'How much of the 3 m bumps the ground keeps.'),
                 num('grain %', 'grain', 0, 100, 5, 'How much of the sub-metre grain the ground keeps. 0 for a lawn.'),
                 num('paths %', 'paths', 0, 100, 5, 'The hills kept ON the villagers’ walking routes. 100 = no difference.'),
                 el('span', {class: 'mapnote'}, w + ' × ' + h + ' vox (' + (w / 10).toFixed(1) + ' × ' + (h / 10).toFixed(1) + ' m). ' +
                    'SOFTENED GROUND: inside the box the seeded relief is kept at these fractions, easing back across the dashed feather; the authored height is untouched. Drag it with the Soften tool; shift+click inside it deletes it.'));
      return;
    }
    pnl.append(...corners,
               num('feather (vox)', 'feather', 0, 512, 8, 'How wide the ragged forest edge is, past the crowns; 0 = a hard edge.'),
               el('span', {class: 'mapnote'}, w + ' × ' + h + ' vox (' + (w / 10).toFixed(1) + ' × ' + (h / 10).toFixed(1) + ' m). ' +
                  'No procedural tree’s CROWN reaches over this box, and across the dashed feather band trees thin out. Ground, cover and terrain are untouched. ' +
                  'Drag it with the Clearing tool; shift+click inside it deletes it.'));
  } else if (s.kind === 'stamp') {
    pnl.append(el('span', {class: 'mapnote'}, 'assets/prefabs/' + (s.template || '?') + '.vox' + (s.rot ? ', rotated ' + s.rot : '') + '. Shift+click its marker with the Stamp tool to delete.'));
  } else if (s.kind === 'tree') {
    const sp = el('select', {});
    for (const n of trees.list) sp.append(el('option', {value: n}, n));
    if (!trees.list.includes(s.species)) sp.append(el('option', {value: s.species || ''}, (s.species || '?') + ' (missing)'));
    sp.value = s.species || '';
    sp.addEventListener('change', () => editSite(() => { s.species = sp.value; }));
    const opt = (label, key, max, title) => {
      const inp = el('input', {type: 'number', min: 0, max, step: 1, value: Number.isInteger(s[key]) ? s[key] : '', placeholder: 'rolled', style: 'width:70px', title});
      inp.addEventListener('change', () => editSite(() => {
        if (inp.value === '') delete s[key]; else s[key] = Math.max(0, Math.min(max, Math.round(+inp.value || 0)));
      }));
      return el('label', {title}, label, ' ', inp);
    };
    pnl.append(el('label', {title: 'the species (assets/trees/<name>.svtree)'}, 'species ', sp),
               opt('variant', 'variant', 63, 'which baked variant; blank = rolled from the seed'),
               opt('turn (0..3)', 'rot', 3, 'quarter turns; blank = rolled from the seed (with a mirror)'),
               el('span', {class: 'mapnote'}, 'Stands on the generated ground at its column, drawn like any tree near and far. Not grown at or above the treeline (the load warns). Drag it with the Tree tool; shift+click deletes.'));
  } else {
    pnl.append(el('span', {class: 'mapnote'}, 'This page has no editor for kind “' + s.kind + '”; the Load check says whether the engine accepts it.'));
  }
}

/* ---- the real-terrain backdrop: the engine's own heights of the view ------------------- */
function decodeHeightmap(buf) {
  const d = new DataView(buf);
  if (d.getUint32(0, true) !== 0x4D485653) throw new Error('not a heightmap');
  const res = d.getUint32(8, true);
  const o = {res, span: d.getUint32(12, true), cx: d.getInt32(16, true), cz: d.getInt32(20, true),
             vpm: d.getInt32(28, true), hMin: d.getInt32(32, true), hMax: d.getInt32(36, true), sea: d.getInt32(40, true),
             h: new Int32Array(res * res), water: new Int16Array(res * res)};
  for (let i = 0; i < res * res; i++) { const b = 48 + i * 8; o.h[i] = d.getInt32(b, true); o.water[i] = d.getInt16(b + 4, true); }
  return o;
}
/** What --heightmap is asked for: THE VISIBLE SQUARE when zoomed in, the
 *  whole extent otherwise. `key` says whether the backdrop on screen answers it. */
function heightsRequest() {
  const e = map.json.extent, whole = Math.max(e.size[0], e.size[1]);
  const L = LC && LC.open(), lb = L && LOC.boundsOf(L);
  if (lb) {
    // THE LOCALE PAGE: the bounds (plus a margin), one sample a voxel up to
    // 1024 a side -- finer than the sculpt layer's 0.8 m, so a stroke's
    // preview and Flatten read the ground the brush is actually on.
    const span = Math.max(lb[2] - lb[0], lb[3] - lb[1]) + 1 + 64;
    const cx = Math.round((lb[0] + lb[2] + 1) / 2), cz = Math.round((lb[1] + lb[3] + 1) / 2);
    return {cx, cz, span, res: Math.min(1024, span), whole, key: 'locale:' + cx + ',' + cz + ',' + span};
  }
  const c = els.canvas;
  const vis = Math.max(c.clientWidth || 1, c.clientHeight || 1) / view.scale;
  const span = Math.min(whole, Math.max(256, Math.round(vis)));
  let cx, cz, res;
  if (span >= whole) { cx = e.min[0] + e.size[0] / 2; cz = e.min[1] + e.size[1] / 2; res = 1024; }
  else { cx = view.cx; cz = view.cz; res = Math.min(1024, Math.max(64, Math.round(span / 2))); }
  cx = Math.round(cx); cz = Math.round(cz);
  return {cx, cz, span, res, whole, key: cx + ',' + cz + ',' + span};
}
async function fetchHeights() {
  if (!map) return;
  const {cx, cz, span, res, whole, key} = heightsRequest();
  status('rendering the engine’s heights over ' + (span >= whole ? 'the whole map' : 'this view') + ' (the SAVED files)…');
  // --heightmap reads tuning.json's world.mapLayer off disk
  if (H.syncTuning) await H.syncTuning();
  const gen = savedGen;
  const base = scClone(sculpt.saved);
  const seed = H.previewSeed ? H.previewSeed() : 1337;
  // &map=: THIS page's map (E4), not necessarily the one tuning names
  const r = await fetch('/api/heightmap?cx=' + cx + '&cz=' + cz + '&span=' + span + '&res=' + res + '&seed=' + seed +
                        '&map=' + encodeURIComponent(map.name), {cache: 'no-store'});
  if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
  const D = decodeHeightmap(await r.arrayBuffer());
  heights = {res: D.res, img: new ImageData(D.res, D.res), D, base, hc: null, key, gen, seed, ver: 0, drawn: -1};
  showHeights = true;
  range.vals = null;
  rampRange();
  reshadeHeights(null);
  status('real terrain: ' + fmtM((D.hMin - seaY()) / vpm()) + '..' + fmtM((D.hMax - seaY()) / vpm()) + ' above sea over ' + fmtM(D.span / vpm()) +
         ' at ' + (D.span / D.res / vpm()).toFixed(1) + ' m/px, seed ' + seed + ' (the SAVED map, plus your unsaved sculpting live)');
  if (els.heightsBtn) els.heightsBtn.classList.add('on');
  paint();
}
// Zoomed in with the backdrop on, it follows the view: a refetch once the
// view has been still for a moment and no longer matches what is drawn.
let realTimer = 0;
function scheduleRealRefresh() {
  if (!showHeights || !map) return;
  clearTimeout(realTimer);
  realTimer = setTimeout(() => {
    if (!showHeights || !map || stroke || (heights && heights.key === heightsRequest().key)) return;
    fetchHeights().catch(e => status('real terrain failed: ' + e.message));
  }, 900);
}
/** World-voxel box around every tile of the edited and the fetched layer, or null. */
function sculptBounds() {
  let b = null;
  for (const m of [sculpt.tiles, heights && heights.base]) {
    if (!m) continue;
    for (const k of m.keys()) {
      const tx = Math.floor(k / 65536) - 32768, tz = (k % 65536) - 32768;
      const x0 = tx * SC_TILE - SC_SP, z0 = tz * SC_TILE - SC_SP, x1 = (tx + 1) * SC_TILE + SC_SP, z1 = (tz + 1) * SC_TILE + SC_SP;
      b = b ? [Math.min(b[0], x0), Math.min(b[1], z0), Math.max(b[2], x1), Math.max(b[3], z1)] : [x0, z0, x1, z1];
    }
  }
  return b;
}
/** Rebuild the backdrop image (all of it, or the pixels over world rect
 *  `rect`) from the --heightmap grid plus the difference between the sculpt
 *  layer being edited and the one it was rendered with, on the page's
 *  hypsometric ramp, hillshaded. */
function reshadeHeights(rect) {
  if (!heights || !heights.D) return;
  const D = heights.D, res = D.res, step = D.span / res;
  const X0 = D.cx - Math.trunc(D.span / 2), Z0 = D.cz - Math.trunc(D.span / 2);
  if (!heights.hc) heights.hc = Float32Array.from(D.h);
  const hc = heights.hc;
  let i0 = 0, i1 = res - 1, j0 = 0, j1 = res - 1;
  if (rect) {
    i0 = Math.max(0, Math.floor((rect[0] - X0) / step - 1)); i1 = Math.min(res - 1, Math.ceil((rect[2] - X0) / step));
    j0 = Math.max(0, Math.floor((rect[1] - Z0) / step - 1)); j1 = Math.min(res - 1, Math.ceil((rect[3] - Z0) / step));
    if (i0 > i1 || j0 > j1) return;
  }
  const tb = sculptBounds();
  for (let j = j0; j <= j1; j++) {
    const z = Z0 + Math.trunc((j + 0.5) * step);
    for (let i = i0; i <= i1; i++) {
      const k = j * res + i;
      const x = X0 + Math.trunc((i + 0.5) * step);
      let d = 0;
      if (tb && x >= tb[0] && x <= tb[2] && z >= tb[1] && z <= tb[3]) d = scAt(sculpt.tiles, x, z) - scAt(heights.base, x, z);
      hc[k] = D.h[k] + d;
    }
  }
  const px = heights.img.data, sea = seaY(), [lo, hi] = rampRange(), col = [0, 0, 0];
  const sp2 = 2 * step;
  for (let j = Math.max(0, j0 - 1); j <= Math.min(res - 1, j1 + 1); j++) for (let i = Math.max(0, i0 - 1); i <= Math.min(res - 1, i1 + 1); i++) {
    const k = j * res + i, hh = hc[k];
    const dx = (hc[j * res + Math.min(i + 1, res - 1)] - hc[j * res + Math.max(i - 1, 0)]) / sp2 * 1.5;
    const dz = (hc[Math.min(j + 1, res - 1) * res + i] - hc[Math.max(j - 1, 0) * res + i]) / sp2 * 1.5;
    rampColor(hh, lo, hi, sea, col);
    const s = (dx * 0.5 + 0.7071 + dz * 0.5) / Math.sqrt(dx * dx + 1 + dz * dz), k2 = 0.3 + 0.95 * Math.max(0, s);
    const o = k * 4;
    px[o] = Math.min(255, col[0] * k2); px[o + 1] = Math.min(255, col[1] * k2); px[o + 2] = Math.min(255, col[2] * k2); px[o + 3] = 255;
  }
  heights.ver = (heights.ver || 0) + 1;
}
let sculptPreviewQueued = false;
function scheduleSculptPreview() {
  if (sculptPreviewQueued) return;
  sculptPreviewQueued = true;
  requestAnimationFrame(() => {
    sculptPreviewQueued = false;
    if (heights && heights.D && sculpt.dirtyRect) reshadeHeights(sculpt.dirtyRect);
    sculpt.dirtyRect = null;
    paint();
  });
}
/** The sculpt layer drawn over the map: warm = raised, cool = lowered, one
 *  32x32 image per tile, rebuilt only when that tile changed. */
function sculptTileCanvas(k, t) {
  const ver = sculpt.tver.get(k) || 0;
  let e = sculpt.over.get(k);
  if (e && e.ver === ver) return e.canvas;
  if (!e) { e = {ver: -1, canvas: document.createElement('canvas')}; e.canvas.width = SC_T; e.canvas.height = SC_T; sculpt.over.set(k, e); }
  const ctx = e.canvas.getContext('2d');
  const im = ctx.createImageData(SC_T, SC_T), d = im.data;
  for (let i = 0; i < SC_T * SC_T; i++) {
    const v = t[i], a = Math.min(1, Math.abs(v) / 60) * 200;
    if (v > 0) { d[i * 4] = 255; d[i * 4 + 1] = 150; d[i * 4 + 2] = 60; } else { d[i * 4] = 80; d[i * 4 + 1] = 160; d[i * 4 + 2] = 255; }
    d[i * 4 + 3] = Math.abs(v) < 0.5 ? 0 : Math.max(40, a);
  }
  ctx.putImageData(im, 0, 0);
  e.ver = ver;
  return e.canvas;
}
function paintSculpt(ctx) {
  if (!sculpt.show || !sculpt.tiles.size) return;
  const side = SC_TILE * view.scale;
  const W = els.canvas.width, Hh = els.canvas.height;
  for (const [k, t] of sculpt.tiles) {
    const tx = Math.floor(k / 65536) - 32768, tz = (k % 65536) - 32768;
    const x = SX(tx * SC_TILE), y = SZ(tz * SC_TILE);
    if (x > W || y > Hh || x + side < 0 || y + side < 0) continue;
    ctx.drawImage(sculptTileCanvas(k, t), x, y, Math.max(1, side), Math.max(1, side));
  }
}

/* ---- THE HEIGHT TOOLS: shape stamps -------------------------------------------------------- */
/** The stamp the Shape tool would make at (wx, wz): heightshapes.js options. */
function shapeOpts(wx, wz) {
  const v = vpm(), base = MT.heightAt(map.H, wx, wz), H = shape.heightM * v, S = HS.SHAPES[shape.kind];
  const o = {kind: shape.kind, x: wx, z: wz, radius: shape.radiusM * v, aspect: shape.aspect, rot: shape.rot};
  if (S.add) {
    // rise: the summit / floor is that far from the ground at the centre; summit:
    // that many metres above the sea (a basin's FLOOR)
    if (shape.mode === 'rise') o.height = H;
    else o.height = shape.kind === 'basin' ? base - (seaY() + H) : seaY() + H - base;
  } else o.top = shape.mode === 'rise' ? base + H : seaY() + H;
  return o;
}
function stampShape(wx, wz) {
  if (!MT.inExtent(map.H, wx, wz)) { status('the shape stamp goes inside the map extent'); return; }
  const before = new Map();
  const r = MT.applyOp(map.H, HS.shapeStamp(map.H, shapeOpts(wx, wz)), before);
  if (r.skipped) status('some tiles under the stamp are still loading; they were skipped — undo and stamp again in a moment');
  if (!r.changed) return;
  pushUndo({tiles: {height: before}});
  range.vals = null;
  markDirty(true);
  const v = vpm();
  status(`${shape.kind} stamped: ${r.changed} posts, centre now ${fmtM((MT.heightAt(map.H, wx, wz) - seaY()) / v)} above sea (Ctrl+Z takes it back)`);
}
/** The stamp's outline at the cursor: its ellipse, rotated, and its long axis. */
function paintShapeCursor(ctx, wx, wz) {
  const v = vpm(), R = Math.max(map.H.sp, shape.radiusM * v) * view.scale;
  const asp = Math.max(0.05, Math.min(1, shape.aspect)), a = shape.rot * Math.PI / 180;
  const sx = SX(wx), sz = SZ(wz);
  ctx.strokeStyle = shape.kind === 'basin' ? 'rgba(120,180,255,0.9)' : 'rgba(255,170,90,0.9)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  if (shape.kind === 'ridge') {
    // the ridge stamp's footprint is its rectangle (u, v in -1..1)
    const c = Math.cos(a), s = Math.sin(a), pts = [[1, 1], [-1, 1], [-1, -1], [1, -1]];
    pts.forEach(([u, w], k) => {
      const x = sx + (u * R * c - w * R * asp * s), z = sz + (u * R * s + w * R * asp * c);
      if (k) ctx.lineTo(x, z); else ctx.moveTo(x, z);
    });
    ctx.closePath();
  } else ctx.ellipse(sx, sz, Math.max(2, R), Math.max(2, R * asp), a, 0, Math.PI * 2);
  ctx.stroke();
  if (asp < 1) {
    ctx.setLineDash([2, 3]);
    ctx.beginPath(); ctx.moveTo(sx - Math.cos(a) * R, sz - Math.sin(a) * R); ctx.lineTo(sx + Math.cos(a) * R, sz + Math.sin(a) * R); ctx.stroke();
    ctx.setLineDash([]);
  }
  ctx.fillStyle = '#ffd866'; ctx.font = '11px monospace';
  const H = shape.heightM;
  ctx.fillText(`${shape.kind} ${shape.mode === 'rise' ? (shape.kind === 'basin' ? '-' : '+') + H + ' m' : (shape.kind === 'basin' ? 'floor ' : 'top ') + H + ' m asl'} r ${fmtM(shape.radiusM)}` +
               (asp < 1 ? ` ${shape.rot}°` : ''), sx + 8, sz - 8);
}

/* ---- THE HEIGHT TOOLS: ridge / valley lines ------------------------------------------------- */
const ridgesOf = () => (map.json.ridges || (map.json.ridges = []));
function newLineId(kind) {
  const rs = ridgesOf();
  let n = rs.filter(r => r.kind === kind).length, id = kind + '_' + n;
  while (rs.some(r => r.id === id)) id = kind + '_' + (++n);
  return id;
}
/** A new vertex's crest / floor height: metres above sea, or the plane's
 *  height there plus / minus a rise. */
function vertexY(wx, wz, kind) {
  const v = vpm();
  if (lines.yMode === 'sea') return Math.round(seaY() + lines.yM * v);
  return Math.round(MT.heightAt(map.H, wx, wz) + (kind === 'valley' ? -1 : 1) * lines.dyM * v);
}
/** The line no longer matches the plane (its bake, if any, is stale). */
function lineEdited(L) { if (L.baked) L.baked = false; }
function segDistPx(a, b, wx, wz) {
  const ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
  const t = L2 > 0 ? Math.max(0, Math.min(1, ((wx - a[0]) * ex + (wz - a[1]) * ez) / L2)) : 0;
  return {d: Math.hypot(wx - a[0] - ex * t, wz - a[1] - ez * t) * view.scale, t};
}
function linePointerDown(ev) {
  const [wx, wz] = worldColumnAt(ev);
  const rs = ridgesOf();
  const grab = 7;   // screen pixels
  // 1. a vertex (of the selected line first): drag it; shift+click deletes it
  const order = lines.sel ? [lines.sel, ...rs.filter(r => r !== lines.sel)] : rs;
  for (const L of order)
    for (let k = 0; k < L.points.length; k++) {
      const p = L.points[k];
      if (Math.hypot((p[0] - wx) * view.scale, (p[1] - wz) * view.scale) > grab) continue;
      if (ev.shiftKey) {
        pushUndo(snapshot());
        L.points.splice(k, 1);
        if (!L.points.length) { rs.splice(rs.indexOf(L), 1); if (lines.sel === L) { lines.sel = null; lines.drawing = false; } }
        else lineEdited(L);
        markDirty(true); syncLines();
        return;
      }
      if (lines.drawing && L === lines.sel && k === L.points.length - 1 && k > 0) { lines.drawing = false; syncLines(); return; }
      lines.sel = L;
      lines.drag = {line: L, k, snap: snapshot(), moved: false};
      syncLines();
      return;
    }
  // 2. drawing: the click is the next vertex
  if (lines.drawing && lines.sel) {
    pushUndo(snapshot());
    lines.sel.points.push([wx, wz, vertexY(wx, wz, lines.sel.kind)]);
    lineEdited(lines.sel);
    markDirty(true); syncLines();
    return;
  }
  // 3. on a segment of the selected line: insert a vertex there (height lerped)
  if (lines.sel) {
    const pts = lines.sel.points;
    for (let k = 0; k + 1 < pts.length; k++) {
      const {d, t} = segDistPx(pts[k], pts[k + 1], wx, wz);
      if (d > 5) continue;
      pushUndo(snapshot());
      pts.splice(k + 1, 0, [wx, wz, Math.round(pts[k][2] + (pts[k + 1][2] - pts[k][2]) * t)]);
      lineEdited(lines.sel);
      lines.drag = {line: lines.sel, k: k + 1, snap: null, moved: false};
      markDirty(true); syncLines();
      return;
    }
  }
  // 4. on another line: select it
  for (const L of rs)
    for (let k = 0; k + 1 < L.points.length; k++)
      if (segDistPx(L.points[k], L.points[k + 1], wx, wz).d <= 5) { lines.sel = L; lines.drawing = false; syncLines(); return; }
  // 5. open map: a new line, drawing
  pushUndo(snapshot());
  const L = {id: newLineId(lines.kind), kind: lines.kind, profile: lines.profile, width: Math.round(lines.widthM * vpm()),
             points: [[wx, wz, vertexY(wx, wz, lines.kind)]]};
  rs.push(L);
  lines.sel = L; lines.drawing = true;
  markDirty(true); syncLines();
  status(`new ${L.kind} line: click to add vertices, double-click / Enter to finish; then Bake in the Height tools panel`);
}
function paintLines(ctx) {
  const rs = map.json.ridges || [];
  if (!rs.length) return;
  const v = vpm();
  ctx.font = '11px monospace';
  for (const L of rs) {
    const pts = L.points || [];
    if (!pts.length) continue;
    const valley = L.kind === 'valley', sel = L === lines.sel;
    const colr = valley ? '#6fb4ff' : '#ff9f4a';
    // the width band: what the bake can touch
    ctx.globalAlpha = sel ? 0.22 : 0.12;
    ctx.strokeStyle = colr; ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    ctx.lineWidth = Math.max(2, 2 * (L.width || 0) * view.scale);
    ctx.beginPath();
    pts.forEach((p, k) => (k ? ctx.lineTo(SX(p[0]), SZ(p[1])) : ctx.moveTo(SX(p[0]), SZ(p[1]))));
    if (pts.length === 1) ctx.lineTo(SX(pts[0][0]) + 0.01, SZ(pts[0][1]));
    ctx.stroke();
    ctx.globalAlpha = 1;
    // the line itself: solid when the plane holds it, dashed when it is not (or no longer) baked
    ctx.lineWidth = sel ? 2 : 1; ctx.lineCap = 'butt';
    if (!L.baked) ctx.setLineDash([5, 3]);
    ctx.beginPath();
    pts.forEach((p, k) => (k ? ctx.lineTo(SX(p[0]), SZ(p[1])) : ctx.moveTo(SX(p[0]), SZ(p[1]))));
    ctx.stroke();
    ctx.setLineDash([]);
    // vertices: pixel squares, the heights beside them when the selected line is zoomed enough
    for (let k = 0; k < pts.length; k++) {
      const x = Math.round(SX(pts[k][0])), z = Math.round(SZ(pts[k][1]));
      ctx.fillStyle = sel ? '#ffffff' : colr;
      ctx.fillRect(x - 2, z - 2, 5, 5);
      ctx.fillStyle = '#0e1219'; ctx.fillRect(x - 1, z - 1, 3, 3);
      if (sel) { ctx.fillStyle = colr; ctx.fillText(fmtM((pts[k][2] - seaY()) / v), x + 6, z - 4); }
    }
    const p0 = pts[0];
    ctx.fillStyle = colr;
    ctx.fillText(L.id + (L.baked ? '' : ' (not baked)'), SX(p0[0]) + 6, SZ(p0[1]) + 12);
  }
}
/** Bake line L into the height plane. With a bake record from this session it
 *  is a REBAKE: the old bake comes out first (MT.unbake: back to the pre-bake
 *  posts, keeping any stroke painted since as the offset it made), then the
 *  line as it is now goes in. One undo step either way. */
function bakeLine(L) {
  const P = map.H;
  if ((L.points || []).length < 1) return;
  const snap = snapshot(), undoTiles = new Map(), prev = bakes.get(L.id) || null;
  let since = 0;
  if (prev) since = MT.unbake(P, prev, undoTiles);
  const opBefore = new Map();
  const r = MT.applyOp(P, HS.lineBake(P, L), opBefore);
  for (const [k, t] of opBefore) if (!undoTiles.has(k)) undoTiles.set(k, t);
  bakes.set(L.id, {before: opBefore, after: MT.snapTiles(P, opBefore.keys())});
  L.baked = true;
  pushUndo({tiles: {height: undoTiles}, json: snap.json, bake: {id: L.id, rec: prev}});
  range.vals = null;
  markDirty(true); syncLines(); paint();
  status(`${prev ? 'rebaked' : 'baked'} ${L.id}: ${r.changed} posts over ${opBefore.size} tile(s)` +
         (prev && since ? `; ${since} posts painted since the last bake kept as offsets` : '') +
         (r.skipped ? `; ${r.skipped} posts in tiles still loading were SKIPPED (undo and bake again)` : ''));
}
function unbakeLine(L) {
  const prev = bakes.get(L.id);
  if (!prev) return;
  const snap = snapshot(), undoTiles = new Map();
  const since = MT.unbake(map.H, prev, undoTiles);
  bakes.delete(L.id);
  L.baked = false;
  pushUndo({tiles: {height: undoTiles}, json: snap.json, bake: {id: L.id, rec: prev}});
  range.vals = null;
  markDirty(true); syncLines(); paint();
  status(`unbaked ${L.id}` + (since ? ` (${since} posts painted since kept as offsets)` : ''));
}
/** The line's profile chart: the plane under the line (grey) against the
 *  crest / floor heights (the line colour), metres above sea. */
function drawProfile(cv, L) {
  const ctx = cv.getContext('2d'), W = cv.width, Hh = cv.height;
  ctx.fillStyle = '#0e1219'; ctx.fillRect(0, 0, W, Hh);
  const pts = L.points;
  if (pts.length < 2) { ctx.fillStyle = '#6f7f97'; ctx.font = '10px monospace'; ctx.fillText('two vertices make a profile', 6, 14); return; }
  const seg = [], v = vpm();
  let tot = 0;
  for (let k = 0; k + 1 < pts.length; k++) { const d = Math.hypot(pts[k + 1][0] - pts[k][0], pts[k + 1][1] - pts[k][1]); seg.push(d); tot += d; }
  const N = W, g = new Float32Array(N), y = new Float32Array(N);
  for (let n = 0; n < N; n++) {
    let s = tot * n / (N - 1), k = 0;
    while (k < seg.length - 1 && s > seg[k]) { s -= seg[k]; k++; }
    const t = seg[k] > 0 ? Math.min(1, s / seg[k]) : 0, a = pts[k], b = pts[k + 1];
    g[n] = MT.heightAt(map.H, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
    y[n] = a[2] + (b[2] - a[2]) * t;
  }
  let lo = Infinity, hi = -Infinity;
  for (let n = 0; n < N; n++) { lo = Math.min(lo, g[n], y[n]); hi = Math.max(hi, g[n], y[n]); }
  if (hi - lo < 10) { hi += 5; lo -= 5; }
  const Y = h => Math.round(Hh - 4 - (h - lo) / (hi - lo) * (Hh - 16));
  ctx.fillStyle = '#3a4a3a';
  for (let n = 0; n < N; n++) { const top = Y(g[n]); ctx.fillRect(n, top, 1, Hh - top); }
  ctx.fillStyle = L.kind === 'valley' ? '#6fb4ff' : '#ff9f4a';
  for (let n = 0; n < N; n++) ctx.fillRect(n, Y(y[n]), 1, 2);
  ctx.fillStyle = '#c7d2e3'; ctx.font = '10px monospace';
  ctx.fillText(`${fmtM((hi - seaY()) / v)}`, 3, 10);
  ctx.fillText(`${fmtM((lo - seaY()) / v)}`, 3, Hh - 3);
  ctx.textAlign = 'right'; ctx.fillText(fmtM(tot / v), W - 3, 10); ctx.textAlign = 'left';
}
/** The Lines part of the Height tools panel: every line, the selected one's inspector. */
function syncLines() {
  if (!els.lineList || !map) return;
  const el = H.el, v = vpm(), rs = map.json.ridges || [];
  els.lineList.replaceChildren();
  for (const L of rs) {
    const row = el('div', {class: 'mapsite' + (L === lines.sel ? ' on' : '')},
      el('span', {class: 'mapkind mapkind-' + (L.kind === 'valley' ? 'water' : 'pad')}, L.kind),
      el('span', {class: 'mapwhere'}, `${L.id} · ${L.points.length} pts · ${L.profile} · ${fmtM((L.width || 0) / v)}` + (L.baked ? '' : ' · not baked')));
    const sel = el('button', {title: 'select it (and centre the map on it)'}, L === lines.sel ? 'selected' : 'select');
    sel.addEventListener('click', () => {
      lines.sel = L; lines.drawing = false;
      const p = L.points[Math.floor(L.points.length / 2)];
      if (p) { view.cx = p[0]; view.cz = p[1]; }
      syncLines(); paint();
    });
    const del = el('button', {title: 'delete the LINE (the plane keeps what it baked; Unbake first to take that out)'}, '×');
    del.addEventListener('click', () => {
      pushUndo(snapshot());
      rs.splice(rs.indexOf(L), 1);
      if (lines.sel === L) { lines.sel = null; lines.drawing = false; }
      markDirty(true); syncLines(); paint();
    });
    row.append(sel, del);
    els.lineList.append(row);
  }
  if (!rs.length) els.lineList.append(el('div', {class: 'mapnote'}, 'no lines — pick the Line tool and click on the map'));
  const box = els.lineInsp;
  box.replaceChildren();
  const L = lines.sel;
  if (!L) { box.append(el('div', {class: 'mapnote'}, 'Select a line to edit its vertices, profile and width, and to bake it.')); return; }
  const edit = (fn) => { pushUndo(snapshot()); fn(); lineEdited(L); markDirty(true); syncLines(); paint(); };
  const kind = el('select', {}, ...['ridge', 'valley'].map(k => el('option', {value: k}, k)));
  kind.value = L.kind;
  kind.addEventListener('change', () => edit(() => { L.kind = kind.value; }));
  const prof = el('select', {title: 'cross-section: V straight flanks; U broad back / flat floor; round; sharp = knife edge / slot gorge'},
                  ...HS.PROFILE_NAMES.map(k => el('option', {value: k}, k)));
  prof.value = L.profile || 'round';
  prof.addEventListener('change', () => edit(() => { L.profile = prof.value; }));
  const wid = el('input', {type: 'number', min: 1, step: 10, value: Math.round((L.width || 0) / v), style: 'width:70px', title: 'half-width in metres: how far either side of the line the bake reaches'});
  wid.addEventListener('change', () => edit(() => { L.width = Math.max(map.H.sp, Math.round((+wid.value || 0) * v)); }));
  box.append(el('div', {class: 'mapbar'}, el('b', {}, L.id), ' ', kind, ' ', prof, el('label', {}, ' half-width ', wid, ' m')));
  const tbl = el('div', {class: 'maplinepts'});
  L.points.forEach((p, k) => {
    const hIn = el('input', {type: 'number', step: 5, value: Math.round((p[2] - seaY()) / v * 10) / 10, style: 'width:72px',
                             title: (L.kind === 'valley' ? 'floor' : 'crest') + ' height at this vertex, metres above sea level'});
    hIn.addEventListener('change', () => edit(() => { p[2] = Math.round(seaY() + (+hIn.value || 0) * v); }));
    const g = MT.heightAt(map.H, p[0], p[1]);
    tbl.append(el('div', {class: 'mapsite'},
      el('span', {class: 'mapwhere'}, `${k} · ${fmtM(p[0] / v)}, ${fmtM(p[1] / v)} · plane ${fmtM((g - seaY()) / v)}`), hIn, el('span', {class: 'mapunit'}, 'm asl')));
  });
  box.append(tbl);
  const allIn = el('input', {type: 'number', step: 10, value: Math.round((L.points[0][2] - seaY()) / v), style: 'width:70px'});
  const allBtn = el('button', {title: 'every vertex to this height (m above sea)'}, 'all to');
  allBtn.addEventListener('click', () => edit(() => { for (const p of L.points) p[2] = Math.round(seaY() + (+allIn.value || 0) * v); }));
  const ramp = el('button', {title: 'the inner vertices on a straight grade between the first and the last (by distance along the line)'}, 'ramp ends');
  ramp.addEventListener('click', () => edit(() => {
    const pts = L.points, d = [0];
    for (let k = 1; k < pts.length; k++) d.push(d[k - 1] + Math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]));
    const T = d[d.length - 1] || 1, a = pts[0][2], b = pts[pts.length - 1][2];
    for (let k = 1; k + 1 < pts.length; k++) pts[k][2] = Math.round(a + (b - a) * d[k] / T);
  }));
  const relIn = el('input', {type: 'number', step: 10, value: lines.dyM, style: 'width:60px'});
  const rel = el('button', {title: 'every vertex this many metres above (ridge) / below (valley) the plane under it as it is now'}, 'from plane ±');
  rel.addEventListener('click', () => edit(() => {
    for (const p of L.points) p[2] = Math.round(MT.heightAt(map.H, p[0], p[1]) + (L.kind === 'valley' ? -1 : 1) * (+relIn.value || 0) * v);
  }));
  box.append(el('div', {class: 'mapbar'}, allBtn, allIn, ' m ', ramp, ' ', rel, relIn, ' m'));
  const rec = bakes.get(L.id);
  const bake = el('button', {class: 'mapgo', title: rec ? 'REBAKE: take this session’s last bake of the line out (strokes painted since stay, as offsets) and bake the line as it is now' :
                                                  'write the line into the height plane (one undo step)'}, rec ? 'Rebake' : 'Bake');
  bake.addEventListener('click', () => bakeLine(L));
  const parts = [bake];
  if (rec) {
    const ub = el('button', {title: 'take the bake back out of the plane (strokes painted since stay, as offsets)'}, 'Unbake');
    ub.addEventListener('click', () => unbakeLine(L));
    parts.push(ub);
  }
  const fin = el('button', {title: 'stop adding vertices (Enter / double-click)'}, lines.drawing ? 'Finish line' : 'Continue line');
  fin.addEventListener('click', () => { lines.drawing = !lines.drawing; tool = 'line'; syncControls(); syncLines(); paint(); });
  parts.push(fin);
  box.append(el('div', {class: 'mapbar'}, ...parts,
    el('span', {class: 'mapnote'}, rec ? (L.baked ? ' baked this session' : ' edited since its bake') :
       (L.baked ? ' baked in an earlier session: no pre-bake copy here, so Bake bakes on top' : ' not baked'))));
  const cv = el('canvas', {width: 300, height: 84, class: 'mapprofile', title: 'the plane under the line (grey) and the line’s crest / floor heights, metres above sea'});
  box.append(cv);
  drawProfile(cv, L);
}

/* ---- THE HEIGHT TOOLS: the Area, erosion, PNG import / export ---------------------------------- */
/** The sample rectangle [i0, j0, w, h] of plane P inside the Area (posts on
 *  the height plane, cells whose CENTRE is inside on the biome plane), or the
 *  whole plane (the biome plane's extra row / column past the extent left out). */
function areaRect(P) {
  const centred = P.name === 'biome';
  const nx = P.posts[0] - (centred ? 1 : 0), nz = P.posts[1] - (centred ? 1 : 0);
  if (!area) return [0, 0, nx, nz];
  const o = centred ? 0.5 : 0;
  const i0 = Math.max(0, Math.ceil((area[0] - P.min[0]) / P.sp - o)), i1 = Math.min(nx - 1, Math.floor((area[2] - P.min[0]) / P.sp - o));
  const j0 = Math.max(0, Math.ceil((area[1] - P.min[1]) / P.sp - o)), j1 = Math.min(nz - 1, Math.floor((area[3] - P.min[1]) / P.sp - o));
  return [i0, j0, Math.max(0, i1 - i0 + 1), Math.max(0, j1 - j0 + 1)];
}
/** True while a tile the rectangle covers is listed but not loaded yet. */
function rectBlocked(P, [i0, j0, w, h]) {
  for (let tz = j0 >> 8; tz <= (j0 + h - 1) >> 8; tz++)
    for (let tx = i0 >> 8; tx <= (i0 + w - 1) >> 8; tx++) if (MT.blocked(P, MT.tileKey(tx, tz))) return true;
  return false;
}
function paintArea(ctx) {
  if (!area) return;
  const x0 = SX(area[0]), z0 = SZ(area[1]), x1 = SX(area[2]), z1 = SZ(area[3]);
  ctx.strokeStyle = erosion.pending ? '#ffd866' : '#ffffff'; ctx.lineWidth = 1;
  ctx.setLineDash([6, 3]);
  ctx.strokeRect(Math.round(x0) + 0.5, Math.round(z0) + 0.5, Math.round(x1 - x0), Math.round(z1 - z0));
  ctx.setLineDash([]);
  ctx.fillStyle = '#ffffff'; ctx.font = '11px monospace';
  const [, , w, h] = areaRect(map.H);
  ctx.fillText(`area ${w}×${h} posts` + (erosion.pending ? ' — erosion PREVIEW (commit / discard in Height tools)' : ''), x0 + 3, z0 - 4);
}
/* ---- THE TERRAIN JOBS: terrainjobs.js in a worker (terrain_worker.js) ----------------------
 * One MAIN job at a time (a bake, a generate, an erosion): it owns the job bar
 * (label, progress, Cancel) under the toolbar. QUIET jobs (live previews) run
 * beside it in their own worker, superseded by the next of their key and
 * cancelled when a main job starts. Without module workers the job runs on
 * this thread (same result, the page waits). */
let mainJob = null;
const quietJobs = new Map();
function runJob(job, opts = {}) {
  let worker = null, done = false;
  const quiet = !!opts.quiet, key = opts.key || 'job';
  if (quiet && quietJobs.has(key)) quietJobs.get(key).cancel();
  if (!quiet) { if (mainJob) throw new Error('another terrain job is running: ' + mainJob.label); for (const q of quietJobs.values()) q.cancel(); }
  const p = new Promise((resolve, reject) => {
    const finish = () => { done = true; if (worker) worker.terminate(); if (quiet) { if (quietJobs.get(key) === p) quietJobs.delete(key); } else if (mainJob === p) { mainJob = null; syncJobBar(); } };
    try { worker = new Worker(new URL('./terrain_worker.js', import.meta.url), {type: 'module'}); }
    catch (e) {
      import('./terrainjobs.js').then(m => { const r = m.runJob(job); finish(); resolve(r); }).catch(err => { finish(); reject(err); });
      return;
    }
    worker.onmessage = (ev) => {
      const m = ev.data;
      if (m.progress != null) { p.progress = m.progress; if (opts.onProgress) opts.onProgress(m.progress); if (!quiet) syncJobBar(); return; }
      finish();
      if (m.error) reject(new Error(m.error)); else resolve(m.result);
    };
    worker.onerror = (ev) => { finish(); reject(new Error(ev.message || 'terrain worker failed')); };
    const tr = job.grid && job.grid.h && job.grid.h.buffer ? [job.grid.h.buffer] : [];
    worker.postMessage({id: 1, job}, tr);
  });
  p.label = opts.label || job.job;
  p.progress = 0;
  p.cancelled = false;
  p.cancel = () => {
    if (done) return;
    p.cancelled = true; done = true;
    if (worker) worker.terminate();
    if (quiet) { if (quietJobs.get(key) === p) quietJobs.delete(key); } else if (mainJob === p) { mainJob = null; syncJobBar(); }
    if (opts.onCancel) opts.onCancel();
  };
  if (quiet) quietJobs.set(key, p); else { mainJob = p; syncJobBar(); }
  return p;
}
function buildJobBar() {
  const el = H.el;
  els.jobLabel = el('span', {class: 'mapnote'});
  els.jobProg = el('progress', {max: 100, value: 0, style: 'width:180px;height:10px'});
  const cancel = el('button', {title: 'stop the job (nothing is written)'}, 'Cancel');
  cancel.addEventListener('click', () => { if (mainJob) mainJob.cancel(); });
  const bar = el('div', {class: 'mapbar mapjob'}, el('b', {}, 'working: '), els.jobLabel, els.jobProg, cancel);
  bar.style.display = 'none';
  return bar;
}
function syncJobBar() {
  if (!els.jobBar) return;
  els.jobBar.style.display = mainJob ? '' : 'none';
  if (!mainJob) return;
  els.jobLabel.textContent = mainJob.label + ' ';
  els.jobProg.value = Math.round((mainJob.progress || 0) * 100);
}
/** The posts of the plane inside world box [x0, z0, x1, z1] (or all of it)
 *  as a terrainjobs grid {w, hh, x0, z0, cell, h} + the rect. */
function postsGrid(box) {
  const P = map.H, nx = P.posts[0], nz = P.posts[1];
  let i0 = 0, j0 = 0, i1 = nx - 1, j1 = nz - 1;
  if (box) {
    i0 = Math.max(0, Math.floor((box[0] - P.min[0]) / P.sp)); i1 = Math.min(nx - 1, Math.ceil((box[2] - P.min[0]) / P.sp));
    j0 = Math.max(0, Math.floor((box[1] - P.min[1]) / P.sp)); j1 = Math.min(nz - 1, Math.ceil((box[3] - P.min[1]) / P.sp));
  }
  const w = i1 - i0 + 1, hh = j1 - j0 + 1;
  if (w < 2 || hh < 2) return null;
  return {rect: [i0, j0, w, hh], grid: {w, hh, x0: P.min[0] + i0 * P.sp, z0: P.min[1] + j0 * P.sp, cell: P.sp, h: null}};
}
/** A coarse grid of the posts in world box (box-averaged by f) for a preview job. */
function coarseGrid(box, f) {
  const g = postsGrid(box);
  if (!g) return null;
  const [i0, j0, w, hh] = g.rect, cw = Math.ceil(w / f), ch = Math.ceil(hh / f), out = new Float32Array(cw * ch);
  const src = MT.readWindow(map.H, i0, j0, cw * f, ch * f);
  for (let j = 0; j < ch; j++)
    for (let i = 0; i < cw; i++) {
      let sum = 0;
      for (let b = 0; b < f; b++) for (let a = 0; a < f; a++) sum += src[(j * f + b) * cw * f + i * f + a];
      out[j * cw + i] = sum / (f * f);
    }
  const sp = map.H.sp;
  return {rect: g.rect, f, grid: {w: cw, hh: ch, x0: g.grid.x0 + (f - 1) / 2 * sp, z0: g.grid.z0 + (f - 1) / 2 * sp, cell: sp * f, h: out}};
}
/* ---- THE PREVIEW OVERLAY: a coarse result drawn over its box (a range, a
 * template, a landform) before anything is written. */
let preview = null;        // {canvas, box: [x0, z0, x1, z1], label, owner} or null
/** Show coarse grid result r = {h, base?, coarse: {w, hh, x0, z0, cell}} as the
 *  preview; only cells that differ from `base` (when given) are drawn. */
function setPreview(r, label, owner) {
  if (!r) { preview = null; schedulePaint(); return; }
  const c = r.coarse, [lo, hi] = rampRange();
  let mask = null;
  if (r.base) { mask = new Uint8Array(c.w * c.hh); for (let n = 0; n < mask.length; n++) mask[n] = Math.abs(r.h[n] - r.base[n]) > 0.5 ? 1 : 0; }
  const rgba = TG.shade(r.h, c.w, c.hh, c.cell, {seaY: seaY(), lo: Math.min(lo, seaY()), hi, tint: 0.75, mask, zf: 1.4});
  const cv = document.createElement('canvas');
  cv.width = c.w; cv.height = c.hh;
  cv.getContext('2d').putImageData(new ImageData(rgba, c.w, c.hh), 0, 0);
  preview = {canvas: cv, box: [c.x0 - c.cell / 2, c.z0 - c.cell / 2, c.x0 + (c.w - 0.5) * c.cell, c.z0 + (c.hh - 0.5) * c.cell], label, owner};
  schedulePaint();
}
function paintPreview(ctx) {
  if (!preview) return;
  const [x0, z0, x1, z1] = preview.box;
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(preview.canvas, SX(x0), SZ(z0), (x1 - x0) * view.scale, (z1 - z0) * view.scale);
  ctx.imageSmoothingEnabled = false;
  ctx.strokeStyle = 'rgba(255,216,102,0.7)'; ctx.setLineDash([3, 3]); ctx.lineWidth = 1;
  ctx.strokeRect(SX(x0) + 0.5, SZ(z0) + 0.5, (x1 - x0) * view.scale, (z1 - z0) * view.scale);
  ctx.setLineDash([]);
  ctx.fillStyle = '#ffd866'; ctx.font = '11px monospace';
  ctx.fillText('PREVIEW: ' + (preview.label || ''), SX(x0) + 4, SZ(z0) + 13);
}
/** Write a job's post heights `h` (NaN / `changed` == 0 = leave) into the
 *  plane's rect as a PENDING result (in the plane, not yet an undo step):
 *  Commit keeps it (one undo step), Discard puts the posts back, the next
 *  edit of any kind commits it. `json` = a map.json snapshot to restore
 *  with it on undo (a generator's `gen` record). */
function writePending(rect, h, changed, label, json) {
  if (erosion.pending) commitErosion();
  const [i0, j0, w, hh] = rect;
  if (changed) for (let n = 0; n < h.length; n++) if (!changed[n]) h[n] = NaN;
  const before = new Map();
  const r = MT.writeValues(map.H, i0, j0, w, hh, h, before);
  erosion.pending = {before, rect, label, json: json || null};
  range.vals = null;
  syncHeightTools(); if (GL) GL.sync(); paint();
  return r;
}
/** Erode the Area (or, for stream power, the whole map): run in the worker,
 *  then hold the result as a PREVIEW in the plane until Commit (one undo
 *  step) or Discard (the posts as they were). */
async function runErosion() {
  if (!map || erosion.running) return null;
  const droplet = erosion.method === 'droplet';
  if (!area && droplet) { toast('draw an Area first: the Area tool, drag a rectangle (droplet erosion works on an area)', true); return null; }
  if (erosion.pending) commitErosion();
  const P = map.H, rect = areaRect(P), [i0, j0, w, h] = rect;
  if (w < 8 || h < 8) { toast('the area is under 8 posts a side', true); return null; }
  if (droplet && w * h > 4200000) { toast(`the area is ${w}×${h} posts; droplet erosion takes at most ~2048×2048 (draw a smaller area)`, true); return null; }
  if (rectBlocked(P, rect)) { toast('tiles under the area are still loading; try again in a moment', true); return null; }
  const job = {job: 'erode', method: erosion.method, seed: erosion.seed | 0, strength: erosion.strength / 100, iters: droplet ? erosion.iterations | 0 : null,
               talusDeg: erosion.talusDeg, hydraulic: erosion.hydraulic, thermal: erosion.thermal, seaY: seaY(),
               grid: {w, hh: h, x0: P.min[0] + i0 * P.sp, z0: P.min[1] + j0 * P.sp, cell: P.sp, h: MT.readWindow(P, i0, j0, w, h)}};
  let run;
  try { run = runJob(job, {label: (area ? 'eroding the area' : 'eroding the whole map') + ' (' + erosion.method + ')', onProgress: p => { erosion.progress = p; syncHeightTools(); }}); }
  catch (e) { toast(e.message, true); return null; }
  erosion.running = run; erosion.progress = 0;
  syncHeightTools();
  const t0 = performance.now();
  try {
    const res = await run;
    if (!map || map.H !== P || run.cancelled) return null;
    const r = writePending(rect, res.h, null, 'erosion');
    status(`erosion preview: ${r.changed} posts moved in ${((performance.now() - t0) / 1000).toFixed(1)} s (${erosion.method}, seed ${job.seed}) — Commit keeps it, Discard puts the posts back`);
    return res.h;
  } catch (e) {
    if (!run.cancelled) toast('erosion: ' + e.message, true);
    return null;
  } finally {
    erosion.running = null;
    syncHeightTools(); paint();
  }
}
function commitErosion() {
  const p = erosion.pending;
  if (!p) return;
  erosion.pending = null;
  if (p.before.size) { pushUndo(p.json ? {tiles: {height: p.before}, json: p.json} : {tiles: {height: p.before}}); markDirty(true); }
  syncHeightTools();
  if (GL) GL.sync();
}
function discardErosion() {
  const p = erosion.pending;
  if (!p) return;
  erosion.pending = null;
  MT.swapTiles(map.H, p.before);
  if (p.json) restore({json: p.json});
  range.vals = null;
  syncHeightTools(); if (GL) GL.sync(); paint();
}
/** The context the K1 layers get (maprange / mapgen / mapoverlay). */
function layerContext() {
  return {
    map: () => map, view, SX, SZ, worldAtF, worldColumnAt, el: H.el,
    pushUndo, snapshot, restore, markDirty, paint, schedulePaint, status, toast, vpm, seaY, fmtM,
    tool: () => tool, setTool: t => { tool = t; syncControls(); paint(); },
    registerTool: REG.registerTool,
    area: () => area, setArea: a => { area = a; syncHeightTools(); paint(); },
    areaRect: () => areaRect(map.H), rectBlocked: r => rectBlocked(map.H, r),
    postsGrid, coarseGrid, readPosts: (i0, j0, w, h) => MT.readWindow(map.H, i0, j0, w, h),
    runJob, jobBusy: () => !!mainJob, setPreview, preview: () => preview,
    writePending, pending: () => erosion.pending, commitPending: commitErosion, discardPending: discardErosion,
    planeChanged: () => { range.vals = null; },
    heightAt: (x, z) => MT.heightAt(map.H, x, z),
    rampRange, rampColor, disp, hover: () => els.hoverW || null,
    syncControls, imgCacheClear: () => imgCache.clear(),
    rebuildPalette, colorOf,
    check: () => check, savedJson: () => savedJson,
  };
}
function download(name, bytes, type) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([bytes], {type}));
  a.download = name;
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}
async function encodePng(w, h, format, body, note) {
  const r = await fetch(`/api/mappng/encode?w=${w}&h=${h}&format=${format}&note=${encodeURIComponent(JSON.stringify(note))}`, {method: 'POST', body});
  if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
  return new Uint8Array(await r.arrayBuffer());
}
/** The height of the Area (or the whole plane) as a 16-bit greyscale PNG:
 *  one unit = one voxel (0.1 m), offset = the lowest post, so the export is
 *  exact; the mapping and the post rectangle ride in the PNG's "sandvox" note. */
async function exportHeightPng(save = true) {
  const P = map.H, v = vpm(), sea = seaY(), [i0, j0, w, h] = areaRect(P);
  if (!w || !h) { toast('the area holds no posts', true); return null; }
  if (rectBlocked(P, [i0, j0, w, h])) { toast('tiles are still loading; export again in a moment', true); return null; }
  const vals = MT.readWindow(P, i0, j0, w, h);
  let lo = Infinity;
  for (let n = 0; n < vals.length; n++) lo = Math.min(lo, Math.round(vals[n]));
  const mpu = 1 / v, offsetM = Math.floor((lo - sea) / v), unit = mpu * v, off = sea + offsetM * v;
  const u = new Uint16Array(w * h);
  for (let n = 0; n < u.length; n++) u[n] = Math.max(0, Math.min(65535, Math.round((Math.round(vals[n]) - off) / unit)));
  const note = {sandvox: 'height', map: map.name, mpu, offsetM, seaLevelY: sea, vpm: v, rect: [i0, j0, w, h], posts: P.posts};
  const png = await encodePng(w, h, 'grey16', u, note);
  if (save) download(`${map.name}_height_${i0}_${j0}_${w}x${h}.png`, png, 'image/png');
  status(`exported the height of ${w}×${h} posts: 16-bit grey, 1 unit = ${mpu} m, 0 = ${offsetM} m above sea (${(png.length / 1024).toFixed(0)} KB)`);
  return png;
}
async function exportBiomePng(save = true) {
  const P = map.B, [i0, j0, w, h] = areaRect(P), pal = palette();
  if (!w || !h) { toast('the area holds no biome cells', true); return null; }
  const rgb = new Uint8Array(w * h * 3);
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const c = pal[MT.sample(P, i0 + i, j0 + j)] || pal[0], o = (j * w + i) * 3;
      rgb[o] = c[0]; rgb[o + 1] = c[1]; rgb[o + 2] = c[2];
    }
  const note = {sandvox: 'biome', map: map.name, rect: [i0, j0, w, h], biomes: map.json.biomes};
  const png = await encodePng(w, h, 'rgb8', rgb, note);
  if (save) download(`${map.name}_biome_${i0}_${j0}_${w}x${h}.png`, png, 'image/png');
  status(`exported the biomes of ${w}×${h} cells as palette colours (${(png.length / 1024).toFixed(0)} KB)`);
  return png;
}
/** The tuner's decoded-PNG blob ('SVPN', map_png.decoded_blob). */
function parseSVPN(buf) {
  const d = new DataView(buf);
  if (String.fromCharCode(d.getUint8(0), d.getUint8(1), d.getUint8(2), d.getUint8(3)) !== 'SVPN') throw new Error('not a decoded PNG');
  const w = d.getUint32(4, true), h = d.getUint32(8, true), fmt = d.getUint32(12, true), max = d.getUint32(16, true), nl = d.getUint32(20, true);
  const text = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 24, nl)).replace(/\0+$/, '') || '{}');
  let note = null;
  try { note = text.sandvox ? JSON.parse(text.sandvox) : null; } catch (e) { note = null; }
  const off = 24 + nl;
  const samples = fmt === 1 ? new Uint16Array(buf.slice(off, off + w * h * 2)) : new Uint8Array(buf.slice(off, off + w * h * 3));
  return {w, h, format: fmt === 1 ? 'grey' : 'rgb', max, samples, note};
}
/** Decode a PNG through the tuner and open the import dialog for `plane`. */
async function openPngImport(bytes, plane, fileName) {
  const r = await fetch('/api/mappng/decode', {method: 'POST', body: bytes});
  if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
  const img = parseSVPN(await r.arrayBuffer());
  if (plane === 'height' && img.format === 'rgb') {
    // a greyscale picture saved as RGB is fine; real colour is not a height
    const s = img.samples, g = new Uint16Array(img.w * img.h);
    for (let n = 0; n < g.length; n++) {
      if (s[n * 3] !== s[n * 3 + 1] || s[n * 3] !== s[n * 3 + 2]) throw new Error('a height import needs a greyscale PNG (this one has colour)');
      g[n] = s[n * 3];
    }
    img.samples = g; img.format = 'grey'; img.max = 255;
  }
  const n = img.note, own = n && n.sandvox === plane && Array.isArray(n.rect);
  pngIn = {plane, img, file: fileName || 'png',
           mpu: plane === 'height' ? (own && n.mpu > 0 ? n.mpu : 1500 / Math.max(1, img.max)) : 0,
           offsetM: own && plane === 'height' ? (+n.offsetM || 0) : 0,
           target: own ? 'note' : area ? 'area' : 'whole', keepUnmapped: true};
  if (plane === 'height') {
    let lo = 65535, hi = 0;
    for (let k = 0; k < img.samples.length; k++) { const s = img.samples[k]; if (s < lo) lo = s; if (s > hi) hi = s; }
    pngIn.lo = lo; pngIn.hi = hi;
  } else pngIn.map = biomeColourMap(img);
  syncHeightTools();
  return pngIn;
}
/** For a biome import: each distinct colour -> the nearest palette biome,
 *  and the colours too far from every palette colour to call ("unmapped"). */
function biomeColourMap(img) {
  const pal = palette(), counts = new Map(), s = img.samples;
  const grey = img.format === 'grey';
  for (let n = 0; n < img.w * img.h; n++) {
    const c = grey ? (Math.round(s[n] * 255 / Math.max(1, img.max)) * 0x10101) : (s[n * 3] << 16) | (s[n * 3 + 1] << 8) | s[n * 3 + 2];
    counts.set(c, (counts.get(c) || 0) + 1);
  }
  const to = new Map(), unmapped = [];
  for (const [c, cnt] of counts) {
    const r = c >> 16, g = (c >> 8) & 255, b = c & 255;
    let best = 0, bd = Infinity;
    pal.forEach((p, i) => { const d = (p[0] - r) ** 2 + (p[1] - g) ** 2 + (p[2] - b) ** 2; if (d < bd) { bd = d; best = i; } });
    const far = bd > 40 * 40;
    to.set(c, {index: best, far});
    if (far) unmapped.push({c, cnt, nearest: best});
  }
  unmapped.sort((a, b) => b.cnt - a.cnt);
  return {to, unmapped, colours: counts.size};
}
/** The post / cell rectangle the dialog's target names, or a reason it cannot. */
function importRect() {
  const P = pngIn.plane === 'height' ? map.H : map.B, n = pngIn.img.note;
  if (pngIn.target === 'note') {
    const r = n && n.rect;
    const nx = P.posts[0] - (P.name === 'biome' ? 1 : 0), nz = P.posts[1] - (P.name === 'biome' ? 1 : 0);
    if (!r || r[0] < 0 || r[1] < 0 || r[0] + r[2] > nx || r[1] + r[3] > nz) return 'the PNG’s own rectangle does not fit this map';
    return r;
  }
  if (pngIn.target === 'area' && !area) return 'no Area drawn';
  if (pngIn.target === 'whole') { const a = area; area = null; const r = areaRect(P); area = a; return r; }
  return areaRect(P);
}
/** Write the dialog's PNG into its plane: one undo step. Height: bilinear onto
 *  the target posts (exact when the sizes match), h = sea + (offset + unit *
 *  value) in metres, rounded to whole voxels. Biome: nearest pixel per cell,
 *  each colour to its nearest palette biome (unmapped colours left alone, or
 *  forced to their nearest). */
function applyPngImport() {
  if (!pngIn || !map) return null;
  const rect = importRect();
  if (typeof rect === 'string') { toast('import: ' + rect, true); return null; }
  const [i0, j0, tw, th] = rect, img = pngIn.img, W = img.w, Hh = img.h, s = img.samples;
  const P = pngIn.plane === 'height' ? map.H : map.B;
  if (!tw || !th) { toast('import: the target holds no samples', true); return null; }
  if (rectBlocked(P, rect)) { toast('tiles under the target are still loading; try again in a moment', true); return null; }
  const before = new Map();
  let r;
  if (pngIn.plane === 'height') {
    const v = vpm(), unit = pngIn.mpu * v, off = seaY() + pngIn.offsetM * v;
    const vals = new Float32Array(tw * th);
    for (let j = 0; j < th; j++) {
      const fy = th > 1 ? j * (Hh - 1) / (th - 1) : 0, y0 = Math.floor(fy), y1 = Math.min(Hh - 1, y0 + 1), ty = fy - y0;
      for (let i = 0; i < tw; i++) {
        const fx = tw > 1 ? i * (W - 1) / (tw - 1) : 0, x0 = Math.floor(fx), x1 = Math.min(W - 1, x0 + 1), tx = fx - x0;
        const a = s[y0 * W + x0] + (s[y0 * W + x1] - s[y0 * W + x0]) * tx;
        const b = s[y1 * W + x0] + (s[y1 * W + x1] - s[y1 * W + x0]) * tx;
        vals[j * tw + i] = Math.round(off + (a + (b - a) * ty) * unit);
      }
    }
    r = MT.writeValues(P, i0, j0, tw, th, vals, before);
  } else {
    const cm = pngIn.map, grey = img.format === 'grey';
    r = MT.writeWindow(P, i0, j0, tw, th, (n) => {
      const x = Math.min(W - 1, Math.floor(((n % tw) + 0.5) * W / tw)), y = Math.min(Hh - 1, Math.floor((Math.floor(n / tw) + 0.5) * Hh / th));
      const k = y * W + x;
      const c = grey ? Math.round(s[k] * 255 / Math.max(1, img.max)) * 0x10101 : (s[k * 3] << 16) | (s[k * 3 + 1] << 8) | s[k * 3 + 2];
      const e = cm.to.get(c);
      return !e || (e.far && pngIn.keepUnmapped) ? null : e.index;
    }, before);
  }
  if (before.size) { pushUndo({tiles: {[pngIn.plane]: before}}); markDirty(true); }
  range.vals = null;
  status(`imported ${pngIn.file} into ${tw}×${th} ${pngIn.plane === 'height' ? 'posts' : 'cells'}: ${r.changed} changed` +
         (r.skipped ? `, ${r.skipped} in loading tiles skipped` : '') + ' (Ctrl+Z takes it back)');
  pngIn = null;
  syncHeightTools(); paint();
  return r;
}
/** The Height tools panel: lines, the area + erosion, PNG import / export. */
function buildHeightToolsPanel() {
  const el = H.el;
  const det = el('details', {class: 'maptools', open: true});
  det.append(el('summary', {}, 'Height tools — ridge / valley lines, erosion, PNG'));
  els.lineList = el('div', {class: 'mapsites-list'});
  els.lineInsp = el('div', {class: 'maptools-panel'});
  det.append(el('div', {class: 'mapsub'}, 'Lines (map.json ridges[]: the plane holds what is BAKED; the engine never reads the lines)'), els.lineList, els.lineInsp);
  // the area + erosion
  els.areaInfo = el('div', {class: 'mapnote'});
  els.erIn = {};
  const num = (key, min, max, step, title, scale = 1) => {
    const i = el('input', {type: 'number', min, max, step, value: erosion[key], style: 'width:56px', title});
    i.addEventListener('change', () => { erosion[key] = Math.max(min, Math.min(max, +i.value || 0)); i.value = erosion[key]; });
    els.erIn[key] = i;
    return i;
  };
  const chk = (key, title) => {
    const c = el('input', {type: 'checkbox', title});
    c.checked = erosion[key];
    c.addEventListener('change', () => { erosion[key] = c.checked; });
    els.erIn[key] = c;
    return c;
  };
  els.erMethod = el('select', {title: 'landscape: STREAM POWER as a landscape-evolution run -- the land regrows under an uplift shaped like itself while rivers cut it, then its heights are put back: dendritic valleys reaching every divide (the Range tool\u2019s erosion); incise: stream power cutting the ground as it is (gentler, keeps its shape); droplet: particle water + thermal scree, gullies and fans (an Area only)'},
    el('option', {value: 'land'}, 'stream power: landscape'), el('option', {value: 'incise'}, 'stream power: incise'), el('option', {value: 'droplet'}, 'droplet + thermal'));
  els.erMethod.value = erosion.method;
  els.erMethod.addEventListener('change', () => { erosion.method = els.erMethod.value; syncHeightTools(); });
  els.erProg = el('progress', {max: 100, value: 0, style: 'width:120px;height:9px'});
  els.erRun = el('button', {class: 'mapgo', title: 'erode the Area (or, with stream power, the whole map when there is no Area) in a worker, deterministic for the seed, then preview it in the plane'}, 'Erode area');
  els.erRun.addEventListener('click', () => {
    if (erosion.running) { erosion.running.cancel(); erosion.running = null; syncHeightTools(); return; }
    runErosion();
  });
  els.erCommit = el('button', {title: 'keep the eroded posts (one undo step)'}, 'Commit');
  els.erCommit.addEventListener('click', () => { commitErosion(); paint(); });
  els.erDiscard = el('button', {title: 'put the posts back as they were'}, 'Discard');
  els.erDiscard.addEventListener('click', discardErosion);
  els.erState = el('span', {class: 'mapnote'});
  det.append(el('div', {class: 'mapsub'}, 'Area + erosion (the Area tool draws it; shift+click clears it)'), els.areaInfo,
    el('div', {class: 'mapbar'}, 'method ', els.erMethod),
    el('div', {class: 'mapgrid2'},
      el('label', {title: 'passes; each pass runs a droplet per four posts and a thermal step'}, 'passes ', num('iterations', 1, 100, 1, 'erosion passes')),
      el('label', {}, 'strength % ', num('strength', 1, 100, 5, 'how much each droplet carries and each slump moves')),
      el('label', {title: 'thermal: slopes steeper than this slump (scree)'}, 'talus ° ', num('talusDeg', 5, 80, 1, 'talus angle')),
      el('label', {title: 'the same seed on the same posts gives the same result'}, 'seed ', num('seed', 0, 999999, 1, 'seed')),
      el('label', {}, chk('hydraulic', 'droplets: gullies and fans'), ' water'),
      el('label', {}, chk('thermal', 'talus slumping: scree slopes'), ' thermal')),
    el('div', {class: 'mapbar'}, els.erRun, els.erCommit, els.erDiscard, els.erProg, els.erState));
  // PNG
  const exH = el('button', {title: 'the Area’s (or the whole map’s) HEIGHT as a 16-bit greyscale PNG: exact, 1 unit = 1 voxel, mapping in the file'}, 'Export height PNG');
  exH.addEventListener('click', () => exportHeightPng().catch(e => toast('export: ' + e.message, true)));
  const exB = el('button', {title: 'the Area’s (or the whole map’s) BIOMES as an RGB PNG in the palette colours'}, 'Export biome PNG');
  exB.addEventListener('click', () => exportBiomePng().catch(e => toast('export: ' + e.message, true)));
  const fileIn = (plane) => {
    const f = el('input', {type: 'file', accept: '.png,image/png', style: 'display:none'});
    f.addEventListener('change', async () => {
      const file = f.files && f.files[0];
      f.value = '';
      if (!file) return;
      try { await openPngImport(await file.arrayBuffer(), plane, file.name); } catch (e) { toast('import: ' + e.message, true); }
    });
    const b = el('button', {title: plane === 'height' ? 'a 16-bit (or 8-bit) greyscale heightmap from Gaea / World Machine / an export: you set metres per unit and the offset next' :
                                                      'an indexed or RGB PNG: each colour becomes the nearest palette biome (the dialog lists colours it could not call)'},
                  plane === 'height' ? 'Import height PNG…' : 'Import biome PNG…');
    b.addEventListener('click', () => f.click());
    return [b, f];
  };
  els.pngDlg = el('div', {class: 'maptools-panel'});
  det.append(el('div', {class: 'mapsub'}, 'PNG (the Area, or the whole map when there is none)'),
    el('div', {class: 'mapbar'}, exH, exB), el('div', {class: 'mapbar'}, ...fileIn('height'), ...fileIn('biome')), els.pngDlg);
  return det;
}
function syncHeightTools() {
  if (GL) GL.sync();
  if (!els.areaInfo || !map) return;
  const el = H.el, v = vpm();
  if (area) {
    const [, , w, h] = areaRect(map.H);
    els.areaInfo.textContent = `${fmtM((area[2] - area[0]) / v)} × ${fmtM((area[3] - area[1]) / v)} — ${w}×${h} height posts`;
  } else els.areaInfo.textContent = 'no area: stream power erodes the WHOLE map, droplet erosion needs one; PNG export / import use the whole map';
  const droplet = erosion.method === 'droplet';
  els.erMethod.value = erosion.method;
  els.erRun.textContent = erosion.running ? 'Cancel' : area ? 'Erode area' : 'Erode whole map';
  els.erRun.disabled = !area && droplet && !erosion.running;
  els.erCommit.disabled = els.erDiscard.disabled = !erosion.pending;
  els.erProg.style.display = erosion.running ? '' : 'none';
  els.erProg.value = Math.round(erosion.progress * 100);
  for (const k of ['iterations', 'hydraulic', 'thermal']) if (els.erIn && els.erIn[k]) els.erIn[k].parentNode.style.display = droplet ? '' : 'none';
  els.erState.textContent = erosion.running ? ` running ${Math.round(erosion.progress * 100)} %` :
    erosion.pending ? ' ' + (erosion.pending.label || 'erosion') + ' preview in the plane' : '';
  const d = els.pngDlg;
  d.replaceChildren();
  if (!pngIn) return;
  const img = pngIn.img;
  d.append(el('b', {}, `import ${pngIn.file} → ${pngIn.plane}`),
           el('span', {class: 'mapnote'}, `${img.w}×${img.h} ${img.format === 'grey' ? 'grey ' + (img.max > 255 ? '16' : '8') + '-bit' : 'colour'}` +
              (img.note ? ' · carries a sandvox note (' + img.note.sandvox + ', map ' + img.note.map + ')' : '')));
  const tgt = el('select', {});
  for (const [k, t] of [['note', 'the PNG’s own posts'], ['area', 'the Area'], ['whole', 'the whole map']])
    if (k !== 'note' || (img.note && img.note.sandvox === pngIn.plane)) tgt.append(el('option', {value: k}, t));
  tgt.value = pngIn.target;
  tgt.addEventListener('change', () => { pngIn.target = tgt.value; syncHeightTools(); });
  const rect = importRect();
  d.append(el('label', {}, 'onto ', tgt), el('span', {class: 'mapnote'}, typeof rect === 'string' ? rect :
           `${rect[2]}×${rect[3]} ${pngIn.plane === 'height' ? 'posts' : 'cells'} at (${rect[0]}, ${rect[1]})` +
           (rect[2] === img.w && rect[3] === img.h ? ' — same size: exact' : ' — resampled')));
  if (pngIn.plane === 'height') {
    const mpu = el('input', {type: 'number', step: 'any', value: pngIn.mpu, style: 'width:90px', title: 'metres per PNG unit (Gaea: the terrain height ÷ 65535)'});
    mpu.addEventListener('change', () => { pngIn.mpu = Math.max(1e-6, +mpu.value || 0); syncHeightTools(); });
    const off = el('input', {type: 'number', step: 'any', value: pngIn.offsetM, style: 'width:80px', title: 'metres above sea level of PNG value 0'});
    off.addEventListener('change', () => { pngIn.offsetM = +off.value || 0; syncHeightTools(); });
    d.append(el('label', {}, 'metres per unit ', mpu), el('label', {}, 'value 0 is at ', off, ' m above sea'),
             el('span', {class: 'mapnote'}, `values ${pngIn.lo}..${pngIn.hi} → ${fmtM(pngIn.offsetM + pngIn.lo * pngIn.mpu)} .. ${fmtM(pngIn.offsetM + pngIn.hi * pngIn.mpu)} above sea`));
  } else {
    const cm = pngIn.map, names = map.json.biomes;
    const keep = el('input', {type: 'checkbox'});
    keep.checked = pngIn.keepUnmapped;
    keep.addEventListener('change', () => { pngIn.keepUnmapped = keep.checked; });
    d.append(el('span', {class: 'mapnote'}, `${cm.colours} colour(s); ${cm.unmapped.length} unmapped (no palette colour within 40)`));
    for (const u of cm.unmapped.slice(0, 8))
      d.append(el('span', {class: 'mapwarn'}, `#${u.c.toString(16).padStart(6, '0')} × ${u.cnt} px — nearest ${names[u.nearest]}`));
    if (cm.unmapped.length) d.append(el('label', {}, keep, ' leave unmapped cells as they are (off: force them to the nearest biome)'));
  }
  const go = el('button', {class: 'mapgo'}, 'Apply import');
  go.addEventListener('click', applyPngImport);
  const no = el('button', {}, 'Cancel');
  no.addEventListener('click', () => { pngIn = null; syncHeightTools(); });
  d.append(el('div', {class: 'mapbar'}, go, no));
}

/* ---- UI --------------------------------------------------------------------------- */
function rebuildPalette() {
  const el = H.el;
  if (!els.palette || !map) return;
  els.palette.replaceChildren();
  map.json.biomes.forEach((name, i) => {
    const b = el('button', {class: 'mapchip' + (i === brush.index ? ' on' : ''), title: `paint ${name} (index ${i})`},
      el('span', {class: 'sw', style: `background:${colorOf(name)}`}), name);
    b.addEventListener('click', () => { brush.index = i; tool = 'biome'; syncControls(); rebuildPalette(); paint(); });
    els.palette.append(b);
  });
}
// the radius slider is logarithmic: 0..1000 -> 1 m .. 5 km
const rToSlider = m => Math.round(1000 * Math.log(m / R_MIN_M) / Math.log(R_MAX_M / R_MIN_M));
const sliderToR = v => R_MIN_M * Math.pow(R_MAX_M / R_MIN_M, v / 1000);
function syncControls() {
  if (!els.tools) return;
  // THE LOCALE PAGE (E4) has its own tool set; the world's tools are hidden
  // there (and Structure / Link are the locale's only)
  const inLocale = !!(LC && LC.open());
  const onPage = k => { const t = REG.get(k); return !!t && (inLocale ? t.locale : t.world); };
  if (!onPage(tool)) tool = inLocale ? 'sculpt' : REG.has('locale') && (tool === 'structure' || tool === 'waylink') ? 'locale' : 'height';
  for (const [k, b] of Object.entries(els.tools)) b.style.display = onPage(k) ? '' : 'none';
  for (const p of [els.vecPanel, els.htPanel, els.terrainPanel, els.palette, ...(els.worldPanels || [])]) if (p) p.style.display = inLocale ? 'none' : '';
  if (els.mapSel) els.mapSel.disabled = inLocale;   // leave the locale to switch maps
  if (els.legend) els.legend.sync(inLocale);
  if (els.guide) els.guide.sync(tool);
  for (const [k, b] of Object.entries(els.tools)) b.classList.toggle('on', k === tool);
  // per-tool option groups: only the active tool's
  for (const [k, g] of Object.entries(els.opts || {})) g.style.display = k.split(' ').includes(tool) ? '' : 'none';
  if (els.scMode) {
    els.scMode.value = sculpt.mode;
    els.scRadius.value = sculpt.radius; els.scRadiusOut.textContent = (sculpt.radius / 10).toFixed(1) + ' m';
    els.scStrength.value = sculpt.strength;
    els.scStrengthOut.textContent = (sculpt.mode === 'raise' || sculpt.mode === 'lower') ? (sculpt.strength / 5).toFixed(1) + ' vox/dab' : sculpt.strength + '%/dab';
  }
  els.brR.value = rToSlider(brush.radiusM); els.brROut.textContent = fmtM(brush.radiusM);
  els.brMode.value = brush.mode;
  els.brStr.value = brush.strength;
  els.brStrOut.textContent = (brush.mode === 'raise' || brush.mode === 'lower') ? '≈' + fmtM(brush.strength / 100 * brush.radiusM / 10) + '/dab' : brush.strength + '%';
  els.brTarget.value = brush.targetM;
  els.brTargetLbl.style.display = brush.mode === 'set' ? '' : 'none';
  els.brSpacing.value = brush.spacing;
  if (els.hbTerrace) {
    els.hbTerrace.style.display = brush.mode === 'terrace' ? '' : 'none';
    els.hbNoise.style.display = brush.mode === 'noise' ? '' : 'none';
    els.brFalloff.value = hb.falloff;
    // noise is stroke-stable at its amplitude; strength does not scale it
    els.brStr.style.display = els.brStrOut.style.display = brush.mode === 'noise' ? 'none' : '';
    for (const [k, i] of Object.entries(els.shIn)) i.value = shape[k];
    els.shKind.value = shape.kind;
    for (const [k, i] of Object.entries(els.lnIn)) i.value = lines[k];
    els.lnYSea.style.display = lines.yMode === 'sea' ? '' : 'none';
    els.lnYGround.style.display = lines.yMode === 'sea' ? 'none' : '';
  }
  if (els.mkKind) {
    els.mkKind.value = brush.mask.kind;
    els.mkBand.style.display = brush.mask.kind === 'height' || brush.mask.kind === 'slope' ? '' : 'none';
    els.mkUnit.textContent = brush.mask.kind === 'slope' ? '°' : ' m asl';
  }
  for (const t of REG.list()) if (t.sync) t.sync();
}

/* ---- 3D GO: teleport the preview's voxel camera to a map column -------------
 * Owner 2026-10-09: "a clear way to teleport to a selected location on the big
 * map". The 3D GO tool (view group): click the map and the voxel view jumps
 * there; T does the same at the cursor whatever tool is active. The 3D camera
 * is drawn on the map (a cyan eye + its heading) so you can see where it is.
 * The camera itself lives in tuner.html (wvTeleport / wvCameraOnMap). */
function teleportTo(wx, wz) {
  if (typeof window.wvTeleport !== 'function') { status('the 3D view is not available on this page'); return; }
  const g = groundAt(wx, wz);
  const y = g != null ? g : (map && map.H ? MT.heightAt(map.H, wx, wz) : null);
  window.wvTeleport(wx, wz, y == null ? null : Math.max(y, seaY()));
  status(`3D view → ${fmtM(wx / vpm())}, ${fmtM(wz / vpm())} (T teleports to the cursor with any tool)`);
  // the marker follows the camera for a moment while the view settles
  let n = 0; const tick = () => { paint(); if (++n < 20) setTimeout(tick, 100); }; tick();
}
function registerTeleport(C) {
  C.registerTool({
    key: 'go3d', label: '3D go', group: 'view', order: 20, locale: true,
    title: 'TELEPORT the 3D view: click a spot on the map and the voxel preview below jumps there (T does the same at the cursor with any tool)',
    down(ev, wx, wz) { teleportTo(wx, wz); return true; },
    statusAt: (wx, wz) => `click: put the 3D view at ${fmtM(wx / vpm())}, ${fmtM(wz / vpm())}`,
    anyKey(ev) {
      if (ev.ctrlKey || ev.metaKey || ev.altKey || (ev.key !== 't' && ev.key !== 'T') || !els.hoverW) return false;
      teleportTo(els.hoverW[0], els.hoverW[1]);
      return true;
    },
    cursor(ctx, wx, wz) {
      const x = C.SX(wx), z = C.SZ(wz);
      ctx.strokeStyle = '#7fe8ff'; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(x, z, 9, 0, Math.PI * 2); ctx.moveTo(x - 14, z); ctx.lineTo(x + 14, z); ctx.moveTo(x, z - 14); ctx.lineTo(x, z + 14); ctx.stroke();
    },
    paint(ctx) {
      const cam = typeof window.wvCameraOnMap === 'function' ? window.wvCameraOnMap() : null;
      if (!cam) return;
      const x = C.SX(cam.x), z = C.SZ(cam.z), l = Math.hypot(cam.dx, cam.dz) || 1;
      const hx = cam.dx / l, hz = cam.dz / l;
      ctx.save();
      ctx.fillStyle = 'rgba(127,232,255,0.22)';                 // the view cone
      ctx.beginPath(); ctx.moveTo(x, z);
      ctx.lineTo(x + (hx * 0.94 - hz * 0.34) * 40, z + (hz * 0.94 + hx * 0.34) * 40);
      ctx.lineTo(x + (hx * 0.94 + hz * 0.34) * 40, z + (hz * 0.94 - hx * 0.34) * 40);
      ctx.closePath(); ctx.fill();
      ctx.fillStyle = '#7fe8ff'; ctx.strokeStyle = '#0b1a22'; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(x, z, 6, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      ctx.font = '11px monospace'; ctx.fillStyle = '#bff4ff';
      ctx.fillText('3D', x + 9, z - 7);
      ctx.restore();
    },
    guide: ['3D go — teleport the voxel view',
      'Puts the 3D voxel preview (under the map) over the spot you pick, looking at it from a little back and above, keeping its heading. The cyan eye on the map is where the 3D camera is now, with its view cone.',
      'Click anywhere on the map. With ANY tool, hover a spot and press T. Then fly in the 3D view as usual (RMB look, WASD/QE).',
      'The 3D view shows the SAVED map (what the engine builds): save first to see edits. Turns the voxel view on if it was off.'],
  });
}
export function attach(hooks) {
  H = hooks;
  const root = H.section;
  if (!root) return;
  const el = H.el;
  const style = document.createElement('style');
  style.textContent = `
#env-map.active{display:flex;flex-direction:column;gap:6px;height:100%}
#env-map .mapbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;font-size:12px}
#env-map .mapbar button.on{background:#1f2a3d;border-color:#2b4a6f;color:#fff}
#env-map .mapopts{display:inline-flex;flex-wrap:wrap;gap:6px;align-items:center;padding:2px 6px;border-left:2px solid #2b4a6f}
#env-map .mapchip{display:inline-flex;align-items:center;gap:5px;padding:3px 7px;border:1px solid #2a3040;border-radius:4px;background:#12161f;color:#c7d2e3;cursor:pointer;font:11px monospace}
#env-map .mapchip.on{border-color:#ffd866;color:#fff}
#env-map .mapchip .sw{width:12px;height:12px;border-radius:2px;display:inline-block;image-rendering:pixelated}
#env-map .mapcanvas{flex:0 0 auto;height:max(800px, 80vh);min-height:800px;width:100%;background:#0e1219;border:1px solid #2a3040;border-radius:6px;cursor:crosshair;touch-action:none;image-rendering:pixelated}
#env-map .mapstatus{font:11px monospace;color:#8fa0b8;min-height:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#env-map .mapnote{font:10px/1.4 monospace;color:#6f7f97}
#env-map .mapbody{display:flex;gap:8px;min-height:0;flex:1}
#env-map .mapmain{flex:1;min-width:0;display:flex;flex-direction:column;gap:6px}
#env-map .mapside{width:340px;flex:0 0 340px;overflow-y:auto;font:11px monospace;color:#c7d2e3;display:flex;flex-direction:column;gap:8px}
#env-map details{border:1px solid #2a3040;border-radius:6px;padding:4px 8px;background:#12161f}
#env-map details summary{cursor:pointer;color:#dbe4f0;font-weight:600;font-size:11px}
#env-map .mapterrain-grid{display:grid;grid-template-columns:1fr;gap:2px;margin:6px 0}
#env-map .mapterrain-grid label{display:flex;justify-content:space-between;align-items:center;gap:6px;font-size:11px}
#env-map .mapunit{color:#6f7f97;width:44px}
#env-map .mapsites-list{display:flex;flex-direction:column;gap:2px;margin:6px 0}
#env-map .mapsite{display:flex;gap:5px;align-items:center;padding:2px 4px;border-radius:4px}
#env-map .mapsite.on{background:#1f2a3d}
#env-map .mapsite button{padding:1px 6px;font-size:10px}
#env-map .mapkind{font-size:9px;text-transform:uppercase;letter-spacing:.06em;width:58px;color:#8fa0b8}
#env-map .mapkind-water{color:#5fc8ff}
#env-map .mapkind-spawn{color:#8dff9a}
#env-map .mapkind-pad{color:#ffb454}
#env-map .mapkind-clearing{color:#c6f06a}
#env-map .mapkind-soften{color:#7ec8ff}
#env-map .mapkind-stamp{color:#7fd4ff}
#env-map .mapkind-tree{color:#6fe07a}
#env-map details.mapcheck summary.bad{color:#ffb454}
#env-map .mapcheck-list{display:flex;flex-direction:column;gap:3px;margin:6px 0}
#env-map .mapwarn{font:10px/1.4 monospace;color:#ffcf8a;border-left:2px solid #6a4a1a;padding-left:6px;overflow-wrap:anywhere}
#env-map .mapwarn.err{color:#ff8a8a;border-left-color:#7a2a2a}
#env-map .mapwarn.link{cursor:pointer}
#env-map .mapwarn.link:hover{background:#1f2a3d}
#env-map .mapid{width:110px;font:11px monospace;background:#0e1219;border:1px solid #2a3040;color:#dbe4f0;border-radius:3px;padding:1px 4px}
#env-map .mapwhere{flex:1;color:#8fa0b8;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#env-map .mapsites-panel{display:flex;flex-direction:column;gap:4px;border-top:1px solid #2a3040;padding-top:6px}
#env-map .mapsites-panel label{display:flex;justify-content:space-between;align-items:center;gap:6px}
#env-map .maptools-panel{display:flex;flex-direction:column;gap:4px;border-top:1px solid #2a3040;padding-top:6px}
#env-map .maptools-panel label{display:flex;justify-content:space-between;align-items:center;gap:6px}
#env-map #wgPreviewHost{border-top:1px solid #2a3040;padding-top:6px}
#env-map .mapsub{font:10px monospace;color:#8fa0b8;text-transform:uppercase;letter-spacing:.05em;margin-top:6px;border-top:1px solid #2a3040;padding-top:4px}
#env-map .mapgrid2{display:grid;grid-template-columns:1fr 1fr;gap:2px 8px;margin:4px 0}
#env-map .mapgrid2 label{display:flex;justify-content:space-between;align-items:center;gap:4px}
#env-map .maplinepts{display:flex;flex-direction:column;gap:1px;max-height:160px;overflow-y:auto}
#env-map .mapprofile{width:300px;height:84px;image-rendering:pixelated;border:1px solid #2a3040;border-radius:3px}
#env-map .mapkind-road{color:#c8b48a}
#env-map .mapkind-region{color:#c9a227}
#env-map .mapkind-marker{color:#e8dcc0;display:inline-flex;align-items:center;gap:3px}
#env-map .mapicons{display:inline-flex;flex-wrap:wrap;gap:2px}
#env-map button.mapicon{padding:1px;line-height:0;background:#0e1219;border:1px solid #2a3040;border-radius:2px}
#env-map button.mapicon.on{border-color:#ffd866}
#env-map .mapicon canvas,#env-map canvas.mapiconsm{image-rendering:pixelated}
#env-map .mapprofile-road{width:320px;height:120px}
#env-map .mapnote.bad{color:#ffcf8a}
#env-map button.mapgo{background:#2a2410;border-color:#6a5a1a;color:#ffd866}
#env-map button.mapgo:disabled,#env-map .maptools button:disabled{opacity:.45}
#env-map .mapkind-locale{color:#7fd4ff;display:inline-flex;align-items:center;gap:3px}
#env-map .mapcrumb{font:12px monospace;color:#cfeeff;padding:2px 4px;border:1px solid #2b4a6f;border-radius:4px;background:#12161f}
#env-map .mapcrumb a{color:#7fd4ff}
#env-map .mapcrumb canvas{image-rendering:pixelated;vertical-align:middle}
#env-map .maplocale3d{display:flex;flex-direction:column;gap:4px;flex:1.6 1 0;min-height:440px}
#env-map .maplocale3d-canvas{flex:1;min-height:360px;width:100%;background:#0e1116;border:1px solid #2a3040;border-radius:6px;outline:none;touch-action:none}
#env-map .maplocale3d.max{position:fixed;inset:0;z-index:1000;background:#0b0e13;padding:8px;min-height:0}
#env-map .maplocale3d-help{font-size:11px;color:#7d8798}
#env-map .maptoolsep{display:inline-block;width:1px;height:18px;background:#2a3040;margin:0 3px}
#env-map .mapjob{border:1px solid #6a5a1a;border-radius:4px;padding:2px 6px;background:#1a1608}
#env-map .mapkind-range{color:#e8a060}
`;
  document.head.append(style);

  els.canvas = el('canvas', {class: 'mapcanvas'});
  els.status = el('div', {class: 'mapstatus'}, 'no map loaded');
  // E3: the vector layers, handed the page (mapvector.js createVectorLayer)
  VL = VEC.createVectorLayer({
    map: () => map, view, SX, SZ, worldAtF,
    fitBox: (x0, z0, x1, z1) => {
      const c = els.canvas, vmin = Math.min(c.clientWidth || 600, c.clientHeight || 400);
      const span = Math.max(x1 - x0, z1 - z0, 64), onScreen = span * view.scale;
      if (onScreen < 40 || onScreen > 0.9 * vmin) view.scale = Math.min(4, 0.5 * vmin / span);
      view.cx = (x0 + x1) / 2; view.cz = (z0 + z1) / 2;
      scheduleRealRefresh();
    },
    pushUndo, snapshot, markDirty, paint, status, toast, el,
    vpm, seaY, check: () => check, savedJson: () => savedJson,
    tool: () => tool, setTool: t => { tool = t; syncControls(); paint(); },
    hover: () => els.hoverW || null,
    materials: () => (H.materials ? H.materials() : null),
    heightAt: (x, z) => MT.heightAt(map.H, x, z),
  });
  // E4: the locales, handed the page (maplocale.js createLocaleLayer)
  LC = LOC.createLocaleLayer({
    map: () => map, view, SX, SZ, worldAtF,
    // centre on a box; `soft` zooms only when it would be tiny or not fit
    // (a list click), else it fills ~90 % of the canvas (opening a locale)
    fitBounds: (x0, z0, x1, z1, soft) => {
      const c = els.canvas, W = c.clientWidth || 600, Hh = c.clientHeight || 400;
      const span = Math.max(x1 - x0, z1 - z0, 32), onScreen = span * view.scale;
      if (!soft || onScreen < 40 || onScreen > 0.9 * Math.min(W, Hh))
        view.scale = Math.max(1e-4, Math.min(16, 0.9 * Math.min(W / Math.max(1, x1 - x0), Hh / Math.max(1, z1 - z0))));
      view.cx = (x0 + x1) / 2; view.cz = (z0 + z1) / 2;
      scheduleRealRefresh();
    },
    pushUndo, snapshot, markDirty, paint, status, toast, el,
    vpm, seaY, check: () => check, savedJson: () => savedJson,
    tool: () => tool, setTool: t => { tool = t; syncControls(); paint(); },
    hover: () => els.hoverW || null,
    groundAt: (x, z) => groundAt(x, z),
    planeAt: (x, z) => MT.heightAt(map.H, x, z),
    refreshHeights: () => fetchHeights().catch(e => status('locale heights failed: ' + e.message)),
    previewSeed: () => (H.previewSeed ? H.previewSeed() : 1337),
    modeChanged: () => { syncControls(); syncSites(); paint(); },
    isVisible: () => (H.isVisible ? H.isVisible() : true),
    saved: () => { if (H.saved) H.saved(); },
  });
  els.palette = el('div', {class: 'mapbar'});
  els.mapSel = el('select', {});
  els.save = el('button', {disabled: true}, 'Save map');
  els.save.addEventListener('click', saveMap);
  // ---- the plane brushes (height + biome): one radius in metres, log slider
  els.brR = el('input', {type: 'range', min: 0, max: 1000, step: 1, style: 'width:130px', title: 'brush radius, 1 m .. 5 km (logarithmic; [ and ])'});
  els.brROut = el('span', {});
  els.brR.addEventListener('input', () => { brush.radiusM = sliderToR(+els.brR.value); syncControls(); paint(); });
  els.brMode = el('select', {title: 'raise / lower: move the posts up or down; smooth: toward the local average; flatten: toward the height where the stroke started; set: toward a typed height (m above sea); terrace: quantise into shelves and cliffs; noise: seeded relief (amplitude in m); erode: water + scree erosion under the brush'});
  for (const m of MT.HEIGHT_MODES) els.brMode.append(el('option', {value: m}, m));
  els.brMode.addEventListener('change', () => { brush.mode = els.brMode.value; tool = 'height'; syncControls(); });
  els.brStr = el('input', {type: 'range', min: 1, max: 100, style: 'width:90px', title: 'strength: raise / lower move the centre strength% of radius/10 per dab; set / smooth go strength% of the way'});
  els.brStrOut = el('span', {});
  els.brStr.addEventListener('input', () => { brush.strength = +els.brStr.value; syncControls(); });
  els.brTarget = el('input', {type: 'number', step: 1, style: 'width:70px', title: 'Set: the height in metres ABOVE SEA LEVEL (Alt+click picks it)'});
  els.brTarget.addEventListener('change', () => { brush.targetM = +els.brTarget.value || 0; syncControls(); });
  els.brTargetLbl = el('label', {}, ' to ', els.brTarget, ' m');
  els.brSpacing = el('input', {type: 'number', min: 5, max: 200, step: 5, style: 'width:50px', title: 'dab spacing along a drag, % of the radius'});
  els.brSpacing.addEventListener('change', () => { brush.spacing = Math.max(5, Math.min(200, +els.brSpacing.value || 25)); syncControls(); });
  // ---- E2: the height brush's falloff and its per-mode options
  const numIn = (obj, key, min, max, step, width, title, after) => {
    const i = el('input', {type: 'number', min, max, step, value: obj[key], style: `width:${width}px`, title});
    i.addEventListener('change', () => { obj[key] = Math.max(min, Math.min(max, +i.value || 0)); i.value = obj[key]; if (after) after(); paint(); });
    return i;
  };
  const selIn = (obj, key, opts, title, after) => {
    const s = el('select', {title}, ...opts.map(o => el('option', {value: o}, o)));
    s.value = obj[key];
    s.addEventListener('change', () => { obj[key] = s.value; if (after) after(); paint(); });
    return s;
  };
  els.hbIn = {};
  els.brFalloff = selIn(hb, 'falloff', HS.FALLOFF_NAMES, 'the brush edge: smooth (cosine), linear, flat (a hard core, a short soft rim), sphere');
  els.hbTerrace = el('span', {}, ' step ', (els.hbIn.stepM = numIn(hb, 'stepM', 1, 2000, 5, 56, 'terrace band height, metres (from the sea)')), ' m sharp ',
                     (els.hbIn.sharp = numIn(hb, 'sharp', 0, 98, 5, 46, 'sharpness %: 0 soft steps; 90 = 90 % flat shelf and a 10 % cliff')), '%');
  els.hbNoise = el('span', {}, ' amp ', (els.hbIn.ampM = numIn(hb, 'ampM', 0, 3000, 5, 56, 'noise amplitude, metres (at strength 100 % in the core)')), ' m size ',
                   (els.hbIn.sizeM = numIn(hb, 'sizeM', 5, 20000, 50, 62, 'the largest noise feature, metres')), ' m ',
                   (els.hbIn.style = selIn(hb, 'style', ['ridged', 'fbm'], 'ridged = sharp crests (mountain ranges); fbm = rolling relief')),
                   ' seed ', (els.hbIn.seed = numIn(hb, 'seed', 0, 999999, 1, 64, 'the same seed paints the same noise')));
  // ---- E2: the Shape tool
  els.shKind = selIn(shape, 'kind', HS.SHAPE_NAMES, 'peak / ridge / basin / volcano ride on the land (additive); plateau / mesa level it to a flat top',
                     () => { shape.aspect = shape.kind === 'ridge' ? 0.25 : 1; syncControls(); });
  els.shIn = {
    heightM: numIn(shape, 'heightM', 0, 4000, 10, 60, 'rise (or depth) in metres, or the summit / floor height above sea'),
    mode: selIn(shape, 'mode', ['rise', 'summit'], 'rise: that far above (basin: below) the land at the centre; summit: that many metres above sea level (a basin’s floor)'),
    radiusM: numIn(shape, 'radiusM', 10, 20000, 50, 64, 'radius in metres ([ and ])'),
    aspect: numIn(shape, 'aspect', 0.05, 1, 0.05, 50, 'width / length: 1 round, 0.25 a long ridge'),
    rot: numIn(shape, 'rot', 0, 359, 15, 50, 'rotation of the long axis, degrees (, and .)'),
  };
  // ---- E2: the Line tool (what a NEW line / vertex takes)
  els.lnIn = {
    kind: selIn(lines, 'kind', ['ridge', 'valley'], 'a NEW line: ridge (raises toward its crest) or valley (lowers toward its floor)'),
    profile: selIn(lines, 'profile', HS.PROFILE_NAMES, 'a NEW line’s cross-section'),
    widthM: numIn(lines, 'widthM', 5, 20000, 50, 62, 'a NEW line’s half-width, metres ([ and ])'),
    yMode: selIn(lines, 'yMode', ['sea', 'ground'], 'a new VERTEX’s height: sea = metres above sea level; ground = the plane under it plus (ridge) / minus (valley) the rise', () => syncControls()),
    yM: numIn(lines, 'yM', -2000, 4000, 10, 60, 'new vertices at this height, metres above sea'),
    dyM: numIn(lines, 'dyM', 0, 4000, 10, 60, 'new vertices this far above (ridge) / below (valley) the plane'),
  };
  // ---- site tools
  els.waterPreset = el('select', {title: 'the water preset a new lake wears (Environment > Water bodies)'});
  els.waterPreset.addEventListener('change', () => { water.preset = els.waterPreset.value; tool = 'water'; syncControls(); paint(); });
  els.waterRadius = el('input', {type: 'number', min: 0, step: 0.5, value: 0, style: 'width:64px', title: 'radius in metres for a NEW lake; 0 = the preset\'s own footprint radius'});
  els.waterRadius.addEventListener('change', () => { water.radiusM = Math.max(0, +els.waterRadius.value || 0); });
  els.stampTpl = el('select', {title: 'the assets/prefabs/<name>.vox a NEW stamp site places'});
  els.stampTpl.addEventListener('change', () => { stamp.template = els.stampTpl.value; tool = 'stamp'; syncControls(); paint(); });
  els.stampRot = el('select', {title: 'quarter turns for a NEW stamp site'});
  for (let i = 0; i < 4; i++) els.stampRot.append(el('option', {value: String(i)}, (i * 90) + '°'));
  els.stampRot.addEventListener('change', () => { stamp.rot = (+els.stampRot.value) & 3; });
  els.treeSp = el('select', {title: 'the species a NEW tree site plants (assets/trees/<name>.svtree)'});
  els.treeSp.addEventListener('change', () => { trees.species = els.treeSp.value; tool = 'tree'; syncControls(); paint(); });
  els.treeVar = el('input', {type: 'number', min: -1, max: 63, step: 1, value: -1, style: 'width:48px', title: 'variant for a NEW tree; -1 = rolled from the seed'});
  els.treeVar.addEventListener('change', () => { trees.variant = Math.max(-1, Math.round(+els.treeVar.value || 0)); });
  els.treeRot = el('select', {title: 'quarter turns for a NEW tree; rolled = from the seed (with a mirror)'});
  els.treeRot.append(el('option', {value: '-1'}, 'rolled'));
  for (let i = 0; i < 4; i++) els.treeRot.append(el('option', {value: String(i)}, (i * 90) + '°'));
  els.treeRot.addEventListener('change', () => { trees.rot = +els.treeRot.value; });
  els.clearFeather = el('input', {type: 'number', min: 0, max: 512, step: 8, value: clearing.feather, style: 'width:56px', title: 'feather in voxels for a NEW clearing'});
  els.clearFeather.addEventListener('change', () => { clearing.feather = Math.max(0, Math.min(512, Math.round(+els.clearFeather.value || 0))); els.clearFeather.value = clearing.feather; });
  // ---- the sculpt brush (P5)
  els.scMode = el('select', {title: 'raise / lower: add or remove height; smooth: pull toward the local average of the GROUND (the layer alone where no heights are loaded); flatten: level to the ground height under the cursor where the stroke started (needs Real terrain); erase: back toward the generated ground'});
  for (const m of SC_MODES) els.scMode.append(el('option', {value: m}, m));
  els.scMode.addEventListener('change', () => { sculpt.mode = els.scMode.value; tool = 'sculpt'; syncControls(); paint(); });
  els.scRadius = el('input', {type: 'range', min: 8, max: 2048, step: 8, value: sculpt.radius, style: 'width:110px', title: 'sculpt brush radius ([ and ] with the Sculpt tool)'});
  els.scRadiusOut = el('span', {});
  els.scRadius.addEventListener('input', () => { sculpt.radius = +els.scRadius.value; syncControls(); paint(); });
  els.scStrength = el('input', {type: 'range', min: 1, max: 100, value: sculpt.strength, style: 'width:90px', title: 'raise / lower: voxels per dab at the centre (strength / 5); smooth / flatten / erase: the fraction of the way per dab'});
  els.scStrengthOut = el('span', {});
  els.scStrength.addEventListener('input', () => { sculpt.strength = +els.scStrength.value; syncControls(); });
  const scShow = el('input', {type: 'checkbox', checked: true, title: 'draw the sculpt layer over the map (warm = raised, cool = lowered)'});
  scShow.addEventListener('change', () => { sculpt.show = scShow.checked; paint(); });
  // ---- the display
  const chk = (key, title, on) => {
    const c = el('input', {type: 'checkbox', title});
    c.checked = disp[key];
    c.addEventListener('change', () => { disp[key] = c.checked; if (on) on(); paint(); });
    return c;
  };
  els.contourSel = el('select', {title: 'contour interval: auto follows the zoom; every 5th line is bold; the 0 m line is the coast'});
  for (const [v, t] of [[0, 'auto'], [10, '10 m'], [25, '25 m'], [50, '50 m'], [100, '100 m']]) els.contourSel.append(el('option', {value: String(v)}, t));
  els.contourSel.addEventListener('change', () => { disp.interval = +els.contourSel.value; paint(); });
  const pct = (key, title) => {
    const r = el('input', {type: 'range', min: 0, max: 100, value: Math.round(disp[key] * 100), style: 'width:70px', title});
    r.addEventListener('input', () => { disp[key] = +r.value / 100; paint(); });
    return r;
  };
  els.heightsBtn = el('button', {title: 'the ENGINE’s own ground (--heightmap of the SAVED files: authored height + noise + sculpt + sites) over the plane at the opacity beside it; zoomed in it follows the view. Click again to hide.'}, 'Real terrain');
  els.heightsBtn.addEventListener('click', () => {
    const seed = H.previewSeed ? H.previewSeed() : 1337;
    const current = heights && heights.gen === savedGen && heights.seed === seed && map && heights.key === heightsRequest().key;
    if (showHeights && current) { showHeights = false; range.vals = null; els.heightsBtn.classList.remove('on'); paint(); return; }
    fetchHeights().catch(e => { toast('heights: ' + e.message, true); status('heights failed: ' + e.message); });
  });
  // The map the GAME loads is tuning.json world.mapLayer (the map selector
  // writes tuning through the host). The EDIT LAYER is the map's own
  // (map.json `editLayer`, map-overhaul P7).
  els.editSel = el('select', {title: 'assets/worldedits/<name>.svedit, applied over this map’s worldgen (map.json editLayer); blank = none'});
  els.editSel.addEventListener('change', () => {
    if (!map) return;
    pushUndo(snapshot());
    if (els.editSel.value) map.json.editLayer = els.editSel.value; else delete map.json.editLayer;
    markDirty();
    toast('map.json editLayer = ' + (els.editSel.value || '(none)') + ' — save the map and press F7 in the game');
  });
  const fitBtn = el('button', {}, 'Fit');
  fitBtn.addEventListener('click', () => { fit(); paint(); });
  const undoBtn = el('button', {title: 'Ctrl+Z'}, 'Undo');
  undoBtn.addEventListener('click', doUndo);
  const redoBtn = el('button', {title: 'Ctrl+Y'}, 'Redo');
  redoBtn.addEventListener('click', doRedo);

  const bar = el('div', {class: 'mapbar'},
    el('label', {title: 'the map the GAME loads (tuning.json world.mapLayer)'}, 'map ', els.mapSel),
    el('label', {title: 'the edit layer this map applies over its worldgen (map.json editLayer)'}, ' edits ', els.editSel),
    els.save, undoBtn, redoBtn, fitBtn,
    el('span', {style: 'width:10px'}),
    el('label', {title: 'the topographic layer: hypsometric colours over the real min..max of the height plane'}, chk('topo', 'topographic layer'), 'topo'),
    el('label', {}, chk('shade', 'hillshade from the north-west'), 'shade'),
    el('label', {}, chk('contours', 'contour lines'), 'contours ', els.contourSel),
    el('label', {title: 'biome overlay opacity (full when the topo layer is off)'}, ' biomes ', pct('biomeAlpha', 'biome overlay opacity')),
    els.heightsBtn, pct('realAlpha', 'real-terrain backdrop opacity'),
    el('label', {}, ' ', scShow, 'sculpt'));
  els.dispBar = bar;
  // ---- the brush MASK (every height brush mode): land only / sea only / a
  // height band / a slope band, soft-edged
  els.mkKind = el('select', {title: 'MASK: where the Height brush may act -- everywhere, land only (above the sea), sea only, a height band (m above sea) or a slope band (degrees); soft at the edges'},
                  ...MT.MASK_KINDS.map(k => el('option', {value: k}, k === 'none' ? 'no mask' : k === 'land' ? 'land only' : k === 'sea' ? 'sea only' : k + ' band')));
  els.mkKind.addEventListener('change', () => { brush.mask.kind = els.mkKind.value; syncControls(); });
  const mkNum = (key, title) => {
    const i = el('input', {type: 'number', step: 5, value: brush.mask[key], style: 'width:56px', title});
    i.addEventListener('change', () => { brush.mask[key] = +i.value || 0; });
    els['mk_' + key] = i;
    return i;
  };
  els.mkUnit = el('span', {}, ' m asl');
  els.mkBand = el('span', {}, ' ', mkNum('lo', 'the band\u2019s low end (m above sea, or degrees)'), '..', mkNum('hi', 'the band\u2019s high end (m above sea, or degrees)'), els.mkUnit,
                  ' soft ', mkNum('soft', 'the soft edge (m, or degrees)'));
  // ---- every tool's options strip, then THE REGISTRY
  els.optHeight = el('span', {class: 'mapopts'}, 'mode ', els.brMode, ' str ', els.brStr, ' ', els.brStrOut, els.brTargetLbl, ' edge ', els.brFalloff, els.hbTerrace, els.hbNoise,
                     ' mask ', els.mkKind, els.mkBand);
  els.optShape = el('span', {class: 'mapopts'}, 'shape ', els.shKind, ' ', els.shIn.heightM, ' m ', els.shIn.mode, ' r ', els.shIn.radiusM, ' m aspect ', els.shIn.aspect, ' rot ', els.shIn.rot, '°');
  els.optLine = el('span', {class: 'mapopts'}, 'new ', els.lnIn.kind, ' ', els.lnIn.profile, ' half-width ', els.lnIn.widthM, ' m · vertex ', els.lnIn.yMode,
                   (els.lnYSea = el('span', {}, ' ', els.lnIn.yM, ' m asl')), (els.lnYGround = el('span', {}, ' ± ', els.lnIn.dyM, ' m')));
  els.optArea = el('span', {class: 'mapopts'}, el('span', {class: 'mapnote'}, 'drag a rectangle; erode it, generate into it or export / import it (Height tools, Generate panels)'));
  els.optSculpt = el('span', {class: 'mapopts'}, 'sculpt ', els.scMode, ' r ', els.scRadius, ' ', els.scRadiusOut, ' str ', els.scStrength, ' ', els.scStrengthOut);
  els.optWater = el('span', {class: 'mapopts'}, 'lake ', els.waterPreset, ' r ', els.waterRadius, ' m');
  els.optStamp = el('span', {class: 'mapopts'}, 'stamp ', els.stampTpl, ' ', els.stampRot);
  els.optTree = el('span', {class: 'mapopts'}, 'tree ', els.treeSp, ' v ', els.treeVar, ' ', els.treeRot);
  els.optClearing = el('span', {class: 'mapopts'}, 'clearing feather ', els.clearFeather, ' vox');
  registerBuiltinTools();
  // K1: the range splines, the generators, the overlays + Measure (each registers its tools)
  const C = layerContext();
  RL = createRangeLayer(C);
  GL = createGenLayer(C);
  OL = createOverlayLayer(C);
  CLM = createClimateLayer(C);
  CST = createCoastLayer(C);
  WL = createWaterLayer(C);
  registerTeleport(C);
  els.dispBar.append(el('span', {style: 'width:6px'}), ...OL.displayControls());
  els.tools = {};
  const toolbar = el('div', {class: 'mapbar'});
  let lastGroup = null;
  for (const t of REG.list()) {
    if (lastGroup && t.group !== lastGroup) toolbar.append(el('span', {class: 'maptoolsep'}));
    lastGroup = t.group;
    const b = el('button', {title: t.title || t.label}, t.label);
    b.addEventListener('click', () => { tool = t.key; syncControls(); paint(); });
    els.tools[t.key] = b;
    toolbar.append(b);
  }
  els.opts = {'height biome': el('span', {class: 'mapopts'}, 'radius ', els.brR, ' ', els.brROut, el('label', {}, ' spacing ', els.brSpacing, '%'))};
  for (const t of REG.list()) { const o = t.opts ? t.opts() : null; if (o) els.opts[t.key] = o; }
  toolbar.append(...Object.values(els.opts));
  const note = el('div', {class: 'mapnote'},
    'The HEIGHT plane is the world’s shape, in metres: a post every ' + '6.4 m (map.json planes.height.log2), exact and never scaled by noise; the seeded noise (Terrain) rides on it and the Sculpt layer (0.8 m) on top of that. ' +
    'The BIOME plane says what grows where (planes.biome.log2: 25.6 m cells). Right/middle-drag pans, wheel zooms, [ ] resize the brush, Alt+click picks. ' +
    'Range draws a MOUNTAIN RANGE as a spline (crest height + half-width per knot) and bakes it eroded; Shape stamps landforms; Line draws ridge / valley lines you Bake into the plane; Area marks a rectangle to erode, generate into or export / import as PNG (Height tools and Generate panels). Measure reads distance, the elevation profile and the grade. ' +
    'Road draws graded roads the engine builds (vertices snap together into junctions; the Vector layers panel shows the engine’s baked profile once saved); Region outlines named areas; Marker pins pixel-art annotations. ' +
    'Sites: the pad box (calm bare ground; the harness map’s is the selftest’s), the spawn diamond, stamps, authored lakes, authored trees, forest clearings, softened ground; they keep out by their own footprint. The Load check panel says what the engine thinks of the saved map. ' +
    'Saving writes map.json and only the plane tiles you changed (assets/worldmap/<name>/planes/), and moves the world hash; regenerate the world (F7 in the game, or Apply in the sidebar) to see it.');
  els.bar = bar;
  els.guide = buildToolGuide(el, REG.guides());
  style.textContent += els.guide.css;
  els.jobBar = buildJobBar();
  const main = el('div', {class: 'mapmain'}, LC.buildBreadcrumb(), bar, toolbar, els.jobBar, els.guide.panel, els.palette, els.canvas, LC.build3d(), els.status, note);
  els.vecPanel = VL.buildPanel();
  els.htPanel = buildHeightToolsPanel();
  els.terrainPanel = buildTerrainSection();
  els.worldPanels = [RL.buildPanel(), GL.buildPanel(), CLM.buildPanel(), WL.buildPanel()];
  els.legend = buildLegend(el, VEC.iconCanvas, {land: LAND_LUT, sea: SEA_LUT});
  style.textContent += els.legend.css;
  const side = el('div', {class: 'mapside'}, els.legend.panel, buildCheckPanel(), LC.buildPanel(), els.worldPanels[0], els.worldPanels[1], els.worldPanels[2], els.vecPanel, els.htPanel, els.terrainPanel, buildSitesPanel());
  root.append(el('div', {class: 'mapbody'}, main, side));
  // The PREVIEW pane (P-I): the heightmap and voxel views tuner.html keeps in
  // #wgPreviewHost, adopted under the map as previews.
  const pv = document.getElementById('wgPreviewHost');
  if (pv) {
    const head = el('div', {class: 'mapbar'},
      el('b', {}, 'Preview'),
      el('span', {class: 'mapnote'}, ' the engine’s heights (--heightmap) or its real voxels (--voxserve) for the SAVED files: previews, not authoring. ' +
         'Regen note: the game re-reads the map, the biomes and the trees on F7 / Apply; the Plants button cycles the two dev A/B switches (tuning.json debug.groundCover, debug.vegetation) through ALL / no ground cover / none.'));
    const pvToggle = el('button', {}, 'Hide preview');
    pvToggle.addEventListener('click', () => {
      const on = pv.style.display === 'none';
      pv.style.display = on ? '' : 'none';
      pvToggle.textContent = on ? 'Hide preview' : 'Show preview';
      if (on && window.wgPreviewShow) window.wgPreviewShow();
    });
    head.prepend(pvToggle);
    main.append(head, pv);
    // Owner, 2026-10-09: the preview is OPEN by default and in its VOXEL view.
    // The voxel view is switched on the first time the page is shown
    // (activate), not here: attach runs at tuner load, and the voxel view
    // starts a --voxserve process the other tabs have no use for.
    pv.style.display = '';
    previewPending = true;
  }
  wire();
  new ResizeObserver(() => paint()).observe(els.canvas);

  els.mapSel.addEventListener('change', () => {
    mapLayerWanted = els.mapSel.value;
    pushTuning();
    openMap(els.mapSel.value);
  });
  refreshEditLayers();
  loadTerrainDefaults().then(() => { if (map) { terrainOf(); syncTerrain(); publishTerrain(); } })
    .catch(e => toast('terrain defaults: ' + e.message + ' (a map missing terrain keys shows them blank)', true));
  loadWaterPresets().then(() => paint()).catch(e => status('water presets: ' + e.message));
  loadStampTemplates().catch(e => status('stamp templates: ' + e.message));
  loadTreeSpecies().then(() => { if (selected && selected.kind === 'tree') syncSites(); })
    .catch(e => status('tree species: ' + e.message));
  listMaps().then(names => {
    els.mapSel.replaceChildren(...names.map(n => el('option', {value: n}, n)));
    const want = currentMapLayer() || names[0];
    if (want && names.includes(want)) els.mapSel.value = want;
    if (els.mapSel.value) return openMap(els.mapSel.value);
    status('no maps under assets/worldmap/');
  }).catch(e => status('map list failed: ' + e.message));
  syncControls();
}
/** loadMap with the page's reporting: a v1 map is a STATUS (the converter
 *  names itself), any other failure a toast. */
function openMap(name) {
  return loadMap(name).catch(e => {
    if (e.v1) {
      map = null;
      status(e.message);
      toast(e.message);
      if (els.sitesList) { els.sitesList.innerHTML = ''; els.sitePanel.innerHTML = ''; }
      paint();
    } else toast('load failed: ' + e.message, true);
  });
}
export function isDirty() { return dirty; }

/* ---- tuning.json world.* through the host (the map + edit layer the game loads) ---- */
function tuningWorld() {
  const T = H.tuning && H.tuning();
  if (!T || !T.tune) return null;
  if (!T.tune.world || typeof T.tune.world !== 'object') T.tune.world = {mapLayer: 'default'};
  return T;
}
function currentMapLayer() {
  const T = tuningWorld();
  return T ? (T.tune.world.mapLayer || 'default') : '';
}
function pushTuning() {
  const T = tuningWorld();
  if (!T) { toast('tuning.json is not loaded: the game keeps its current map', true); return; }
  if (mapLayerWanted && T.tune.world.mapLayer !== mapLayerWanted) {
    T.tune.world.mapLayer = mapLayerWanted;
    tuningTouched = true;
    if (T.touchTune) T.touchTune();
    toast('tuning.json world.mapLayer = ' + T.tune.world.mapLayer + ' — save (Ctrl+S) and press F7 in the game');
  }
}
// MIGRATION (P7): a tuning.json that still names world.editLayer hands it to
// the loaded map (if the map names none) and drops the retired key.
function migrateEditLayer() {
  const T = tuningWorld();
  if (!T || !map || !('editLayer' in T.tune.world)) return;
  const old = T.tune.world.editLayer || '';
  delete T.tune.world.editLayer;
  tuningTouched = true;
  if (T.touchTune) T.touchTune();
  if (old && !map.json.editLayer) {
    map.json.editLayer = old;
    markDirty();
    toast('the edit layer moved to the map: map.json editLayer = ' + old + ' (was tuning.json world.editLayer) — save both');
  }
  syncEditSel();
}
function syncEditSel() {
  if (!els.editSel) return;
  const cur = (map && map.json.editLayer) || '';
  if (cur && ![...els.editSel.options].some(o => o.value === cur))
    els.editSel.append(H.el('option', {value: cur}, cur + ' (missing)'));
  els.editSel.value = cur;
}
async function refreshEditLayers() {
  if (!els.editSel) return;
  try {
    const r = await fetch('/api/worldedits', {cache: 'no-store'});
    const j = r.ok ? await r.json() : {layers: []};
    els.editSel.replaceChildren(H.el('option', {value: ''}, '(none)'), ...(j.layers || []).map(L => H.el('option', {value: L.name}, L.name)));
  } catch (e) { els.editSel.replaceChildren(H.el('option', {value: ''}, '(none)')); }
  syncEditSel();
}
/** tuning.json arrived (or changed): the selectors follow it. */
export function tuningAvailable() {
  const T = tuningWorld();
  if (!T) return;
  if (els.mapSel && T.tune.world.mapLayer && [...els.mapSel.options].some(o => o.value === T.tune.world.mapLayer) &&
      els.mapSel.value !== T.tune.world.mapLayer && !mapLayerWanted) {
    els.mapSel.value = T.tune.world.mapLayer;
    openMap(els.mapSel.value);
  }
  refreshEditLayers();
}
/** Ctrl+S on the map page: the map if it is dirty, and tuning if a selector moved. */
export function saveFromHost() {
  if (dirty) saveMap();
  if (tuningTouched && H.saveTuning) { H.saveTuning(); tuningTouched = false; }
}
export function activate() {
  if (map) { syncTerrain(); syncSites(); publishTerrain(); syncLines(); syncHeightTools(); VL.sync(); LC.sync(); RL.sync(); GL.sync(); CLM.sync(); WL.sync(); }
  // First showing: open the preview in its voxel view (the deep link's recipe,
  // tuner.html tunerDeepLink). wvToggle / wvOn are tuner.html globals.
  if (previewPending) {
    previewPending = false;
    if (window.wgPreviewShow) window.wgPreviewShow();
    if (typeof window.wvToggle === 'function' && !window.wvOn) window.wvToggle();
  }
}
// test seams
export function _map() { return map; }
export function _load(name) { return loadMap(name); }
export function _save() { return saveMap(); }
export function _payload() { return map ? planePayload() : null; }
export function _brush() { return brush; }
export function _disp() { return disp; }
export function _view() { return view; }
export function _stats() { return stats; }
export function _status() { return els.status ? els.status.textContent : ''; }
export function _paint() { paint(); }
export function _loadsIdle() { return loads.active === 0 && loads.queue.length === 0; }
export function _selected() { return selected; }
export function _select(site) { selected = site; syncSites(); paint(); }
export function _check() { return check; }
export function _trees() { return trees; }
export function _sculpt() { return sculpt; }
export function _tool(t) { if (t) { tool = t; syncControls(); paint(); } return tool; }
export function _undoDepth() { return undo.length; }
// E2 test seams (the Height tools)
export function _hb() { return hb; }
export function _shape() { return shape; }
export function _lines() { return lines; }
export function _bakes() { return bakes; }
export function _bake(L) { bakeLine(L); }
export function _unbake(L) { unbakeLine(L); }
export function _area(a) { if (a !== undefined) { area = a; syncHeightTools(); } return area; }
export function _erosion() { return erosion; }
export function _runErosion() { return runErosion(); }
export function _commitErosion() { commitErosion(); }
export function _discardErosion() { discardErosion(); }
export function _exportPng(plane) { return plane === 'biome' ? exportBiomePng(false) : exportHeightPng(false); }
export function _importPng(bytes, plane, name) { return openPngImport(bytes, plane, name); }
export function _pngIn() { return pngIn; }
export function _applyPng() { return applyPngImport(); }
// E3 test seams (the vector layers)
export function _vl() { return VL; }
export function _savedJson() { return savedJson; }
// K1 test seams (the registry, the ranges, the generators, the overlays, the jobs)
export function _reg() { return REG; }
export function _rl() { return RL; }
export function _gl() { return GL; }
export function _ol() { return OL; }
// E2b test seams (climate + coast)
export function _clm() { return CLM; }
export function _cst() { return CST; }
export function _wl() { return WL; }
export function _preview() { return preview; }
export function _job() { return mainJob; }
// E4 test seams (the locales)
export function _lc() { return LC; }
export function _heights() { return heights; }
export function _fetchHeights() { return fetchHeights(); }
export function _groundAt(x, z) { return groundAt(x, z); }
export function _scAt(x, z) { return scAt(sculpt.tiles, x, z); }
