/* water.js — the Water bodies page of the Environment tab.
 *
 * WHAT IT IS. A preset library (assets/water/<name>.json) of bodies of standing
 * water — tarn, kettle, marsh, spring pool, oasis, playa, crater lake, lava
 * pool — each edited through a slider column driven by the schema tables below,
 * with three live views of exactly what watergen.js produces:
 *
 *   * a 3D voxel view (WorldView, the same renderer as the Trees tab and the
 *     Worldgen tab), with an optional quad of four seeds;
 *   * a PLAN: top-down depth and zone map — open water, emergent band,
 *     floating band, mud ring, shore fringe, berm, islands, dry bed;
 *   * a SECTION: the bathymetry profile through the centre, which is also the
 *     curve editor. Drag a point, click to add one, double-click to remove.
 *
 * WHY THREE VIEWS. A shoreline is judged from above (is the outline organic?
 * where do the reeds sit?), a bathymetry from the side (is there a shelf? does
 * the drop-off read?), and the whole thing in 3D against a player's height.
 * Each view answers a question the other two cannot.
 *
 * THE PAGE WORKS WITH NO BUILT EXE: generation is pure JS, the palette comes
 * from the materials.json the page already holds, files go through the
 * ordinary /api/model routes (the server allowlists `water`).
 */

import * as WG from './watergen.js';
import * as VOX from './vox.js';
import * as UI from './envui.js';

const PAGE = 'env-water';
const CLS = 'wb';

let H = null;
let wv = null;
let params = null;
let presetName = '';
let seed = 0;
let quad = false;
let dirty = false;
let genTimer = 0;
let lastResult = null;
let els = {};
let widgets = [];
let undo = null;
let framed = false;

/* ===========================================================================
 * THE SCHEMA — every editable value, one row each. Lengths in metres.
 * ======================================================================== */

const FOOTPRINT_ROWS = [
  {k: 'radius', n: 'radius (m)', min: 0.5, max: 30, step: 0.1, u: 'm',
   d: 'Half the mean width. The whole body scales with it: lobes, islands and the ' +
      'shore warp are all authored as fractions of this.'},
  {k: 'radiusV', n: 'radius variance (m)', min: 0, max: 20, step: 0.1, u: 'm',
   d: 'Per-instance jitter added on top, so two tarns in one valley are not the same tarn.'},
  {k: 'aspect', n: 'aspect', min: 0.3, max: 3, step: 0.05,
   d: 'Stretch along the body’s own axis. 1 is round; an oxbow is 2.5 with a rotation.'},
  {k: 'squareness', n: 'squareness', min: 0.8, max: 6, step: 0.1,
   d: 'The superellipse exponent. 2 is an ellipse, 4 a squircle (a flooded quarry), ' +
      '1.2 a diamond. This is a whole family of outlines in one number.'},
  {k: 'rotation', n: 'rotation (deg)', min: -180, max: 180, step: 1, u: '°',
   d: 'Orientation of the long axis. Added to the random roll when that is on.'},
  {k: 'warpAmp', n: 'shore warp', min: 0, max: 0.6, step: 0.01,
   d: 'How far the shoreline wanders, as a fraction of the radius. 0 is a drawn ' +
      'ellipse; 0.15 reads as a natural pond; 0.4 is a marsh that has forgotten its shape.'},
  {k: 'warpFreq', n: 'warp frequency', min: 0.5, max: 8, step: 0.1,
   d: 'Bays per diameter. Low is a few big lobes; high is a crinkled edge.'},
  {k: 'warpOctaves', n: 'warp octaves', min: 1, max: 5, step: 1, int: true,
   d: 'Detail levels in the warp. Each octave halves in size and amplitude.'},
  {k: 'lobes', n: 'lobes', min: 0, max: 8, step: 1, int: true,
   d: 'Extra ellipses welded onto the main outline — the bays and arms that no ' +
      'single warped disc can make. 0 for a simple pond, 3 for a ragged marsh.'},
  {k: 'lobeRadius', n: 'lobe radius (x r)', min: 0.1, max: 1.2, step: 0.05, d: ''},
  {k: 'lobeSpread', n: 'lobe spread (x r)', min: 0.2, max: 1.6, step: 0.05,
   d: 'How far from the centre the lobes sit. Past 1 they become separate pools ' +
      'joined by a neck.'},
  {k: 'lobeWeld', n: 'lobe weld', min: 0, max: 1, step: 0.05,
   d: 'Smooth-min radius between lobe and body. 0 leaves a cusp at the join; ' +
      '0.4 rounds it into a bay.'},
  {k: 'islands', n: 'islands', min: 0, max: 6, step: 1, int: true,
   d: 'Domes of natural ground left standing in the water.'},
  {k: 'islandRadius', n: 'island radius (x r)', min: 0.05, max: 0.6, step: 0.01, d: ''},
  {k: 'islandHeight', n: 'island height (m)', min: 0.1, max: 3, step: 0.1, u: 'm',
   d: 'How far the dome peak stands above the waterline.'}
];

const BATHY_ROWS = [
  {k: 'depth', n: 'max depth (m)', min: 0.1, max: 12, step: 0.1, u: 'm',
   d: 'At the deepest point. The player is 1.7 m: under about 2 m a body can only be ' +
      'waded, so this is the knob that decides whether it is swimmable.'},
  {k: 'rimDepth', n: 'rim depth (m)', min: 0, max: 4, step: 0.05, u: 'm',
   d: 'The drop right at the shoreline. Small wades in off a beach; large steps ' +
      'off a wall. The engine clamps its version against the angle of repose so ' +
      'the sand bed does not avalanche forever — keep rim depth under half the ' +
      'radius here too.'},
  {k: 'floorNoise', n: 'floor noise (m)', min: 0, max: 1.5, step: 0.05, u: 'm',
   d: 'Bumps on the bed, fading to nothing at the shore.'},
  {k: 'floorNoiseFreq', n: 'floor noise freq', min: 0.5, max: 8, step: 0.1, d: ''}
];

const FILL_ROWS = [
  {k: 'level', n: 'water level (m)', min: -6, max: 1, step: 0.05, u: 'm',
   d: 'Relative to the rim ground. 0 is brim-full; negative exposes a band of bed ' +
      '(a drought shore); the engine\'s berm always stands above whatever this is.'}
];

