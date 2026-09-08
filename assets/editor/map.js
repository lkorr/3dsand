/* map.js — the World map page of the Environment tab: THE ONE FRONT DOOR to
 * the world (PLAN_environment_truth P-I). Paint WHERE the biomes are, shape
 * the landform, declare the terrain's numbers, place every site (the harness
 * pad, the spawn, authored lakes, declared landforms, stamps), pick the map
 * and edit layer the game loads, and preview the result as a heightmap or as
 * real voxels — on the map worldgen reads (assets/worldmap/<name>/{map.json,
 * map.svmap}; src/sim/worldmap.h is the format's authority).
 *
 * THE TERRAIN IS THE MAP'S (P-G). map.json `terrain` carries every number
 * that used to be a worldgen.* knob in tuning.json — the datum, what a
 * painted landform spans, the seeded octave ladder, the attenuation, the calm
 * home area, the sediment wedge, the treeline, the reference scale — packed
 * into the worldMap buffer header and read by the height mirror on both
 * sides. The Terrain section below edits it; the biome pages carry the
 * per-biome relief (curve + multipliers). A `kind: "landform"` site is a
 * DECLARED mountain: a peak / ridge / basin / plateau overlaid onto the
 * landform plane at load, the same on every seed (Tier A).
 *
 * WHAT THIS IS. Tier A of docs/PLAN_world_map.md — the seed-INDEPENDENT
 * layout. A cell is 2^cellLog2 voxels (1024 = 102.4 m by default); the plane
 * is `size` cells with `originCell` at world (0,0). The biome plane holds
 * indices into `biomes[]` (the palette, by NAME); the landform plane is 0..255
 * (0 = ocean floor, 255 = alpine) and owns the height from P4; moisture is
 * reserved. Sites are a list: one `kind: "pad"` box (the selftest harness
 * region), one `kind: "spawn"` column (where the game starts and the centre
 * of the calm home area; PLAN_environment_truth P-C), `kind: "stamp"`
 * markers (P5), and `kind: "water"` AUTHORED LAKES (P-F: a water preset at a
 * fixed centre, the same on every seed).
 *
 * WHAT IT IS NOT. Not a preview of the generated world: the Worldgen tab's
 * heightmap and voxel views are. This page draws the PLANES, plainly, so you
 * see what you painted and not what noise made of it. Every save moves the
 * world hash (the map is an input beside the seed); the engine reads the map
 * at boot, so regenerate the world (or restart) to see a change.
 *
 * Painting model: a circular brush in CELL space, left-drag paints, right- or
 * middle-drag pans, wheel zooms about the cursor, Shift+drag with the pad tool
 * draws the harness box in world voxels. Undo is a stroke stack of plane
 * snapshots (115 KB each, capped) — the same coalescing-per-drag rule
 * envui.makeUndo uses for rows, done by hand because the target is a
 * Uint8Array rather than a JSON path.
 */

const MAGIC = 0x504D5653;   // 'SVMP'
const VERSION = 1;

// Display colours by biome NAME; anything unknown gets a stable hash colour.
const COLOR = {
  forest: '#2f7a3a', meadow: '#8cc95a', pine: '#1f5a3a', desert: '#d9c27a',
  tundra: '#d8e6f0', swamp: '#6a7a2f', alpine: '#8f959c', ocean: '#2b5d9e'
};
function colorOf(name) {
  if (COLOR[name]) return COLOR[name];
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return `hsl(${h % 360} 45% 55%)`;
}

let H = null;
let els = {};
let map = null;            // {name, json, biome:Uint8Array, landform:Uint8Array, moisture:Uint8Array}
let dirty = false;
let tool = 'biome';        // biome | landform | pad | spawn | stamp | water | site
// P-G: the terrain block map.json carries (src/sim/worldmap.h TerrainParams
// is the C++ twin; scripts/seed_terrain_rows.py the seeder). A map without
// one gets these, which are the engine's defaults too.
const TERRAIN_DEFAULTS = {
  refVoxelsPerMetre: 10, baseHeight: 200, landformRangeVox: 1024,
  rangeAmplitude: 256, rangeLog2: 9, hillAmplitude: 64, hillLog2: 7,
  detailAmplitude: 16, detailLog2: 5, grainAmplitude: 4, grainLog2: 3,
  fbmAtten: 256, homeArea: {y: 200, radius: 320, fade: 2048},
  sedCeil: 264, sedFraction: 64, sedStrip: 6, sedSlope: 96, sedMax: 32, sedTopsoil: 4,
  treeline: 228
};
// [path, label, min, max, step, unit, description]; the path is into terrain
const TERRAIN_ROWS = [
  ['baseHeight', 'world datum', 0, 1024, 4, 'vox', 'The MEAN ground height. The landform plane and the range octave are centred deviations around it.'],
  ['landformRangeVox', 'landform range', 0, 4096, 16, 'vox', 'What a painted landform 0..255 spans, centred on 128: at 1024 a painted 255 is 51 m above the datum and a painted 0 is 51 m below. Raise it and a painted ridge becomes a 150 m mountain (was worldgen.contAmplitude).'],
  ['rangeAmplitude', 'range height', 0, 1024, 8, 'vox', 'Full swing of the seeded range octave: ridges and valleys inside a painted region. This and the landform are what the home area fades out and the biome curve reshapes.'],
  ['rangeLog2', 'range size (log2)', 3, 15, 1, 'log2', 'Range noise cell as a power of two: 9 = 512 voxels = 51.2 m.'],
  ['hillAmplitude', 'hill height', 0, 512, 4, 'vox', 'Full swing of the broad hill octave (each biome scales it by its terrain.hill). Live inside the home area, which is what makes spawn rolling country rather than a dinner plate.'],
  ['hillLog2', 'hill size (log2)', 3, 15, 1, 'log2', 'Broad-hill noise cell as a power of two: 7 = 128 voxels = 12.8 m.'],
  ['detailAmplitude', 'detail height', 0, 128, 1, 'vox', 'Full swing of the detail octave (scaled per biome by terrain.detail).'],
  ['detailLog2', 'detail size (log2)', 3, 15, 1, 'log2', 'Detail noise cell: 5 = 32 voxels = 3.2 m.'],
  ['grainAmplitude', 'grain height', 0, 32, 1, 'vox', 'Full swing of the finest octave (scaled per biome by terrain.grain).'],
  ['grainLog2', 'grain size (log2)', 3, 15, 1, 'log2', 'Grain noise cell: 3 = 8 voxels = 0.8 m.'],
  ['fbmAtten', 'slope attenuation (Q8)', 0, 256, 8, 'Q8', 'iq\u2019s derivative attenuation: each octave is divided by 1 + atten\u00b7|slope|\u00b2. 0 is plain fBm (the whole world above the angle of repose, nothing loose ever rests); 256 is the textbook form. A rule-2 mechanism, not a look knob.'],
  ['homeArea.y', 'home area height', 0, 1024, 4, 'vox', 'The ground height the calm area around the spawn site sits at (was worldgen.spawnPlainY). The two coarse rungs fade to it; the three fine ones stay live.'],
  ['homeArea.radius', 'home area radius', 0, 4096, 16, 'vox', 'Chebyshev radius around the spawn site held calm (was spawnPlainR).'],
  ['homeArea.fade', 'home area fade', 1, 8192, 64, 'vox', 'How far past the radius the coarse relief ramps back to full (was spawnPlainFade). LOAD-BEARING: a short fade builds a cliff at exactly the boundary; the terrain gate\u2019s A4 measures it.'],
  ['sedCeil', 'sediment ceiling', 0, 1024, 4, 'vox', 'The sediment wedge: ground below this carries loose dirt over gravel, thickness (ceiling - ground) \u00d7 fraction/256 - strip, slope-gated, clamped to max.'],
  ['sedFraction', 'sediment fraction (Q8)', 0, 256, 4, 'Q8', ''],
  ['sedStrip', 'sediment strip', 0, 64, 1, 'vox', ''],
  ['sedSlope', 'sediment slope gate (Q8)', 0, 512, 8, 'Q8', 'Dirt and gravel are POWDERS: the wedge thins to nothing as the landform slope approaches this, or the CA never sleeps. 0 = no wedge.'],
  ['sedMax', 'sediment max', 0, 40, 1, 'vox', 'Clamped under the cave shell (36 vox) at load.'],
  ['sedTopsoil', 'topsoil depth', 0, 40, 1, 'vox', 'The top of the wedge that wears the biome\u2019s subsoil material.'],
  ['treeline', 'treeline', 0, 2048, 4, 'vox', 'Snow and no trees at or above this ground Y. The shipped cover rows stop at 227; the alpine cushion row starts at 228.'],
];
const LANDFORM_SHAPES = ['peak', 'ridge', 'basin', 'plateau'];
let landform = {shape: 'ridge', radius: 8000, heightVox: 600, rotation: 90};   // the tool's defaults for a NEW site
let landDrag = null;       // {site, dx, dz}: dragging a declared landform
let selected = null;       // the site the sites panel has selected (any kind)
let heights = null;        // the decoded --heightmap backdrop, or null
let showHeights = false;
let mapLayerWanted = '';   // world.mapLayer / editLayer as the selectors have them
let editLayerWanted = '';
let tuningTouched = false;
const SPAWN_DEFAULT = [140, 140];   // worldmap.cpp's default when a map names no spawn
// The water tool (P-F): an AUTHORED LAKE is `{kind: "water", preset, at: [x, z],
// radius?}` -- Tier A, the same place on every seed, wearing the preset's
// geometry (footprint radius unless overridden, depth profile, berm, shore
// band, bed, flora). `presets` is assets/water/ by name; `radiusM` 0 = the
// preset's own radius; `geom` caches each preset's radius/band in voxels for
// drawing the footprint to scale.
let water = {preset: '', radiusM: 0, presets: [], geom: {}};
let waterDrag = null;      // {site, dx, dz}: dragging an existing lake by its centre
let brush = {index: 0, radius: 2, landform: 128, soft: true};
let view = {cx: 0, cz: 0, scale: 4};   // cell-space centre + pixels per cell
let showLandform = true;
let undo = [];
let redo = [];
let stroke = null;         // snapshot taken at pointerdown, pushed at pointerup if anything changed
let padDrag = null;
let img = null;            // ImageData of the planes