const BERM_ROWS = [
  {k: 'height', n: 'berm height (m)', min: 0, max: 4, step: 0.05, u: 'm',
   d: 'How far above the waterline the bank just outside the shore is FORCED. ' +
      'This is what contains the water in the engine — structural, not a ' +
      'sampling density. Keep it under the shore lift or nothing near the pond ' +
      'can be shore.'},
  {k: 'width', n: 'berm width (m)', min: 0.1, max: 8, step: 0.1, u: 'm',
   d: 'Over how far the berm ramps back down to natural ground.'},
  {k: 'coreFrac', n: 'berm core', min: 0, max: 1, step: 0.05,
   d: 'Inner fraction of the width held flat at full height. The engine uses a quarter.'}
];

const BED_ROWS = [
  {k: 'shallowDepth', n: 'shallow bed to (m)', min: 0, max: 6, step: 0.1, u: 'm',
   d: 'Water shallower than this gets the shallow bed material; deeper gets the deep one.'},
  {k: 'thickness', n: 'bed thickness (m)', min: 0.1, max: 2, step: 0.1, u: 'm', d: ''}
];

const GROUND_ROWS = [
  {k: 'soilDepth', n: 'soil depth (m)', min: 0, max: 4, step: 0.1, u: 'm', d: ''},
  {k: 'relief', n: 'preview relief (m)', min: 0, max: 2, step: 0.05, u: 'm',
   d: 'PREVIEW ONLY. Gentle noise on the surrounding ground so you can see how the ' +
      'berm meets uneven terrain. Not part of the preset the engine will read.'},
  {k: 'reliefFreq', n: 'preview relief freq', min: 0.2, max: 4, step: 0.1, d: ''}
];

const SHORE_ROWS = [
  {k: 'band', n: 'shore band (m)', min: 0, max: 8, step: 0.1, u: 'm',
   d: 'How far past the shoreline the wet fringe reaches. 0 turns the shoreline off.'},
  {k: 'lift', n: 'shore lift (m)', min: 0, max: 4, step: 0.05, u: 'm',
   d: 'How far ABOVE the waterline ground may stand and still count as shore. This ' +
      'is what keeps the reed beds in the shallow bays and leaves a steep bank dry ' +
      '— the highest-leverage knob on the fringe.'},
  {k: 'mudWidth', n: 'mud ring (m)', min: 0, max: 4, step: 0.1, u: 'm',
   d: 'Inner ring of the fringe that wears the mud skin instead of the biome’s.'},
  {k: 'mossChance', n: 'wet moss 1-in-N', min: 0, max: 20, step: 1, int: true,
   d: 'One in this many mud-ring columns wears wet moss instead. 0 never.'}
];

const EMERGENT_ROWS = [
  {k: 'chance', n: 'reed 1-in-N', min: 0, max: 400, step: 1, int: true,
   d: 'One in this many columns in the emergent band grows a reed. 0 never. ' +
      'Real cattails and bulrushes stop at about 0.7 m of water; the band below ' +
      'is where they live.'},
  {k: 'minDepth', n: 'from depth (m)', min: 0, max: 4, step: 0.05, u: 'm', d: ''},
  {k: 'maxDepth', n: 'to depth (m)', min: 0, max: 6, step: 0.05, u: 'm', d: ''},
  {k: 'height', n: 'reed height (m)', min: 0.2, max: 4, step: 0.1, u: 'm',
   d: 'From the BED. Reeds are meant to break the waterline, so this should exceed the band’s depth.'}
];
const FLOATING_ROWS = [
  {k: 'chance', n: 'lily 1-in-N', min: 0, max: 400, step: 1, int: true,
   d: 'One in this many columns in the floating band carries a pad on the surface. ' +
      'Water lilies root in 0.3-2 m of water.'},
  {k: 'flowerChance', n: 'flower 1-in-N pads', min: 0, max: 40, step: 1, int: true, d: ''},
  {k: 'minDepth', n: 'from depth (m)', min: 0, max: 6, step: 0.05, u: 'm', d: ''},
  {k: 'maxDepth', n: 'to depth (m)', min: 0, max: 10, step: 0.05, u: 'm', d: ''}
];
const SUBMERGED_ROWS = [
  {k: 'chance', n: 'weed 1-in-N', min: 0, max: 400, step: 1, int: true,
   d: 'One in this many columns deeper than the band start grows a submerged weed.'},
  {k: 'minDepth', n: 'from depth (m)', min: 0, max: 8, step: 0.05, u: 'm', d: ''},
  {k: 'height', n: 'weed height (m)', min: 0.2, max: 6, step: 0.1, u: 'm', d: ''},
  {k: 'clearance', n: 'surface clearance (m)', min: 0, max: 3, step: 0.05, u: 'm',
   d: 'Weeds are held this far under the surface regardless, so tall weed only ' +
      'appears in the deep middle.'}
];

const PLACEMENT_ROWS = [
  {k: 'tile', n: 'default tile (m)', min: 4, max: 400, step: 0.8, u: 'm',
   d: 'Metres between candidate sites — one body per tile at most. The DEFAULT a ' +
      'biome row inherits; each biome can override it.'},
  {k: 'rarity', n: 'default rarity 1-in-N', min: 0, max: 64, step: 1, int: true,
   d: 'One in this many tiles actually gets one. Shown on the biome page as a ' +
      'percentage and a count per hectare, because "1 in 4 of 44.8 m tiles" is ' +
      'not a number anyone can picture.'},
  {k: 'maxSlope', n: 'default max slope (Q8)', min: 0, max: 512, step: 8, int: true,
   d: 'Steepest landform this body may sit on, in the engine’s Q8 units (256 = ' +
      '45° = the angle of repose). A bowl cut into a slope lays its sand bed on a ' +
      'wall and the chunk never sleeps.'},
  {k: 'minY', n: 'lowest ground Y', min: -1, max: 300, step: 1, int: true, d: '-1 for no bound.'},
  {k: 'maxY', n: 'highest ground Y', min: -1, max: 300, step: 1, int: true, d: '-1 for no bound.'}
];

const PROFILE_PRESETS = [
  ['parabola', [[0, 1], [0.5, 0.75], [1, 0]]],
  ['bathtub', [[0, 1], [0.6, 0.98], [0.85, 0.7], [1, 0]]],
  ['shelf', [[0, 1], [0.45, 0.92], [0.7, 0.4], [0.88, 0.32], [1, 0]]],
  ['cone', [[0, 1], [1, 0]]],
  ['flat', [[0, 1], [1, 1]]]
];

/* ===========================================================================
 * generation
 * ======================================================================== */
const QUAD_GAP = 6;

function ctx() {
  return {
    el: H.el, cls: CLS, params: () => params, widgets,
    snapshot: (k) => undo.snapshot(k),
    onChange: () => { markDirty(); regenerate(false); }
  };
}

function regenerate(now) {
  clearTimeout(genTimer);
  const run = () => {
    if (!params) return;
    const t0 = performance.now();
    const seeds = quad ? [seed, seed + 1, seed + 2, seed + 3] : [seed];
    let results;
    try {
      results = seeds.map(s => WG.generateWaterBody(params, s, {vpm: vpm()}));
    } catch (e) {
      els.stats.textContent = 'generate failed: ' + (e && e.message || e);
      console.error(e);
      return;
    }
    lastResult = results[0];
    const mats = H.materials() || [];
    const missing = [];
    if (wv) {
      let span = 1, high = 1;
      for (const r of results) { span = Math.max(span, r.dim.x); high = Math.max(high, r.dim.y); }
      const pitch = span + QUAD_GAP;
      const placed = results.map((r, i) => ({
        res: r, origin: [(i & 1) * pitch, 0, (i >> 1) * pitch]
      }));
      wv.setLocalRegions(placed.map(p => ({
        cells: UI.toViewerCells(p.res, mats, missing),
        nx: p.res.dim.x, ny: p.res.dim.y, nz: p.res.dim.z, origin: p.origin
      })));
      const wide = quad ? pitch + span : span;
      if (!framed) {
        UI.frame(wv, {x: wide, y: high, z: wide}, [0, 0, 0], 0.5);
        wv.cam.pitch = -0.55;
        framed = true;
      }
    } else {
      results.forEach(r => UI.toViewerCells(r, mats, missing));
    }
    drawPlan(lastResult);
    drawProfile();
    const m = lastResult.meta;
    const plants = Object.entries(m.plants).map(([k, v]) => k + ' ' + v).join(', ');
    els.stats.innerHTML =
      (quad ? `<b>quad</b> seeds ${seeds.join(', ')} · ` : '') +
      `<b>${lastResult.dim.x}×${lastResult.dim.y}×${lastResult.dim.z}</b> · ` +
      `r ${m.radiusM.toFixed(1)} m · ` +
      (m.wet ? `surface <b>${m.areaM2.toFixed(0)} m²</b> · volume <b>${m.volumeM3.toFixed(0)} m³</b> · ` +
               `depth max ${m.maxDepthM.toFixed(2)} / mean ${m.meanDepthM.toFixed(2)} m · ` +
               `Vd ${m.volumeDevelopment.toFixed(2)} · shore ${m.shoreCells} · berm ${m.bermCells} · `
             : '<span class="warn">DRY BASIN</span> · ') +
      `${(m.voxels).toLocaleString()} voxels · ${Math.round(performance.now() - t0)} ms` +
      (plants ? '<br>' + plants : '') +
      (m.clipped ? ' · <span class="warn">CLIPPED (over ' + WG.MAX_DIM + ' cells on an axis)</span>' : '') +
      (missing.length ? ' · <span class="warn">materials.json has no ' + missing.join(', ') + '</span>' : '');
  };
  if (now) run(); else genTimer = setTimeout(run, 80);
}

function vpm() { return (H.voxelsPerMetre && H.voxelsPerMetre()) || WG.DEFAULT_VOX_PER_M; }

/* ===========================================================================
 * PLAN — top-down zones and depth
 * ======================================================================== */
const ZONE_COLOUR = {
  0: null,                 // land: shaded by height
  1: '#6b7a4a',            // shore fringe
  2: '#3e8a5a',            // emergent band
  3: '#2f6f8f',            // floating band
  4: '#1f4f8a',            // open water (depth-shaded)
  5: '#8a7a55',            // berm
  6: '#7f9a5a',            // island
  7: '#5a4a3a',            // mud ring
  8: '#a89a7a'             // dry bed
};