/* ---- helpers ---------------------------------------------------------------- */
function toast(m, bad) { if (H && H.toast) H.toast(m, bad); }
function markDirty(d = true) {
  dirty = d;
  if (H && H.onDirty) H.onDirty(d);
  if (els.save) els.save.disabled = !d;
}
function cellOfWorld(x, z) {
  const j = map.json;
  return [(x >> j.cellLog2) + j.originCell[0], (z >> j.cellLog2) + j.originCell[1]];
}
function worldOfCell(cx, cz) {
  const j = map.json;
  return [(cx - j.originCell[0]) << j.cellLog2, (cz - j.originCell[1]) << j.cellLog2];
}
function snapshot() {
  return {biome: new Uint8Array(map.biome), landform: new Uint8Array(map.landform),
          sites: JSON.parse(JSON.stringify(map.json.sites || [])),
          terrain: JSON.parse(JSON.stringify(terrainOf()))};
}
function restore(s) {
  map.biome.set(s.biome); map.landform.set(s.landform);
  map.json.sites = JSON.parse(JSON.stringify(s.sites));
  map.json.terrain = JSON.parse(JSON.stringify(s.terrain));
  if (selected && !map.json.sites.includes(selected)) selected = map.json.sites.find(x => x.id === selected.id) || null;
  syncTerrain(); syncSites();
}
/** The map's terrain block, created from the defaults if the file has none. */
function terrainOf() {
  const j = map.json;
  if (!j.terrain || typeof j.terrain !== 'object') j.terrain = JSON.parse(JSON.stringify(TERRAIN_DEFAULTS));
  if (!j.terrain.homeArea || typeof j.terrain.homeArea !== 'object') j.terrain.homeArea = {...TERRAIN_DEFAULTS.homeArea};
  for (const k of Object.keys(TERRAIN_DEFAULTS))
    if (k !== 'homeArea' && typeof j.terrain[k] !== 'number') j.terrain[k] = TERRAIN_DEFAULTS[k];
  for (const k of Object.keys(TERRAIN_DEFAULTS.homeArea))
    if (typeof j.terrain.homeArea[k] !== 'number') j.terrain.homeArea[k] = TERRAIN_DEFAULTS.homeArea[k];
  return j.terrain;
}
function getPath(o, path) { return path.split('.').reduce((a, k) => (a == null ? a : a[k]), o); }
function setPath(o, path, v) {
  const ks = path.split('.');
  let a = o;
  for (let i = 0; i < ks.length - 1; i++) a = a[ks[i]];
  a[ks[ks.length - 1]] = v;
}
// The preview pane (the heightmap / voxel views in tuner.html) reads the
// treeline and the home height for its colours and its camera; hand them
// over rather than let it guess.
function publishTerrain() {
  if (!map) return;
  const t = terrainOf();
  const sp = (map.json.sites || []).find(s => s.kind === 'spawn');
  const at = sp && Array.isArray(sp.at) ? sp.at : SPAWN_DEFAULT;
  window.wgTerrain = {treeline: t.treeline, homeY: t.homeArea.y, homeR: t.homeArea.radius, homeFade: t.homeArea.fade,
                      seaLevelY: map.json.seaLevelY | 0, spawnX: at[0], spawnZ: at[1]};
}

/* ---- planes <-> bytes -------------------------------------------------------- */
export function parsePlanes(buf, w, h) {
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== MAGIC) throw new Error('not an SVMP file');
  if (dv.getUint32(4, true) !== VERSION) throw new Error('unknown SVMP version');
  const fw = dv.getUint32(8, true), fh = dv.getUint32(12, true);
  if (fw !== w || fh !== h) throw new Error(`planes are ${fw}x${fh}, map.json says ${w}x${h}`);
  const n = w * h;
  if (buf.byteLength < 16 + n * 3) throw new Error('SVMP truncated');
  const u8 = new Uint8Array(buf);
  return {biome: u8.slice(16, 16 + n), landform: u8.slice(16 + n, 16 + 2 * n), moisture: u8.slice(16 + 2 * n, 16 + 3 * n)};
}
export function serializePlanes(w, h, biome, landform, moisture) {
  const n = w * h;
  const out = new Uint8Array(16 + n * 3);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, MAGIC, true); dv.setUint32(4, VERSION, true);
  dv.setUint32(8, w, true); dv.setUint32(12, h, true);
  out.set(biome, 16); out.set(landform, 16 + n); out.set(moisture, 16 + 2 * n);
  return out;
}

/* ---- server ------------------------------------------------------------------ */
async function listMaps() {
  const r = await fetch('/api/worldmaps', {cache: 'no-store'});
  const j = await r.json();
  return (j.maps || []).map(m => m.name);
}
async function loadMap(name) {
  const rj = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (!rj.ok) throw new Error('map.json: HTTP ' + rj.status);
  const json = await rj.json();
  const rp = await fetch('/api/worldmap/planes?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (!rp.ok) throw new Error('map.svmap: HTTP ' + rp.status);
  const planes = parsePlanes(await rp.arrayBuffer(), json.size[0], json.size[1]);
  map = {name, json, ...planes};
  undo = []; redo = [];
  selected = null; heights = null;
  markDirty(false);
  rebuildPalette();
  terrainOf();
  syncTerrain(); syncSites(); publishTerrain();
  fit();
  paint();
  status(`loaded worldmap/${name}: ${json.size[0]}x${json.size[1]} cells of ${1 << json.cellLog2} vox (${(json.size[0] * (1 << json.cellLog2) / 10 / 1000).toFixed(1)} km)`);
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
async function saveMap() {
  if (!map) return;
  const name = map.name;
  const body = JSON.stringify(map.json, null, 2) + '\n';
  let r = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {method: 'POST', body});
  let j = await r.json();
  if (!j.ok) { toast('save failed: ' + (j.error || '?'), true); return; }
  const blob = serializePlanes(map.json.size[0], map.json.size[1], map.biome, map.landform, map.moisture);
  r = await fetch('/api/worldmap/planes?name=' + encodeURIComponent(name), {method: 'POST', body: blob});
  j = await r.json();
  if (!j.ok) { toast('planes save failed: ' + (j.error || '?'), true); return; }
  markDirty(false);
  toast(`saved worldmap/${name} — press Apply to game (or F7 in the game) to see it; every map edit moves the world hash`);
}

/* ---- rendering --------------------------------------------------------------- */
function rebuildImage() {
  const [w, h] = map.json.size;
  if (!img || img.width !== w || img.height !== h) img = new ImageData(w, h);
  const d = img.data;
  const pal = map.json.biomes.map(n => {
    const c = colorOf(n);
    const tmp = document.createElement('canvas').getContext('2d');
    tmp.fillStyle = c; tmp.fillRect(0, 0, 1, 1);
    return tmp.getImageData(0, 0, 1, 1).data;
  });
  for (let i = 0; i < w * h; i++) {
    const p = pal[map.biome[i]] || pal[0];
    let k = 1;
    if (showLandform) k = 0.55 + 0.9 * (map.landform[i] / 255);
    d[i * 4] = Math.min(255, p[0] * k);
    d[i * 4 + 1] = Math.min(255, p[1] * k);
    d[i * 4 + 2] = Math.min(255, p[2] * k);
    d[i * 4 + 3] = 255;
  }
}
function paint() {
  if (!map || !els.canvas) return;
  const c = els.canvas, ctx = c.getContext('2d');
  const W = c.clientWidth, Hh = c.clientHeight;
  if (c.width !== W || c.height !== Hh) { c.width = W; c.height = Hh; }
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#0e1219'; ctx.fillRect(0, 0, W, Hh);
  rebuildImage();
  // planes -> screen: screen = (cell - view.c) * scale + centre
  const [w, h] = map.json.size;
  const ox = W / 2 - view.cx * view.scale, oy = Hh / 2 - view.cz * view.scale;
  const off = els.off || (els.off = document.createElement('canvas'));
  if (off.width !== w || off.height !== h) { off.width = w; off.height = h; }
  off.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  if (showHeights && heights && heights.img) {
    // The engine's own heights (World::TerrainColumn over the whole map, the
    // saved file), hillshaded, under the biome colours: the painted landform
    // seen as RELIEF rather than as a shade. Cells are a colour wash on top.
    const hi = els.heightsOff || (els.heightsOff = document.createElement('canvas'));
    if (hi.width !== heights.res || hi.height !== heights.res) { hi.width = heights.res; hi.height = heights.res; }
    hi.getContext('2d').putImageData(heights.img, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(hi, ox + heights.cell0x * view.scale, oy + heights.cell0z * view.scale,
                  heights.cellsW * view.scale, heights.cellsH * view.scale);
    ctx.imageSmoothingEnabled = false;
    ctx.globalAlpha = 0.45;
    ctx.drawImage(off, ox, oy, w * view.scale, h * view.scale);
    ctx.globalAlpha = 1;
  } else {
    ctx.drawImage(off, ox, oy, w * view.scale, h * view.scale);
  }
  // origin crosshair
  const [ocx, ocz] = map.json.originCell;
  ctx.strokeStyle = '#ffd866'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(ox + ocx * view.scale - 8, oy + ocz * view.scale); ctx.lineTo(ox + ocx * view.scale + 8, oy + ocz * view.scale);
  ctx.moveTo(ox + ocx * view.scale, oy + ocz * view.scale - 8); ctx.lineTo(ox + ocx * view.scale, oy + ocz * view.scale + 8);
  ctx.stroke();
  // sites: the pad box in world voxels -> cells (fractional)
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'pad') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const x0 = (s.min[0] / cv) + ocx, z0 = (s.min[1] / cv) + ocz;
    const x1 = ((s.max[0] + 1) / cv) + ocx, z1 = ((s.max[1] + 1) / cv) + ocz;
    ctx.strokeStyle = tool === 'pad' ? '#ff7a7a' : '#ffb454'; ctx.lineWidth = 2;
    ctx.strokeRect(ox + x0 * view.scale, oy + z0 * view.scale, (x1 - x0) * view.scale, (z1 - z0) * view.scale);
    ctx.fillStyle = '#ffb454'; ctx.font = '11px monospace';
    ctx.fillText(s.id || 'pad', ox + x0 * view.scale + 3, oy + z0 * view.scale - 3);
  }
  // the spawn site: a diamond at the column, the default (dim) if the map
  // names none -- the engine starts the player there either way
  {
    const l = map.json.cellLog2, cv = 1 << l;
    const s = (map.json.sites || []).find(s => s.kind === 'spawn');
    const at = s && Array.isArray(s.at) ? s.at : SPAWN_DEFAULT;
    const sx = ox + (at[0] / cv + ocx) * view.scale, sz = oy + (at[1] / cv + ocz) * view.scale;
    const r = 6;
    ctx.strokeStyle = tool === 'spawn' ? '#ff7a7a' : (s ? '#8dff9a' : '#6f8f75'); ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(sx, sz - r); ctx.lineTo(sx + r, sz); ctx.lineTo(sx, sz + r); ctx.lineTo(sx - r, sz); ctx.closePath();
    ctx.stroke();
    ctx.fillStyle = s ? '#8dff9a' : '#6f8f75'; ctx.font = '11px monospace';
    ctx.fillText(s ? `spawn (${at[0]},${at[1]})` : `spawn (default ${at[0]},${at[1]})`, sx + r + 3, sz + 4);
  }
  // stamp sites: a marker at the centre with the footprint radius
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'stamp') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const sx = ox + (s.x / cv + ocx) * view.scale, sz = oy + (s.z / cv + ocz) * view.scale;
    const rr = Math.max(3, ((s.radius || 16) / cv) * view.scale);
    ctx.strokeStyle = tool === 'stamp' ? '#ff7a7a' : '#7fd4ff'; ctx.lineWidth = 2;
    ctx.strokeRect(sx - rr, sz - rr, rr * 2, rr * 2);
    ctx.fillStyle = '#7fd4ff'; ctx.font = '11px monospace';
    ctx.fillText((s.id || 'site') + ' (' + (s.template || '?') + (s.rot ? ' r' + s.rot : '') + ')', sx + rr + 3, sz + 4);
  }
  // water sites (P-F): the disc to scale, the shore/berm band as a dashed
  // ring, the preset and radius as the label
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'water') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const at = Array.isArray(s.at) ? s.at : [0, 0];
    const sx = ox + (at[0] / cv + ocx) * view.scale, sz = oy + (at[1] / cv + ocz) * view.scale;
    const g = water.geom[s.preset] || {radius: 60, band: 24};
    const rv = s.radius || g.radius;
    const rr = Math.max(3, (rv / cv) * view.scale);
    const rb = Math.max(rr + 1, ((rv + g.band) / cv) * view.scale);
    ctx.strokeStyle = tool === 'water' ? '#ff7a7a' : '#5fc8ff'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(sx, sz, rr, 0, Math.PI * 2); ctx.stroke();
    ctx.setLineDash([3, 3]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(sx, sz, rb, 0, Math.PI * 2); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#5fc8ff'; ctx.font = '11px monospace';
    ctx.fillText((s.id || 'lake') + ' (' + (s.preset || '?') + ' r' + (rv / 10).toFixed(1) + 'm)', sx + rr + 3, sz + 4);
  }
  // declared landforms (P-G): a peak / basin / plateau as a disc, a ridge as
  // its ellipse turned to its heading; + for a lift, - for a basin
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'landform') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const at = Array.isArray(s.at) ? s.at : [0, 0];
    const sx = ox + (at[0] / cv + ocx) * view.scale, sz = oy + (at[1] / cv + ocz) * view.scale;
    const rr = Math.max(3, ((s.radius || 1024) / cv) * view.scale);
    const sel = selected === s;
    ctx.strokeStyle = sel ? '#ffffff' : (tool === 'landform' ? '#ff7a7a' : (s.shape === 'basin' ? '#7fa8ff' : '#e8c477'));
    ctx.lineWidth = sel ? 3 : 2;
    ctx.beginPath();
    if (s.shape === 'ridge') ctx.ellipse(sx, sz, rr, Math.max(2, rr / 3), (s.rotation || 0) * Math.PI / 180, 0, Math.PI * 2);
    else ctx.arc(sx, sz, rr, 0, Math.PI * 2);
    ctx.stroke();
    if (s.shape === 'plateau') { ctx.setLineDash([2, 3]); ctx.beginPath(); ctx.arc(sx, sz, rr * 0.6, 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]); }
    ctx.fillStyle = s.shape === 'basin' ? '#7fa8ff' : '#e8c477'; ctx.font = '11px monospace';
    ctx.fillText((s.id || s.shape) + ' (' + s.shape + ' ' + ((s.heightVox || 0) / 10).toFixed(0) + ' m)', sx + rr + 3, sz + 4);
  }
  // the selected site of any other kind: a white ring
  if (selected && selected.kind !== 'landform') {
    const l = map.json.cellLog2, cv = 1 << l;
    const at = Array.isArray(selected.at) ? selected.at : [selected.x || 0, selected.z || 0];
    if (selected.kind !== 'pad') {
      const sx = ox + (at[0] / cv + ocx) * view.scale, sz = oy + (at[1] / cv + ocz) * view.scale;
      ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(sx, sz, 10, 0, Math.PI * 2); ctx.stroke();
    }
  }
  // brush cursor
  if (els.hover && tool !== 'pad' && tool !== 'stamp' && tool !== 'spawn' && tool !== 'water' && tool !== 'landform' && tool !== 'site') {
    ctx.strokeStyle = 'rgba(255,255,255,0.7)'; ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(ox + (els.hover[0] + 0.5) * view.scale, oy + (els.hover[1] + 0.5) * view.scale,
            (brush.radius + 0.5) * view.scale, 0, Math.PI * 2);
    ctx.stroke();
  }
}
function fit() {
  if (!map || !els.canvas) return;
  const [w, h] = map.json.size;
  view.cx = w / 2; view.cz = h / 2;
  view.scale = Math.max(1, Math.floor(Math.min(els.canvas.clientWidth / w, els.canvas.clientHeight / h)));
}
function status(t) { if (els.status) els.status.textContent = t; }