function drawPlan(res) {
  const cv = els.plan;
  if (!cv || !res) return;
  const W = cv.width, Hh = cv.height;
  const g = cv.getContext('2d');
  g.fillStyle = '#0e1116'; g.fillRect(0, 0, W, Hh);
  const {nx, nz, depth, zone} = res.plan;
  const s = Math.min(W / nx, Hh / nz);
  const ox = (W - nx * s) / 2, oz = (Hh - nz * s) / 2;
  const maxD = Math.max(0.01, res.meta.maxDepthM);
  const img = g.createImageData(nx, nz);
  const px = img.data;
  const hex = (h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
  const ZC = {};
  for (const k in ZONE_COLOUR) ZC[k] = ZONE_COLOUR[k] ? hex(ZONE_COLOUR[k]) : null;
  for (let z = 0; z < nz; z++)
    for (let x = 0; x < nx; x++) {
      const i = z * nx + x, zn = zone[i], d = depth[i];
      let c;
      if (zn === 4 || zn === 2 || zn === 3) {
        // water: darker with depth, tinted by band
        const t = Math.min(1, d / maxD);
        const base = ZC[zn];
        c = [base[0] * (1 - 0.6 * t), base[1] * (1 - 0.6 * t), base[2] * (1 - 0.35 * t)];
      } else if (zn === 0) {
        // land: height above the rim ground
        const h = -d;
        const t = Math.max(-1, Math.min(1, h / 2));
        c = [70 + 40 * t, 96 + 30 * t, 52 + 20 * t];
      } else c = ZC[zn];
      const o = i * 4;
      px[o] = c[0]; px[o + 1] = c[1]; px[o + 2] = c[2]; px[o + 3] = 255;
    }
  // Upscale through an offscreen canvas so the nearest-neighbour cells stay crisp.
  const off = document.createElement('canvas');
  off.width = nx; off.height = nz;
  off.getContext('2d').putImageData(img, 0, 0);
  g.imageSmoothingEnabled = false;
  g.drawImage(off, ox, oz, nx * s, nz * s);
  // Depth contours every 0.5 m, drawn where a cell's rounded band differs from its neighbour's.
  g.strokeStyle = 'rgba(255,255,255,0.22)';
  g.lineWidth = 1;
  g.beginPath();
  for (let z = 1; z < nz; z++)
    for (let x = 1; x < nx; x++) {
      const i = z * nx + x;
      const wet = (k) => zone[k] === 4 || zone[k] === 2 || zone[k] === 3;
      if (!wet(i)) continue;
      const b = Math.floor(depth[i] * 2);
      if (wet(i - 1) && Math.floor(depth[i - 1] * 2) !== b) {
        g.moveTo(ox + x * s, oz + z * s); g.lineTo(ox + x * s, oz + (z + 1) * s);
      }
      if (wet(i - nx) && Math.floor(depth[i - nx] * 2) !== b) {
        g.moveTo(ox + x * s, oz + z * s); g.lineTo(ox + (x + 1) * s, oz + z * s);
      }
    }
  g.stroke();
  // Scale bar: 5 m.
  const v = vpm();
  const bar = 5 * v * s;
  g.fillStyle = '#dbe4f0'; g.fillRect(10, Hh - 14, bar, 2);
  g.font = '10px monospace'; g.fillText('5 m', 10, Hh - 18);
  // Legend.
  const legend = [[4, 'water'], [2, 'emergent'], [3, 'floating'], [7, 'mud'], [1, 'shore'], [5, 'berm'], [6, 'island'], [8, 'dry bed']];
  let ly = 12;
  for (const [zn, name] of legend) {
    g.fillStyle = ZONE_COLOUR[zn]; g.fillRect(W - 78, ly - 8, 9, 9);
    g.fillStyle = '#9fb0c8'; g.fillText(name, W - 65, ly);
    ly += 12;
  }
}

/* ===========================================================================
 * PROFILE — the bathymetry curve editor and a live cross-section
 * ======================================================================== */
const PAD = {l: 34, r: 10, t: 10, b: 18};
let dragIdx = -1;

function profilePts() { return params.bathymetry.profile; }

function toCanvas(cv, u, f) {
  const w = cv.width - PAD.l - PAD.r, h = cv.height - PAD.t - PAD.b;
  return [PAD.l + u * w, PAD.t + f * h];          // f = 1 (deep) at the BOTTOM
}
function fromCanvas(cv, x, y) {
  const w = cv.width - PAD.l - PAD.r, h = cv.height - PAD.t - PAD.b;
  return [Math.min(1, Math.max(0, (x - PAD.l) / w)), Math.min(1, Math.max(0, (y - PAD.t) / h))];
}

function drawProfile() {
  const cv = els.profile;
  if (!cv || !params) return;
  const g = cv.getContext('2d');
  const W = cv.width, Hh = cv.height;
  g.fillStyle = '#0e1116'; g.fillRect(0, 0, W, Hh);
  const B = params.bathymetry;
  const pts = WG.sanitizeProfile(B.profile);
  // Axes: u along x (0 centre .. 1 shore), depth down.
  g.strokeStyle = '#2a3040'; g.lineWidth = 1;
  g.beginPath();
  g.moveTo(PAD.l, PAD.t); g.lineTo(PAD.l, Hh - PAD.b); g.lineTo(W - PAD.r, Hh - PAD.b);
  g.stroke();
  // Waterline.
  const [, y0] = toCanvas(cv, 0, 0);
  g.strokeStyle = '#5aa9e6'; g.setLineDash([3, 3]);
  g.beginPath(); g.moveTo(PAD.l, y0); g.lineTo(W - PAD.r, y0); g.stroke();
  g.setLineDash([]);
  // Filled section: the depth in metres mapped so full depth = bottom.
  g.beginPath();
  g.moveTo(PAD.l, y0);
  for (let i = 0; i <= 100; i++) {
    const u = i / 100;
    const f = WG.profileAt(pts, u);
    const dM = B.rimDepth + (B.depth - B.rimDepth) * f;
    const fr = Math.max(0, Math.min(1, dM / Math.max(0.01, B.depth)));
    const [x, y] = toCanvas(cv, u, fr);
    g.lineTo(x, y);
  }
  const [xr] = toCanvas(cv, 1, 0);
  g.lineTo(xr, y0);
  g.closePath();
  g.fillStyle = 'rgba(31,79,138,0.55)'; g.fill();
  g.strokeStyle = '#a89a7a'; g.lineWidth = 1.5; g.stroke();
  // Control points (in curve space, not metres).
  for (let i = 0; i < pts.length; i++) {
    const [x, y] = toCanvas(cv, pts[i][0], pts[i][1]);
    g.fillStyle = i === dragIdx ? '#ffb454' : '#dbe4f0';
    g.beginPath(); g.arc(x, y, 4, 0, Math.PI * 2); g.fill();
  }
  g.fillStyle = '#7c8ba3'; g.font = '10px monospace';
  g.fillText('centre', PAD.l, Hh - 5);
  g.fillText('shore', W - PAD.r - 30, Hh - 5);
  g.save(); g.translate(10, Hh / 2); g.rotate(-Math.PI / 2);
  g.fillText(B.depth.toFixed(1) + ' m', -14, 0); g.restore();
  // Band markers: where the emergent/floating/submerged bands start, as depth lines.
  const bands = [[params.aquatic.emergent.minDepth, '#3e8a5a'], [params.aquatic.floating.minDepth, '#2f6f8f'],
                 [params.aquatic.submerged.minDepth, '#1f4f8a']];
  for (const [d, c] of bands) {
    if (d <= 0 || d > B.depth) continue;
    const [, y] = toCanvas(cv, 0, d / B.depth);
    g.strokeStyle = c; g.setLineDash([2, 4]);
    g.beginPath(); g.moveTo(PAD.l, y); g.lineTo(W - PAD.r, y); g.stroke();
  }
  g.setLineDash([]);
}

function nearestPoint(cv, x, y) {
  const pts = profilePts();
  let best = -1, bd = 10 * 10;
  for (let i = 0; i < pts.length; i++) {
    const [px, py] = toCanvas(cv, pts[i][0], pts[i][1]);
    const d = (px - x) * (px - x) + (py - y) * (py - y);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

function bindProfileEditor(cv) {
  const pos = (e) => {
    const r = cv.getBoundingClientRect();
    return [(e.clientX - r.left) * cv.width / r.width, (e.clientY - r.top) * cv.height / r.height];
  };
  cv.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    const [x, y] = pos(e);
    let i = nearestPoint(cv, x, y);
    undo.snapshot(null);
    if (i < 0) {
      const [u, f] = fromCanvas(cv, x, y);
      const pts = profilePts();
      pts.push([u, f]);
      params.bathymetry.profile = WG.sanitizeProfile(pts);
      i = params.bathymetry.profile.findIndex(p => Math.abs(p[0] - u) < 1e-6);
    }
    dragIdx = i;
    drawProfile();
    e.preventDefault();
  });
  cv.addEventListener('dblclick', (e) => {
    const [x, y] = pos(e);
    const i = nearestPoint(cv, x, y);
    const pts = profilePts();
    if (i <= 0 || i >= pts.length - 1) return;     // the ends stay
    undo.snapshot(null);
    pts.splice(i, 1);
    dragIdx = -1;
    markDirty(); regenerate(false);
  });
  window.addEventListener('mousemove', (e) => {
    if (dragIdx < 0) return;
    const [x, y] = pos(e);
    let [u, f] = fromCanvas(cv, x, y);
    const pts = profilePts();
    if (dragIdx === 0) u = 0;
    if (dragIdx === pts.length - 1) u = 1;
    // Keep x order so the curve stays a function.
    if (dragIdx > 0) u = Math.max(u, pts[dragIdx - 1][0] + 0.01);
    if (dragIdx < pts.length - 1) u = Math.min(u, pts[dragIdx + 1][0] - 0.01);
    pts[dragIdx] = [u, f];
    drawProfile();
  });
  window.addEventListener('mouseup', () => {
    if (dragIdx < 0) return;
    dragIdx = -1;
    markDirty(); regenerate(false);
  });
}

/* ===========================================================================
 * shore plant list — a dynamic, reorderable stack
 * ======================================================================== */
function buildPlantList(body) {
  const el = H.el;
  const mats = () => H.materials() || [];
  const list = el('div', {});
  const render = () => {
    list.innerHTML = '';
    const plants = params.shore.plants;
    plants.forEach((pl, i) => {
      const matSel = el('select', {class: CLS + 'num', title: 'the stalk material'});
      const headSel = el('select', {class: CLS + 'num', title: 'an optional single cell on top (a cattail head)'});
      headSel.append(el('option', {value: ''}, '(no head)'));
      for (const m of mats()) {
        matSel.append(el('option', {value: m.id}, m.id));
        headSel.append(el('option', {value: m.id}, m.id));
      }
      matSel.value = pl.material; headSel.value = pl.head || '';
      const chance = el('input', {type: 'number', class: CLS + 'num', value: pl.chance, min: 0, max: 999, step: 1,
                                  title: '1 in N shore columns within reach grows this'});
      const reach = el('input', {type: 'number', class: CLS + 'num', value: pl.reach, min: 0, max: 20, step: 0.1,
                                 title: 'metres past the shoreline it will grow'});
      const height = el('input', {type: 'number', class: CLS + 'num', value: pl.height, min: 0.1, max: 6, step: 0.1,
                                  title: 'stalk height in metres'});
      const bind = (inp, k, num) => inp.addEventListener('change', () => {
        undo.snapshot(null);
        pl[k] = num ? +inp.value : inp.value;
        markDirty(); regenerate(false);
      });
      bind(matSel, 'material'); bind(headSel, 'head'); bind(chance, 'chance', true);
      bind(reach, 'reach', true); bind(height, 'height', true);
      const up = el('button', {title: 'Earlier rows win the roll first: move up to crowd the water'}, '▲');
      const dn = el('button', {}, '▼');
      const rm = el('button', {title: 'remove'}, '✕');
      up.disabled = i === 0; dn.disabled = i === plants.length - 1;
      up.addEventListener('click', () => { undo.snapshot(null); [plants[i - 1], plants[i]] = [plants[i], plants[i - 1]]; render(); markDirty(); regenerate(false); });
      dn.addEventListener('click', () => { undo.snapshot(null); [plants[i + 1], plants[i]] = [plants[i], plants[i + 1]]; render(); markDirty(); regenerate(false); });
      rm.addEventListener('click', () => { undo.snapshot(null); plants.splice(i, 1); render(); markDirty(); regenerate(false); });
      list.append(el('div', {class: CLS + 'plant'},
        el('span', {class: CLS + 'ord'}, String(i + 1)), matSel, headSel,
        el('span', {class: CLS + 'unit'}, '1 in'), chance,
        el('span', {class: CLS + 'unit'}, 'reach'), reach,
        el('span', {class: CLS + 'unit'}, 'h'), height, up, dn, rm));
    });
    const add = el('button', {}, '+ plant');
    add.addEventListener('click', () => {
      undo.snapshot(null);
      params.shore.plants.push({material: 'marsh_grass', head: '', chance: 6, reach: params.shore.band, height: 0.3});
      render(); markDirty(); regenerate(false);
    });
    list.append(el('div', {class: CLS + 'bar', style: 'margin-top:4px'}, add,
      el('span', {class: CLS + 'hint'}, 'Ordered by distance from the water: row 1 rolls first, so ' +
         'give the water-hugging species the top row and the widest reach to the last.')));
  };
  render();
  widgets.push(render);
  body.append(list);
}

/* ===========================================================================
 * the panel
 * ======================================================================== */
function buildPanel() {
  const el = H.el;
  const col = els.sliders;
  col.innerHTML = '';
  widgets = [];
  const C = ctx();
  const mats = () => H.materials() || [];
  const isSolid = m => m.class === 'solid' || m.class === 'powder';
  const isLiquid = m => m.class === 'liquid';

  let s = UI.section(el, CLS, 'Footprint', 'The outline. `squareness` and `shore warp` do most of ' +
                     'the work; lobes are for bays a warped disc cannot make.');
  FOOTPRINT_ROWS.forEach(r => UI.row(C, s.body, r, 'footprint'));
  UI.boolRow(C, s.body, {k: 'rotationRandom', n: 'random rotation',
                         d: 'Roll the orientation per instance. Off for an authored set piece.'}, 'footprint');
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Bathymetry', 'Depth as a function of distance from the centre. The ' +
                 'curve below the 3D view IS this section’s editor: drag points, click to add, ' +
                 'double-click to remove.');
  BATHY_ROWS.forEach(r => UI.row(C, s.body, r, 'bathymetry'));
  const pre = el('div', {class: CLS + 'bar'}, el('span', {class: CLS + 'unit'}, 'profile'));
  for (const [nm, pts] of PROFILE_PRESETS) {
    const b = el('button', {}, nm);
    b.addEventListener('click', () => {
      undo.snapshot(null);
      params.bathymetry.profile = JSON.parse(JSON.stringify(pts));
      markDirty(); regenerate(false);
    });
    pre.append(b);
  }
  s.body.append(pre);
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Fill', 'What the basin holds. A body with no fill is a dry bed — a ' +
                 'playa, a dust bowl, a drained tarn.');
  UI.matRow(C, s.body, {k: 'material', n: 'fluid', d: 'water, oil, lava, acid… or none.'},
            'fill', mats, {allowNone: true, filter: isLiquid});
  UI.matRow(C, s.body, {k: 'surfaceMaterial', n: 'surface skin',
                        d: 'An optional solid ON the surface — ice on a frozen tarn.'},
            'fill', mats, {allowNone: true, filter: isSolid});
  FILL_ROWS.forEach(r => UI.row(C, s.body, r, 'fill'));
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Berm', 'The containment. The engine forces the bank just outside the ' +
                 'shore above the waterline; this is that lift and how it fades.');
  BERM_ROWS.forEach(r => UI.row(C, s.body, r, 'berm'));
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Bed & ground', 'By NAME, resolved against materials.json.');
  UI.matRow(C, s.body, {k: 'shallow', n: 'bed (shallow)', d: ''}, 'bed', mats, {filter: isSolid});
  UI.matRow(C, s.body, {k: 'deep', n: 'bed (deep)', d: ''}, 'bed', mats, {filter: isSolid});
  UI.matRow(C, s.body, {k: 'substrate', n: 'under the bed', d: ''}, 'bed', mats, {filter: isSolid});
  BED_ROWS.forEach(r => UI.row(C, s.body, r, 'bed'));
  UI.matRow(C, s.body, {k: 'skin', n: 'ground skin', d: 'The natural ground the body is cut into. ' +
            'In the world the BIOME decides this; here it is what the preview stands in.'}, 'ground', mats, {filter: isSolid});
  UI.matRow(C, s.body, {k: 'soil', n: 'soil', d: ''}, 'ground', mats, {filter: isSolid});
  UI.matRow(C, s.body, {k: 'rock', n: 'rock', d: ''}, 'ground', mats, {filter: isSolid});
  GROUND_ROWS.forEach(r => UI.row(C, s.body, r, 'ground'));
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Shore', 'The wet fringe on land, by distance past the waterline.');
  SHORE_ROWS.forEach(r => UI.row(C, s.body, r, 'shore'));
  UI.matRow(C, s.body, {k: 'mudMaterial', n: 'mud skin', d: ''}, 'shore', mats, {allowNone: true, filter: isSolid});
  UI.matRow(C, s.body, {k: 'mossMaterial', n: 'moss skin', d: ''}, 'shore', mats, {allowNone: true, filter: isSolid});
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin-top:6px'}, 'Shore plants'));
  buildPlantList(s.body);
  col.append(s.wrap);

  s = UI.section(el, CLS, 'In the water', 'Vegetation by DEPTH. Band widths are not knobs: they ' +
                 'fall out of the bathymetry. A steep kettle gets a one-cell reed fringe; a marsh ' +
                 'is all fringe.');
  s.body.append(el('div', {class: CLS + 'hint'}, 'Emergent — rooted in the shallows, standing above the surface'));
  UI.matRow(C, s.body, {k: 'material', n: 'emergent plant', d: ''}, 'aquatic.emergent', mats, {allowNone: true, filter: isSolid});
  EMERGENT_ROWS.forEach(r => UI.row(C, s.body, r, 'aquatic.emergent'));
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin-top:8px'}, 'Floating — pads on the surface'));
  UI.matRow(C, s.body, {k: 'material', n: 'floating plant', d: ''}, 'aquatic.floating', mats, {allowNone: true, filter: isSolid});
  UI.matRow(C, s.body, {k: 'flower', n: 'its flower', d: ''}, 'aquatic.floating', mats, {allowNone: true, filter: isSolid});
  FLOATING_ROWS.forEach(r => UI.row(C, s.body, r, 'aquatic.floating'));
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin-top:8px'}, 'Submerged — held under the surface'));
  UI.matRow(C, s.body, {k: 'material', n: 'submerged plant', d: ''}, 'aquatic.submerged', mats, {allowNone: true, filter: isSolid});
  SUBMERGED_ROWS.forEach(r => UI.row(C, s.body, r, 'aquatic.submerged'));
  col.append(s.wrap);

  s = UI.section(el, CLS, 'Placement defaults', 'What a biome row inherits when it lists this ' +
                 'preset. The biome page is where these are OVERRIDDEN per biome; these are the ' +
                 'values it starts from.', {closed: true});
  PLACEMENT_ROWS.forEach(r => UI.row(C, s.body, r, 'placement'));
  col.append(s.wrap);

  // The engine's live pond knobs, so the one tarn worldgen grows TODAY can be
  // tuned from the same page that authors the presets it will grow tomorrow —
  // built by the Worldgen tab's own tuneRow, so a knob has one definition.
  const T = H.tuning && H.tuning();
  const wgTab = T && T.schema && T.tuneRow ? T.schema.find(t => t.id === 'worldgen') : null;
  if (wgTab && T.tune && T.tune.worldgen) {
    s = UI.section(el, CLS, 'Engine today — worldgen.pond*/shore*',
                   'What the ENGINE grows right now: one parabolic tarn per pond tile, shaped by these ' +
                   'tuning.json rows (live; a new world shows them). The presets above are what it will ' +
                   'grow once worldgen reads the water table (PLAN_biomes.md §5, step 3).', {closed: true});
    const box = el('div', {class: 'trows'});
    for (const pr of wgTab.params) {
      if (pr.sec || !/^(pond|shore|reed|kelp|lily)/.test(pr.k)) continue;
      box.append(T.tuneRow(wgTab, pr, T.paramGroup(wgTab, pr), T.touchTune));
    }
    s.body.append(box);
    col.append(s.wrap);
  }

  s = UI.section(el, CLS, 'Identity', '', {closed: true});
  const disp = el('input', {type: 'text', class: CLS + 'num', value: params.displayName});
  disp.addEventListener('change', () => { undo.snapshot(null); params.displayName = disp.value; markDirty(); });
  widgets.push(() => { disp.value = params.displayName; });
  s.body.append(el('div', {class: CLS + 'row'}, el('label', {}, 'display name'), disp));
  const kind = el('select', {class: CLS + 'num'});
  ['lake', 'pond', 'marsh', 'pool', 'dry'].forEach(k => kind.append(el('option', {value: k}, k)));
  kind.value = params.kind;
  kind.addEventListener('change', () => { undo.snapshot(null); params.kind = kind.value; markDirty(); });
  widgets.push(() => { kind.value = params.kind; });
  s.body.append(el('div', {class: CLS + 'row', title: 'A label the biome page groups by. No engine meaning.'},
                   el('label', {}, 'kind'), kind));
  col.append(s.wrap);
}