/* ---- input --------------------------------------------------------------------- */
function cellAt(ev) {
  const r = els.canvas.getBoundingClientRect();
  const W = els.canvas.clientWidth, Hh = els.canvas.clientHeight;
  const px = ev.clientX - r.left, py = ev.clientY - r.top;
  return [Math.floor((px - W / 2) / view.scale + view.cx), Math.floor((py - Hh / 2) / view.scale + view.cz)];
}
// The exact world column under the cursor (fractional cell -> voxels): the
// cell centre when zoomed out, the column itself when zoomed in.
function worldColumnAt(ev) {
  const r = els.canvas.getBoundingClientRect();
  const W = els.canvas.clientWidth, Hh = els.canvas.clientHeight;
  const fx = (ev.clientX - r.left - W / 2) / view.scale + view.cx;
  const fz = (ev.clientY - r.top - Hh / 2) / view.scale + view.cz;
  const cv = 1 << map.json.cellLog2;
  return [Math.round((fx - map.json.originCell[0]) * cv), Math.round((fz - map.json.originCell[1]) * cv)];
}
function applyBrush(cx, cz) {
  const [w, h] = map.json.size;
  const r = brush.radius;
  let changed = false;
  for (let dz = -r; dz <= r; dz++)
    for (let dx = -r; dx <= r; dx++) {
      if (dx * dx + dz * dz > r * r + r * 0.5) continue;
      const x = cx + dx, z = cz + dz;
      if (x < 0 || z < 0 || x >= w || z >= h) continue;
      const i = z * w + x;
      if (tool === 'biome') {
        if (map.biome[i] !== brush.index) { map.biome[i] = brush.index; changed = true; }
      } else if (tool === 'landform') {
        // soft brush: move the value a third of the way per pass so a held
        // stroke ramps rather than stamps a plateau
        const cur = map.landform[i];
        const next = brush.soft ? Math.round(cur + (brush.landform - cur) * 0.34) : brush.landform;
        if (next !== cur) { map.landform[i] = next; changed = true; }
      }
    }
  return changed;
}
function wire() {
  const c = els.canvas;
  let panning = null;
  c.addEventListener('contextmenu', e => e.preventDefault());
  c.addEventListener('pointerdown', ev => {
    if (!map) return;
    c.setPointerCapture(ev.pointerId);
    if (ev.button === 1 || ev.button === 2) { panning = {x: ev.clientX, y: ev.clientY, cx: view.cx, cz: view.cz}; return; }
    if (tool === 'pad') {
      const [cx, cz] = cellAt(ev);
      const [wx, wz] = worldOfCell(cx, cz);
      stroke = snapshot();
      padDrag = {x0: wx, z0: wz};
      return;
    }
    if (tool === 'spawn') {
      // Click: the spawn site goes to the cursor's world column. ONE per map
      // (the loader refuses two), so this moves it rather than adding one.
      const [wx, wz] = worldColumnAt(ev);
      const sites = map.json.sites || (map.json.sites = []);
      stroke = snapshot();
      const s = sites.find(s => s.kind === 'spawn');
      if (s) s.at = [wx, wz];
      else sites.push({id: 'spawn', kind: 'spawn', at: [wx, wz]});
      stroke.changed = true;
      paint();
      return;
    }
    if (tool === 'stamp') {
      // Click: a new stamp site at the cursor's world column (cell centre if
      // zoomed out, the exact column when zoomed in). Shift+click on an
      // existing marker deletes it. Sites are hand-placed here; rules
      // (seeded placement per biome) are P5b.
      const [wx, wz] = worldColumnAt(ev);
      const sites = map.json.sites || (map.json.sites = []);
      const hit = sites.find(s => s.kind === 'stamp' && Math.abs(s.x - wx) <= (s.radius || 16) && Math.abs(s.z - wz) <= (s.radius || 16));
      stroke = snapshot();
      if (ev.shiftKey && hit) {
        sites.splice(sites.indexOf(hit), 1);
        stroke.changed = true;
      } else if (!hit) {
        const t = prompt('Template: assets/prefabs/<name>.vox', els.lastTemplate || '');
        if (!t) { stroke = null; return; }
        els.lastTemplate = t;
        const rot = parseInt(prompt('Rotation (0..3 quarter turns):', '0') || '0', 10) & 3;
        sites.push({id: t + '_' + sites.length, kind: 'stamp', template: t, x: wx, z: wz, rot, padMargin: 8, salt: sites.length});
        stroke.changed = true;
      }
      paint();
      return;
    }
    if (tool === 'landform') {
      // Click on a landform: pick it up (drag moves it) and select it. Shift+
      // click: delete it. Click on open map: a new one at the cursor's world
      // column with the panel's shape / radius / height / rotation. Tier A:
      // overlaid onto the landform plane at load, the same on every seed.
      const [wx, wz] = worldColumnAt(ev);
      const sites = map.json.sites || (map.json.sites = []);
      const hit = landformAt(wx, wz);
      stroke = snapshot();
      if (ev.shiftKey && hit) {
        sites.splice(sites.indexOf(hit), 1);
        if (selected === hit) selected = null;
        stroke.changed = true;
      } else if (hit) {
        selected = hit;
        landDrag = {site: hit, dx: hit.at[0] - wx, dz: hit.at[1] - wz};
      } else {
        const n = sites.filter(s => s.kind === 'landform').length;
        const s = {id: landform.shape + '_' + n, kind: 'landform', shape: landform.shape, at: [wx, wz],
                   radius: Math.max(1, Math.round(landform.radius)), heightVox: Math.round(landform.heightVox)};
        if (s.shape === 'ridge') s.rotation = Math.round(landform.rotation) || 0;
        sites.push(s);
        selected = s;
        stroke.changed = true;
      }
      syncSites();
      paint();
      return;
    }
    if (tool === 'site') {
      // The select tool: click the nearest site marker of any kind.
      const [wx, wz] = worldColumnAt(ev);
      const hit = siteAt(wx, wz);
      selected = hit;
      syncSites();
      paint();
      return;
    }
    if (tool === 'water') {
      // Click on a lake: pick it up (drag moves it). Shift+click: delete it.
      // Click on open map: a new lake at the cursor's world column with the
      // chosen preset and radius (0 = the preset's own). Tier A: the engine
      // reads the centre and radius from map.json, never from the seed.
      const [wx, wz] = worldColumnAt(ev);
      const sites = map.json.sites || (map.json.sites = []);
      const hit = sites.find(s => {
        if (s.kind !== 'water' || !Array.isArray(s.at)) return false;
        const g = water.geom[s.preset] || {radius: 60};
        const r = s.radius || g.radius;
        const dx = s.at[0] - wx, dz = s.at[1] - wz;
        return dx * dx + dz * dz <= r * r;
      });
      stroke = snapshot();
      if (ev.shiftKey && hit) {
        sites.splice(sites.indexOf(hit), 1);
        stroke.changed = true;
      } else if (hit) {
        waterDrag = {site: hit, dx: hit.at[0] - wx, dz: hit.at[1] - wz};
      } else {
        if (!water.preset) { toast('pick a water preset first', true); stroke = null; return; }
        const n = sites.filter(s => s.kind === 'water').length;
        const s = {id: water.preset + '_' + n, kind: 'water', preset: water.preset, at: [wx, wz]};
        if (water.radiusM > 0) s.radius = Math.round(water.radiusM * 10);
        sites.push(s);
        stroke.changed = true;
      }
      paint();
      return;
    }
    stroke = snapshot();
    stroke.changed = applyBrush(...cellAt(ev));
    paint();
  });
  c.addEventListener('pointermove', ev => {
    if (!map) return;
    const cell = cellAt(ev);
    els.hover = cell;
    if (panning) {
      view.cx = panning.cx - (ev.clientX - panning.x) / view.scale;
      view.cz = panning.cz - (ev.clientY - panning.y) / view.scale;
    } else if (padDrag) {
      const [wx, wz] = worldOfCell(cell[0] + 1, cell[1] + 1);
      const s = (map.json.sites || []).find(s => s.kind === 'pad');
      const box = {min: [Math.min(padDrag.x0, wx), Math.min(padDrag.z0, wz)],
                   max: [Math.max(padDrag.x0, wx) - 1, Math.max(padDrag.z0, wz) - 1]};
      if (s) { s.min = box.min; s.max = box.max; }
      else map.json.sites = [...(map.json.sites || []), {id: 'harness', kind: 'pad', ...box}];
      stroke.changed = true;
    } else if (waterDrag && ev.buttons & 1) {
      const [wx, wz] = worldColumnAt(ev);
      waterDrag.site.at = [wx + waterDrag.dx, wz + waterDrag.dz];
      stroke.changed = true;
    } else if (landDrag && ev.buttons & 1) {
      const [wx, wz] = worldColumnAt(ev);
      landDrag.site.at = [wx + landDrag.dx, wz + landDrag.dz];
      stroke.changed = true;
      syncSites();
    } else if (stroke && ev.buttons & 1) {
      if (applyBrush(...cell)) stroke.changed = true;
    }
    const [wx, wz] = worldOfCell(cell[0], cell[1]);
    const [w, h] = map.json.size;
    const inside = cell[0] >= 0 && cell[1] >= 0 && cell[0] < w && cell[1] < h;
    const i = cell[1] * w + cell[0];
    status(inside
      ? `cell (${cell[0]},${cell[1]})  world x ${wx}..${wx + (1 << map.json.cellLog2) - 1}  z ${wz}..  biome ${map.json.biomes[map.biome[i]]}  landform ${map.landform[i]}`
      : `outside the painted planes (ocean)`);
    paint();
  });
  const up = () => {
    panning = null;
    padDrag = null;
    waterDrag = null;
    landDrag = null;
    if (stroke) {
      if (stroke.changed) { undo.push(stroke); if (undo.length > 40) undo.shift(); redo = []; markDirty(true); }
      stroke = null;
    }
    paint();
  };
  c.addEventListener('pointerup', up);
  c.addEventListener('pointercancel', up);
  c.addEventListener('pointerleave', () => { els.hover = null; paint(); });
  c.addEventListener('wheel', ev => {
    if (!map) return;
    ev.preventDefault();
    const [cx, cz] = cellAt(ev);
    const f = ev.deltaY < 0 ? 1.25 : 0.8;
    const ns = Math.max(0.5, Math.min(64, view.scale * f));
    // zoom about the cursor: keep the cell under it fixed
    view.cx = cx + (view.cx - cx) * (view.scale / ns);
    view.cz = cz + (view.cz - cz) * (view.scale / ns);
    view.scale = ns;
    paint();
  }, {passive: false});
  window.addEventListener('keydown', ev => {
    if (!H.isVisible() || !map) return;
    const inField = /input|select|textarea/i.test(document.activeElement && document.activeElement.tagName);
    if (inField) return;
    if ((ev.ctrlKey || ev.metaKey) && ev.key === 'z') { ev.preventDefault(); doUndo(); }
    else if ((ev.ctrlKey || ev.metaKey) && ev.key === 'y') { ev.preventDefault(); doRedo(); }
    else if (ev.key === '[') { brush.radius = Math.max(0, brush.radius - 1); syncControls(); paint(); }
    else if (ev.key === ']') { brush.radius = Math.min(24, brush.radius + 1); syncControls(); paint(); }
  });
}
function doUndo() {
  if (!undo.length) return;
  redo.push(snapshot());
  restore(undo.pop());
  markDirty(true); paint();
}
function doRedo() {
  if (!redo.length) return;
  undo.push(snapshot());
  restore(redo.pop());
  markDirty(true); paint();
}