function refreshWidgets() { for (const w of widgets) { try { w(); } catch (e) {} } }

function markDirty() {
  dirty = true;
  if (H.onDirty) H.onDirty(true);
  if (els.save) els.save.disabled = false;
}

/* ===========================================================================
 * files
 * ======================================================================== */
export async function listPresets() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const seen = new Set();
  return (j.files || [])
      .filter(f => f.dir === 'water' && f.name.endsWith('.json') && f.name[0] !== '_')
      .map(f => f.name.slice(0, -5))
      .filter(n => (seen.has(n) ? false : (seen.add(n), true)))
      .sort();
}

async function loadPreset(name) {
  const r = await fetch('/api/model?path=water/' + encodeURIComponent(name + '.json'), {cache: 'no-store'});
  if (!r.ok) throw new Error('HTTP ' + r.status);
  setParams(WG.normalizeParams(await r.json(), vpm()), name);
}

function setParams(p, name) {
  params = p;
  presetName = name;
  undo.clear();
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  if (els.save) els.save.disabled = true;
  framed = false;
  buildPanel();
  regenerate(true);
}

async function savePreset(asName) {
  const name = asName || presetName;
  if (!name) return;
  const out = JSON.parse(JSON.stringify(params));
  delete out.vpm;
  out.name = name;
  const body = JSON.stringify(out, null, 2) + '\n';
  const r = await fetch('/api/model?path=water/' + encodeURIComponent(name + '.json'), {method: 'POST', body});
  const j = await r.json();
  if (!j.ok) { H.toast('save failed: ' + (j.error || '?'), true); return; }
  presetName = name;
  params.name = name;
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  els.save.disabled = true;
  H.toast('saved water/' + name + '.json');
  await refreshList(name);
  if (H.onLibraryChanged) H.onLibraryChanged('water');
}