/** The declared landform whose footprint covers (wx, wz), or null. */
function landformAt(wx, wz) {
  return (map.json.sites || []).find(s => {
    if (s.kind !== 'landform' || !Array.isArray(s.at)) return false;
    const r = s.radius || 1024, dx = s.at[0] - wx, dz = s.at[1] - wz;
    return dx * dx + dz * dz <= r * r;
  }) || null;
}
/** The nearest site marker of any kind within a generous radius, or null. */
function siteAt(wx, wz) {
  const cv = 1 << map.json.cellLog2;
  const reach = Math.max(64, cv * 2 / Math.max(1, view.scale)) * 4;
  let best = null, bd = Infinity;
  for (const s of (map.json.sites || [])) {
    let at = Array.isArray(s.at) ? s.at : null;
    if (s.kind === 'stamp') at = [s.x || 0, s.z || 0];
    if (s.kind === 'pad' && Array.isArray(s.min) && Array.isArray(s.max)) at = [(s.min[0] + s.max[0]) / 2, (s.min[1] + s.max[1]) / 2];
    if (!at) continue;
    const d = Math.hypot(at[0] - wx, at[1] - wz);
    const r = s.kind === 'landform' ? (s.radius || 1024) : s.kind === 'water' ? ((s.radius || (water.geom[s.preset] || {}).radius || 60)) : reach;
    if (d <= Math.max(r, reach) && d < bd) { bd = d; best = s; }
  }
  return best;
}

/* ---- the Terrain section (P-G) --------------------------------------------------------- */
function buildTerrainSection() {
  const el = H.el;
  const det = el('details', {class: 'mapterrain'});
  det.append(el('summary', {}, 'Terrain — the numbers this map\u2019s ground is built from (map.json terrain; live, read by the height mirror on both sides)'));
  const grid = el('div', {class: 'mapterrain-grid'});
  els.terrainInputs = {};
  for (const [path, label, min, max, step, unit, desc] of TERRAIN_ROWS) {
    const inp = el('input', {type: 'number', min, max, step, style: 'width:80px'});
    inp.addEventListener('change', () => {
      if (!map) return;
      const v = Math.max(min, Math.min(max, Math.round(+inp.value || 0)));
      if (v === getPath(terrainOf(), path)) { inp.value = v; return; }
      undo.push(snapshot()); if (undo.length > 40) undo.shift(); redo = [];
      setPath(terrainOf(), path, v);
      inp.value = v;
      markDirty(true); publishTerrain(); paint();
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
  const t = terrainOf();
  for (const [path] of TERRAIN_ROWS) { const inp = els.terrainInputs[path]; if (inp) inp.value = getPath(t, path); }
  if (els.terrainRef) els.terrainRef.textContent = 'refVoxelsPerMetre ' + t.refVoxelsPerMetre + ' \u00b7 sea level y' + (map.json.seaLevelY | 0) +
      ' \u00b7 a painted 255 is ' + ((t.landformRangeVox / 2) / t.refVoxelsPerMetre).toFixed(0) + ' m above the datum, a painted 0 that far below';
}

/* ---- the SITES panel (P-I): every site by kind, select / rename / delete ---------------- */
function buildSitesPanel() {
  const el = H.el;
  const det = el('details', {class: 'mapsites', open: true});
  det.append(el('summary', {}, 'Sites — everything declared on this map (pad, spawn, water, landform, stamp)'));
  els.sitesList = el('div', {class: 'mapsites-list'});
  els.sitePanel = el('div', {class: 'mapsites-panel'});
  det.append(els.sitesList, els.sitePanel);
  return det;
}
function siteWhere(s) {
  if (s.kind === 'pad' && Array.isArray(s.min)) return '(' + s.min.join(',') + ')..(' + s.max.join(',') + ')';
  if (s.kind === 'stamp') return '(' + (s.x | 0) + ',' + (s.z | 0) + ')';
  return Array.isArray(s.at) ? '(' + s.at.join(',') + ')' : '';
}
function syncSites() {
  if (!map || !els.sitesList) return;
  const el = H.el;
  const sites = map.json.sites || (map.json.sites = []);
  els.sitesList.innerHTML = '';
  const order = {pad: 0, spawn: 1, water: 2, landform: 3, stamp: 4};
  const sorted = sites.slice().sort((a, b) => (order[a.kind] ?? 9) - (order[b.kind] ?? 9));
  for (const s of sorted) {
    const row = el('div', {class: 'mapsite' + (selected === s ? ' on' : '')});
    const kind = el('span', {class: 'mapkind mapkind-' + s.kind}, s.kind);
    const id = el('input', {type: 'text', value: s.id || '', class: 'mapid', title: 'rename'});
    id.addEventListener('change', () => {
      undo.push(snapshot()); if (undo.length > 40) undo.shift(); redo = [];
      s.id = id.value.trim() || s.kind;
      markDirty(true); syncSites(); paint();
    });
    const where = el('span', {class: 'mapwhere'}, siteWhere(s) +
      (s.kind === 'water' ? ' ' + (s.preset || '?') : s.kind === 'landform' ? ' ' + s.shape + ' ' + ((s.heightVox || 0) / 10).toFixed(0) + ' m' : s.kind === 'stamp' ? ' ' + (s.template || '?') : ''));
    const sel = el('button', {title: 'select on the map'}, selected === s ? 'selected' : 'select');
    sel.addEventListener('click', () => { selected = s; syncSites(); paint(); });
    const del = el('button', {title: 'delete this site'}, '\u00d7');
    del.addEventListener('click', () => {
      if (s.kind === 'pad' && !confirm('Delete the harness pad? The selftest fixtures stand on its ground.')) return;
      undo.push(snapshot()); if (undo.length > 40) undo.shift(); redo = [];
      sites.splice(sites.indexOf(s), 1);
      if (selected === s) selected = null;
      markDirty(true); syncSites(); paint();
    });
    row.append(kind, id, where, sel, del);
    els.sitesList.append(row);
  }
  if (!sorted.length) els.sitesList.append(el('div', {class: 'mapnote'}, 'no sites'));
  // the selected site's own panel
  const pnl = els.sitePanel;
  pnl.innerHTML = '';
  if (!selected) { pnl.append(el('div', {class: 'mapnote'}, 'Select a site to edit it. Towns and notable buildings will join this list.')); return; }
  const s = selected;
  const num = (label, key, min, max, step, title) => {
    const inp = el('input', {type: 'number', min, max, step, value: s[key] == null ? '' : s[key], style: 'width:80px', title});
    inp.addEventListener('change', () => {
      undo.push(snapshot()); if (undo.length > 40) undo.shift(); redo = [];
      s[key] = Math.max(min, Math.min(max, Math.round(+inp.value || 0)));
      markDirty(true); syncSites(); paint();
    });
    return el('label', {title}, label, ' ', inp);
  };
  pnl.append(el('b', {}, s.kind + ' \u201c' + (s.id || '') + '\u201d'));
  if (s.kind === 'landform') {
    const shape = el('select', {});
    for (const sh of LANDFORM_SHAPES) shape.append(el('option', {value: sh}, sh));
    shape.value = s.shape || 'peak';
    shape.addEventListener('change', () => {
      undo.push(snapshot()); if (undo.length > 40) undo.shift(); redo = [];
      s.shape = shape.value; if (s.shape !== 'ridge') delete s.rotation; else if (s.rotation == null) s.rotation = 0;
      markDirty(true); syncSites(); paint();
    });
    pnl.append(el('label', {title: 'peak: a cone; ridge: an elongated cone (width a third of the length) at a heading; basin: a cone sunk into the plane; plateau: flat inside 60% of the radius, ramped out'}, 'shape ', shape),
               num('radius (vox)', 'radius', 64, 1 << 20, 64, 'The footprint: a peak\u2019s radius, a ridge\u2019s half-length along its crest.'),
               num('height (vox)', 'heightVox', -100000, 100000, 10, 'How far the landform plane is lifted at the centre (a basin uses the magnitude and sinks). 600 = 60 m.'));
    if (s.shape === 'ridge') pnl.append(num('heading (deg)', 'rotation', -180, 360, 5, 'The crest\u2019s heading: 0 = along +X, 90 = along +Z.'));
    pnl.append(el('div', {class: 'mapnote'}, 'Overlaid onto the landform plane at load (Tier A): the same on every seed. Coordinates are world voxels; the plane\u2019s cell is ' + (1 << map.json.cellLog2) + ' vox, so a landform narrower than a cell cannot be resolved.'));
  } else if (s.kind === 'water') {
    pnl.append(el('span', {class: 'mapnote'}, 'preset ' + (s.preset || '?') + (s.radius ? ', radius ' + s.radius + ' vox' : ', the preset\u2019s radius') + ' \u2014 drag it with the Water tool.'));
  } else if (s.kind === 'spawn') {
    pnl.append(el('span', {class: 'mapnote'}, 'Where the game starts and the centre of the calm home area (Terrain \u2192 home area). Move it with the Spawn tool; the spawn-site gate checks it is on land, off the pad and off any lake.'));
  } else if (s.kind === 'pad') {
    pnl.append(el('span', {class: 'mapnote'}, 'The selftest harness box: no trunks, crowns, tarns or cover inside. Drag it with the Pad box tool.'));
  } else if (s.kind === 'stamp') {
    pnl.append(el('span', {class: 'mapnote'}, 'assets/prefabs/' + (s.template || '?') + '.vox' + (s.rot ? ', rotated ' + s.rot : '') + '. Shift+click its marker with the Stamp tool to delete.'));
  }
}

/* ---- the heightmap backdrop: the engine's own heights over the whole map --------------- */
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
async function fetchHeights() {
  if (!map) return;
  const [w, h] = map.json.size;
  const cv = 1 << map.json.cellLog2;
  const span = Math.max(w, h) * cv;
  const cx = ((w / 2) - map.json.originCell[0]) * cv, cz = ((h / 2) - map.json.originCell[1]) * cv;
  const res = Math.min(1024, Math.max(64, Math.max(w, h) * 2));
  status('rendering the engine\u2019s heights over the whole map (the SAVED file)\u2026');
  const r = await fetch('/api/heightmap?cx=' + Math.round(cx) + '&cz=' + Math.round(cz) + '&span=' + span + '&res=' + res + '&seed=1337', {cache: 'no-store'});
  if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
  const D = decodeHeightmap(await r.arrayBuffer());
  // shade: depth below sea in blues, land from the datum up in a ramp, hillshaded
  const img = new ImageData(D.res, D.res), px = img.data;
  const sea = map.json.seaLevelY | 0;
  const zScale = 2.0 * D.res / Math.max(D.span, 1);
  for (let j = 0; j < D.res; j++) for (let i = 0; i < D.res; i++) {
    const k = j * D.res + i, hh = D.h[k];
    const hx = D.h[j * D.res + Math.min(i + 1, D.res - 1)] - D.h[j * D.res + Math.max(i - 1, 0)];
    const hz = D.h[Math.min(j + 1, D.res - 1) * D.res + i] - D.h[Math.max(j - 1, 0) * D.res + i];
    let r, g, b;
    if (hh < sea) { const t = Math.min(1, (sea - hh) / 400); r = 40 - 20 * t; g = 90 - 40 * t; b = 150 - 40 * t; }
    else { const t = Math.min(1, (hh - sea) / 900); r = 90 + 140 * t; g = 130 + 100 * t; b = 80 + 150 * t; }
    const sh = Math.max(-1, Math.min(1, (-hx - hz) * zScale * 0.5)), k2 = 1 + sh * 0.6;
    const o = k * 4;
    px[o] = Math.max(0, Math.min(255, r * k2)); px[o + 1] = Math.max(0, Math.min(255, g * k2)); px[o + 2] = Math.max(0, Math.min(255, b * k2)); px[o + 3] = 255;
  }
  // where the sampled square sits, in cells
  const cells = D.span / cv;
  heights = {res: D.res, img, cell0x: map.json.originCell[0] + (D.cx - D.span / 2) / cv, cell0z: map.json.originCell[1] + (D.cz - D.span / 2) / cv,
             cellsW: cells, cellsH: cells, hMin: D.hMin, hMax: D.hMax};
  status('heights: y' + D.hMin + '..y' + D.hMax + ' over ' + (D.span / 10 / 1000).toFixed(1) + ' km (the SAVED map; save, then refresh, to see an edit)');
  showHeights = true;
  if (els.heightsBtn) els.heightsBtn.classList.add('on');
  paint();
}

/* ---- UI --------------------------------------------------------------------------- */
function rebuildPalette() {
  const el = H.el;
  els.palette.replaceChildren();
  map.json.biomes.forEach((name, i) => {
    const b = el('button', {class: 'mapchip' + (i === brush.index ? ' on' : ''), title: `paint ${name} (index ${i})`},
      el('span', {class: 'sw', style: `background:${colorOf(name)}`}), name);
    b.addEventListener('click', () => { brush.index = i; tool = 'biome'; syncControls(); rebuildPalette(); paint(); });
    els.palette.append(b);
  });
}
function syncControls() {
  for (const [k, b] of Object.entries(els.tools)) b.classList.toggle('on', k === tool);
  els.radius.value = brush.radius; els.radiusOut.textContent = brush.radius;
  els.land.value = brush.landform; els.landOut.textContent = brush.landform;
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
#env-map .mapchip{display:inline-flex;align-items:center;gap:5px;padding:3px 7px;border:1px solid #2a3040;border-radius:4px;background:#12161f;color:#c7d2e3;cursor:pointer;font:11px monospace}
#env-map .mapchip.on{border-color:#ffd866;color:#fff}
#env-map .mapchip .sw{width:12px;height:12px;border-radius:2px;display:inline-block}
#env-map .mapcanvas{flex:1;min-height:320px;width:100%;background:#0e1219;border:1px solid #2a3040;border-radius:6px;cursor:crosshair;touch-action:none}
#env-map .mapstatus{font:11px monospace;color:#8fa0b8;min-height:14px}
#env-map .mapnote{font:10px/1.4 monospace;color:#6f7f97}
#env-map .mapbody{display:flex;gap:8px;min-height:0;flex:1}
#env-map .mapmain{flex:1;min-width:0;display:flex;flex-direction:column;gap:6px}
#env-map .mapside{width:340px;flex:0 0 340px;overflow-y:auto;font:11px monospace;color:#c7d2e3;display:flex;flex-direction:column;gap:8px}
#env-map details{border:1px solid #2a3040;border-radius:6px;padding:4px 8px;background:#12161f}
#env-map details summary{cursor:pointer;color:#dbe4f0;font-weight:600;font-size:11px}
#env-map .mapterrain-grid{display:grid;grid-template-columns:1fr;gap:2px;margin:6px 0}
#env-map .mapterrain-grid label{display:flex;justify-content:space-between;align-items:center;gap:6px;font-size:11px}
#env-map .mapunit{color:#6f7f97;width:34px}
#env-map .mapsites-list{display:flex;flex-direction:column;gap:2px;margin:6px 0}
#env-map .mapsite{display:flex;gap:5px;align-items:center;padding:2px 4px;border-radius:4px}
#env-map .mapsite.on{background:#1f2a3d}
#env-map .mapsite button{padding:1px 6px;font-size:10px}
#env-map .mapkind{font-size:9px;text-transform:uppercase;letter-spacing:.06em;width:58px;color:#8fa0b8}
#env-map .mapkind-landform{color:#e8c477}
#env-map .mapkind-water{color:#5fc8ff}
#env-map .mapkind-spawn{color:#8dff9a}
#env-map .mapkind-pad{color:#ffb454}
#env-map .mapkind-stamp{color:#7fd4ff}
#env-map .mapid{width:110px;font:11px monospace;background:#0e1219;border:1px solid #2a3040;color:#dbe4f0;border-radius:3px;padding:1px 4px}
#env-map .mapwhere{flex:1;color:#8fa0b8;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#env-map .mapsites-panel{display:flex;flex-direction:column;gap:4px;border-top:1px solid #2a3040;padding-top:6px}
#env-map .mapsites-panel label{display:flex;justify-content:space-between;align-items:center;gap:6px}
#env-map #wgPreviewHost{border-top:1px solid #2a3040;padding-top:6px}
`;
  document.head.append(style);

  els.canvas = el('canvas', {class: 'mapcanvas'});
  els.status = el('div', {class: 'mapstatus'}, 'no map loaded');
  els.palette = el('div', {class: 'mapbar'});
  els.mapSel = el('select', {});
  els.save = el('button', {disabled: true}, 'Save map');
  els.save.addEventListener('click', saveMap);
  els.tools = {
    biome: el('button', {title: 'paint the selected biome (left-drag)'}, 'Biome brush'),
    landform: el('button', {title: 'paint landform 0..255 (left-drag)'}, 'Landform brush'),
    pad: el('button', {title: 'drag the harness pad box (world voxels)'}, 'Pad box'),
    spawn: el('button', {title: 'click: put the spawn site there (where the game starts; the calm home area centres on it). One per map.'}, 'Spawn'),
    stamp: el('button', {title: 'click: place a stamp site (a .vox from assets/prefabs/); shift+click a marker: delete it'}, 'Stamp site'),
    water: el('button', {title: 'click: place an AUTHORED LAKE (a water preset at that column, same on every seed); drag a lake to move it; shift+click: delete it'}, 'Water'),
    landform: el('button', {title: 'click: DECLARE A LANDFORM (a peak / ridge / basin / plateau overlaid onto the landform plane at load, same on every seed); drag one to move it; shift+click: delete it'}, 'Landform'),
    site: el('button', {title: 'click a site marker of any kind to select it in the Sites panel'}, 'Select'),
  };
  for (const [k, b] of Object.entries(els.tools)) b.addEventListener('click', () => { tool = k; syncControls(); paint(); });
  // The water tool's preset and radius (P-F). The preset list is
  // assets/water/ (the Water bodies page's own list); the geometry cache is
  // what draws each lake's footprint and band to scale.
  els.waterPreset = el('select', {title: 'the water preset a new lake wears (Environment > Water bodies)'});
  els.waterPreset.addEventListener('change', () => { water.preset = els.waterPreset.value; tool = 'water'; syncControls(); paint(); });
  els.waterRadius = el('input', {type: 'number', min: 0, step: 0.5, value: 0, style: 'width:64px', title: 'radius in metres for a NEW lake; 0 = the preset\'s own footprint radius'});
  els.waterRadius.addEventListener('change', () => { water.radiusM = Math.max(0, +els.waterRadius.value || 0); });
  // The landform tool's parameters for a NEW site (P-G); an existing one is
  // edited in the Sites panel.
  els.lfShape = el('select', {title: 'the shape a new landform takes'});
  for (const sh of LANDFORM_SHAPES) els.lfShape.append(el('option', {value: sh}, sh));
  els.lfShape.value = landform.shape;
  els.lfShape.addEventListener('change', () => { landform.shape = els.lfShape.value; tool = 'landform'; syncControls(); paint(); });
  els.lfRadius = el('input', {type: 'number', min: 64, max: 1 << 20, step: 64, value: landform.radius, style: 'width:70px', title: 'radius in voxels for a NEW landform (a ridge: the crest\u2019s half-length)'});
  els.lfRadius.addEventListener('change', () => { landform.radius = Math.max(64, +els.lfRadius.value || 64); });
  els.lfHeight = el('input', {type: 'number', min: -100000, max: 100000, step: 10, value: landform.heightVox, style: 'width:70px', title: 'height in voxels for a NEW landform: 600 = 60 m'});
  els.lfHeight.addEventListener('change', () => { landform.heightVox = Math.round(+els.lfHeight.value || 0); });
  els.lfRot = el('input', {type: 'number', min: -180, max: 360, step: 5, value: landform.rotation, style: 'width:56px', title: 'a new ridge\u2019s heading in degrees (0 = along +X, 90 = along +Z)'});
  els.lfRot.addEventListener('change', () => { landform.rotation = Math.round(+els.lfRot.value || 0); });
  els.heightsBtn = el('button', {title: 'draw the engine\u2019s own heights (World::TerrainColumn over the SAVED map, --heightmap) under the biomes; click again to hide'}, 'Heights');
  els.heightsBtn.addEventListener('click', () => {
    if (showHeights) { showHeights = false; els.heightsBtn.classList.remove('on'); paint(); return; }
    fetchHeights().catch(e => { toast('heights: ' + e.message, true); status('heights failed: ' + e.message); });
  });
  // The map and edit layer the GAME loads (tuning.json world.mapLayer /
  // editLayer): the selectors write tuning through the host, saved with
  // Ctrl+S or the tuner's Save.
  els.editSel = el('select', {title: 'assets/worldedits/<name>.svedit, applied over worldgen (tuning.json world.editLayer); blank = none'});
  els.editSel.addEventListener('change', () => { editLayerWanted = els.editSel.value; pushTuning(); });
  els.radius = el('input', {type: 'range', min: 0, max: 24, value: brush.radius, style: 'width:110px'});
  els.radiusOut = el('span', {}, String(brush.radius));
  els.radius.addEventListener('input', () => { brush.radius = +els.radius.value; syncControls(); paint(); });
  els.land = el('input', {type: 'range', min: 0, max: 255, value: brush.landform, style: 'width:140px'});
  els.landOut = el('span', {}, String(brush.landform));
  els.land.addEventListener('input', () => { brush.landform = +els.land.value; tool = 'landform'; syncControls(); });
  const showLf = el('input', {type: 'checkbox', checked: true});
  showLf.addEventListener('change', () => { showLandform = showLf.checked; paint(); });
  const fitBtn = el('button', {}, 'Fit');
  fitBtn.addEventListener('click', () => { fit(); paint(); });
  const undoBtn = el('button', {title: 'Ctrl+Z'}, 'Undo');
  undoBtn.addEventListener('click', doUndo);
  const redoBtn = el('button', {title: 'Ctrl+Y'}, 'Redo');
  redoBtn.addEventListener('click', doRedo);

  const bar = el('div', {class: 'mapbar'},
    el('label', {title: 'the map the GAME loads (tuning.json world.mapLayer)'}, 'map ', els.mapSel),
    el('label', {title: 'the edit layer the game applies over worldgen (tuning.json world.editLayer)'}, ' edits ', els.editSel),
    els.save, undoBtn, redoBtn, fitBtn, els.heightsBtn,
    el('span', {style: 'width:10px'}),
    els.tools.site, els.tools.biome, els.tools.landform, els.tools.pad, els.tools.spawn, els.tools.stamp, els.tools.water,
    el('label', {}, ' lake ', els.waterPreset, ' r ', els.waterRadius, ' m'),
    el('label', {}, ' new landform ', els.lfShape, ' r ', els.lfRadius, ' h ', els.lfHeight, ' \u00b0 ', els.lfRot),
    el('label', {}, ' brush ', els.radius, ' ', els.radiusOut),
    el('label', {}, ' landform paint ', els.land, ' ', els.landOut),
    el('label', {}, ' ', showLf, ' shade by landform'));
  const note = el('div', {class: 'mapnote'},
    'Tier A: what you paint and declare here is where the biomes and the mountains ARE on every seed; the boundary warp and everything inside a region take the seed. ' +
    'Cells are 2^cellLog2 voxels (102.4 m). Right/middle-drag pans, wheel zooms, [ ] resize the brush. ' +
    'The pad box is the selftest harness region (no trunks, crowns, tarns or cover inside); the spawn diamond is where the game starts and the centre of the calm home area (Terrain \u2192 home area) -- keep it outside the pad, on land, or the spawn-site gate says so. ' +
    'A stamp site places assets/prefabs/<name>.vox on a levelled pad at that column; the engine refuses to start if the .vox is missing. ' +
    'A water site is an AUTHORED LAKE: the chosen preset (Environment > Water bodies) carved at that column on every seed. ' +
    'A landform site is a DECLARED mountain: a peak, ridge, basin or plateau overlaid onto the landform plane at load. ' +
    'Saving writes assets/worldmap/<name>/ and moves the world hash; regenerate the world (F7 in the game, or Apply in the sidebar) to see it.');
  const main = el('div', {class: 'mapmain'}, bar, els.palette, els.canvas, els.status, note);
  const side = el('div', {class: 'mapside'}, buildTerrainSection(), buildSitesPanel());
  root.append(el('div', {class: 'mapbody'}, main, side));
  // The PREVIEW pane (P-I): the heightmap and voxel views that were the
  // Worldgen tab, re-homed under the map as previews. tuner.html keeps their
  // markup and glue in #wgPreviewHost; this page adopts the node and the
  // one-click Vegetation switch beside its regen note lives in its toolbar.
  const pv = document.getElementById('wgPreviewHost');
  if (pv) {
    const head = el('div', {class: 'mapbar'},
      el('b', {}, 'Preview'),
      el('span', {class: 'mapnote'}, ' the engine\u2019s heights (--heightmap) or its real voxels (--voxserve) for the SAVED files: previews, not authoring. ' +
         'Regen note: the game re-reads the map, the biomes and the trees on F7 / Apply; the Vegetation button is the dev A/B switch (tuning.json debug.vegetation).'));
    const pvToggle = el('button', {}, 'Show preview');
    pvToggle.addEventListener('click', () => {
      const on = pv.style.display === 'none';
      pv.style.display = on ? '' : 'none';
      pvToggle.textContent = on ? 'Hide preview' : 'Show preview';
      if (on && window.wgPreviewShow) window.wgPreviewShow();
    });
    head.prepend(pvToggle);
    main.append(head, pv);
    pv.style.display = 'none';
  }
  wire();
  new ResizeObserver(() => paint()).observe(els.canvas);

  els.mapSel.addEventListener('change', () => {
    mapLayerWanted = els.mapSel.value;
    pushTuning();
    loadMap(els.mapSel.value).catch(e => toast('load failed: ' + e.message, true));
  });
  refreshEditLayers();
  // The water presets, and each one's radius + band in voxels (the engine's
  // rounding: 10 voxels to the metre, band = max(shore.band, berm.width)).
  loadWaterPresets().then(() => paint()).catch(e => status('water presets: ' + e.message));
  listMaps().then(names => {
    els.mapSel.replaceChildren(...names.map(n => el('option', {value: n}, n)));
    const want = currentMapLayer() || names[0];
    if (want && names.includes(want)) els.mapSel.value = want;
    if (els.mapSel.value) return loadMap(els.mapSel.value);
    status('no maps under assets/worldmap/ — run python scripts/seed_worldmap.py');
  }).catch(e => status('map list failed: ' + e.message));
  syncControls();
}
export function isDirty() { return dirty; }

/* ---- tuning.json world.* through the host (the map + edit layer the game loads) ---- */
function tuningWorld() {
  const T = H.tuning && H.tuning();
  if (!T || !T.tune) return null;
  if (!T.tune.world || typeof T.tune.world !== 'object') T.tune.world = {mapLayer: 'default', editLayer: ''};
  return T;
}
function currentMapLayer() {
  const T = tuningWorld();
  return T ? (T.tune.world.mapLayer || 'default') : '';
}
function pushTuning() {
  const T = tuningWorld();
  if (!T) { toast('tuning.json is not loaded: the game keeps its current map / edit layer', true); return; }
  let changed = false;
  if (mapLayerWanted && T.tune.world.mapLayer !== mapLayerWanted) { T.tune.world.mapLayer = mapLayerWanted; changed = true; }
  if (editLayerWanted !== T.tune.world.editLayer && (editLayerWanted || T.tune.world.editLayer)) { T.tune.world.editLayer = editLayerWanted; changed = true; }
  if (changed) {
    tuningTouched = true;
    if (T.touchTune) T.touchTune();
    toast('tuning.json world.mapLayer = ' + T.tune.world.mapLayer + (T.tune.world.editLayer ? ', editLayer = ' + T.tune.world.editLayer : '') + ' \u2014 save (Ctrl+S) and press F7 in the game');
  }
}
async function refreshEditLayers() {
  if (!els.editSel) return;
  const cur = (tuningWorld() && tuningWorld().tune.world.editLayer) || '';
  try {
    const r = await fetch('/api/worldedits', {cache: 'no-store'});
    const j = r.ok ? await r.json() : {layers: []};
    els.editSel.replaceChildren(H.el('option', {value: ''}, '(none)'), ...(j.layers || []).map(L => H.el('option', {value: L.name}, L.name)));
  } catch (e) { els.editSel.replaceChildren(H.el('option', {value: ''}, '(none)')); }
  els.editSel.value = cur;
  editLayerWanted = cur;
}
/** tuning.json arrived (or changed): the selectors follow it. */
export function tuningAvailable() {
  const T = tuningWorld();
  if (!T) return;
  if (els.mapSel && T.tune.world.mapLayer && [...els.mapSel.options].some(o => o.value === T.tune.world.mapLayer) &&
      els.mapSel.value !== T.tune.world.mapLayer && !mapLayerWanted) {
    els.mapSel.value = T.tune.world.mapLayer;
    loadMap(els.mapSel.value).catch(() => {});
  }
  refreshEditLayers();
}
/** Ctrl+S on the map page: the map if it is dirty, and tuning if a selector moved. */
export function saveFromHost() {
  if (dirty) saveMap();
  if (tuningTouched && H.saveTuning) { H.saveTuning(); tuningTouched = false; }
}
export function activate() { syncTerrain(); syncSites(); publishTerrain(); }
// test seams
export function _map() { return map; }
export function _selected() { return selected; }
export function _select(site) { selected = site; syncSites(); paint(); }