async function exportVox() {
  if (!lastResult) return;
  const res = lastResult;
  if (res.dim.x > VOX.MAX_DIM || res.dim.y > VOX.MAX_DIM || res.dim.z > VOX.MAX_DIM) {
    H.toast('too big for .vox: ' + res.dim.x + '×' + res.dim.y + '×' + res.dim.z + ' exceeds ' +
            VOX.MAX_DIM + ' per axis. Shrink the radius or the shore band.', true);
    return;
  }
  const mats = H.materials() || [];
  const idOf = new Map();
  mats.forEach((m, i) => idOf.set(m.id, i + 1));
  const g = VOX.makeGrid({x: res.dim.x, y: res.dim.y, z: res.dim.z});
  for (let z = 0; z < res.dim.z; z++)
    for (let y = 0; y < res.dim.y; y++)
      for (let x = 0; x < res.dim.x; x++) {
        const w = res.cells[(z * res.dim.y + y) * res.dim.x + x];
        if (!w) continue;
        const id = idOf.get(res.names[(w & 0xFFF) - 1]) || 0;
        if (id) VOX.gridSet(g, x, y, z, id);
      }
  const name = presetName + '_' + seed;
  try {
    const buf = VOX.writeVox(VOX.gridToModel(g, name));
    const back = VOX.readVox(buf);
    if (!back || !back.prefab) throw new Error('round trip produced nothing');
    const r = await fetch('/api/model?path=prefabs/' + encodeURIComponent(name + '.vox'), {method: 'POST', body: buf});
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || '?');
    H.toast('wrote prefabs/' + name + '.vox — press R in game to reload prefabs');
  } catch (e) {
    H.toast('.vox export failed: ' + (e && e.message || e), true);
  }
}

async function refreshList(select) {
  const names = await listPresets();
  els.preset.innerHTML = '';
  names.forEach(n => els.preset.append(H.el('option', {value: n}, n)));
  if (select && names.includes(select)) els.preset.value = select;
  return names;
}

/* ===========================================================================
 * mount
 * ======================================================================== */
const CSS = UI.pageCss(PAGE, CLS, `
#${PAGE} .${CLS}bottom{display:flex;gap:8px;height:190px;flex:0 0 190px}
#${PAGE} canvas.${CLS}plan{width:190px;height:190px;flex:0 0 190px;background:#0e1116;border:1px solid #2a3040;border-radius:6px}
#${PAGE} canvas.${CLS}profile{flex:1;min-width:0;height:190px;background:#0e1116;border:1px solid #2a3040;border-radius:6px;cursor:crosshair}
#${PAGE} .${CLS}plant{display:grid;grid-template-columns:14px 1fr 1fr auto 44px auto 44px auto 44px 22px 22px 22px;gap:3px;align-items:center;margin:2px 0;font-size:11px}
#${PAGE} .${CLS}plant button{padding:0 3px;font-size:10px;line-height:18px}
#${PAGE} .${CLS}ord{color:#6f7f97;font:10px monospace}
`);

export function attach(hooks) {
  H = hooks;
  const el = H.el;
  const root = H.section;
  if (!root) return;
  root.id = PAGE;

  const style = document.createElement('style');
  style.textContent = CSS;
  document.head.append(style);

  undo = UI.makeUndo({
    get: () => params,
    set: (o) => { params = WG.normalizeParams(o, vpm()); refreshWidgets(); markDirty(); regenerate(true); },
    toast: H.toast
  });

  els.preset = el('select', {class: CLS + 'num', style: 'width:150px'});
  els.save = el('button', {disabled: true}, 'Save');
  els.saveAs = el('button', {}, 'Save as…');
  els.fromLib = el('select', {class: CLS + 'num', style: 'width:130px', title: 'Start a new preset from a library shape'});
  els.fromLib.append(el('option', {value: ''}, 'new from…'));
  WG.PRESET_ORDER.forEach(n => els.fromLib.append(el('option', {value: n}, n)));
  els.vox = el('button', {}, 'Export .vox');
  els.seed = el('input', {type: 'number', class: CLS + 'num', value: '0', style: 'width:56px', min: '0', max: '9999'});
  els.quad = el('input', {type: 'checkbox'});
  els.undo = el('button', {title: 'Undo (Ctrl+Z)'}, '↶');
  els.redo = el('button', {title: 'Redo (Ctrl+Shift+Z)'}, '↷');
  els.stats = el('div', {class: CLS + 'stats'}, 'loading…');
  els.sliders = el('div', {class: CLS + 'sliders'});
  const cv = el('canvas', {class: CLS + 'view'});
  els.plan = el('canvas', {class: CLS + 'plan', width: '190', height: '190', title: 'Plan: depth and vegetation zones from above'});
  els.profile = el('canvas', {class: CLS + 'profile', width: '600', height: '190',
                              title: 'Section through the centre. Drag a point; click empty space to add one; double-click a point to remove it.'});

  els.preset.addEventListener('change', () => {
    if (dirty && !confirm('Discard unsaved changes to ' + presetName + '?')) { els.preset.value = presetName; return; }
    loadPreset(els.preset.value).catch(e => H.toast(String(e), true));
  });
  els.fromLib.addEventListener('change', () => {
    const n = els.fromLib.value;
    els.fromLib.value = '';
    if (!n) return;
    if (dirty && !confirm('Discard unsaved changes to ' + presetName + '?')) return;
    setParams(WG.normalizeParams(WG.PRESETS[n], vpm()), '');
    markDirty();
    H.toast('new preset from the library shape "' + n + '" — Save as… to keep it');
  });
  els.save.addEventListener('click', () => savePreset());
  els.saveAs.addEventListener('click', () => {
    const n = prompt('Preset name (a file under assets/water/):', presetName || params.name);
    if (n) savePreset(n.trim().replace(/[^a-zA-Z0-9_-]/g, '_').toLowerCase());
  });
  els.vox.addEventListener('click', exportVox);
  els.seed.addEventListener('change', () => { seed = Math.max(0, parseInt(els.seed.value, 10) || 0); regenerate(true); });
  els.quad.addEventListener('change', () => { quad = els.quad.checked; framed = false; regenerate(true); });
  els.undo.addEventListener('click', () => undo.undo());
  els.redo.addEventListener('click', () => undo.redo());

  document.addEventListener('keydown', (e) => {
    if (!root.classList.contains('active') || !H.isVisible()) return;
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = (e.key || '').toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); undo.undo(); }
    else if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); e.stopImmediatePropagation(); undo.redo(); }
  }, true);

  root.append(
    el('div', {class: CLS + 'left'},
      el('div', {class: CLS + 'bar'}, el('span', {class: 'hint'}, 'preset'), els.preset, els.save, els.saveAs, els.fromLib),
      el('div', {class: CLS + 'bar'}, els.vox, els.undo, els.redo,
         el('span', {class: 'hint'}, 'seed'), els.seed,
         el('label', {class: 'hint', style: 'display:flex;align-items:center;gap:4px',
                      title: 'Four seeds at once — this one and the next three. Save and Export act on the first.'},
            els.quad, 'quad')),
      els.sliders),
    el('div', {class: CLS + 'right'}, cv,
      el('div', {class: CLS + 'bottom'}, els.plan, els.profile),
      els.stats));

  wv = UI.makeView(cv, H.materials() || [], () => root.classList.contains('active') && H.isVisible(),
                   (e) => { els.stats.textContent = 'the 3D preview needs WebGL2: ' + (e && e.message || e); });
  bindProfileEditor(els.profile);
}

export function activate() {
  if (!params) {
    refreshList().then(names => {
      const pick = names.includes('tarn') ? 'tarn' : names[0];
      if (!pick) {
        // No files yet: start from the library so the page is never empty.
        setParams(WG.normalizeParams(WG.PRESETS.tarn, vpm()), 'tarn');
        markDirty();
        els.stats.textContent = 'no presets in assets/water/ — run `node scripts/seed_environment.mjs --seed`, or Save this one';
        return;
      }
      return loadPreset(pick);
    }).catch(e => { els.stats.textContent = 'could not list presets: ' + (e && e.message || e); });
  }
}

export function saveFromHost() { if (presetName) savePreset(); }
export function isDirty() { return dirty; }
export function currentName() { return presetName; }
/** Open a named preset (the biome page's "edit" link). */
export function open(name) { return loadPreset(name); }

// test seams, as trees.js
export function _view() { return wv; }
export function _undoDepth() { return undo ? undo.depth() : 0; }
export function _params() { return params; }
export function _last() { return lastResult; }
