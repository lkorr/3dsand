/* trees.js — the Trees tab: an algorithmic tree editor with a live voxel preview.
 *
 * WHAT IT IS. A slider column driven by TREE_SCHEMA, a species dropdown backed
 * by assets/trees/<name>.json, and a 3D view of exactly the voxels the engine
 * will place. Drag a slider, see the tree.
 *
 * WHY IT IS WYSIWYG AND NOT AN APPROXIMATION. assets/editor/treegen.js is the
 * ONLY voxelizer in the project: this tab calls it, `scripts/bake_trees.mjs`
 * calls it, and the engine reads what it baked. There is no second
 * implementation to drift, which is the reason the engine gave up evaluating
 * tree shapes per cell in the first place (see worldgen.wgsl's tree section).
 *
 * WHY IT REUSES WorldView. assets/worldview.js already draws a voxel grid with
 * per-fragment AO, greedy meshing, an orbit camera, slice, isolate and the
 * engine's own material palette. Growing a second WebGL renderer here would be
 * a second thing to keep looking like the game. The one addition it needed is
 * `setLocalRegion` — a region that comes from an array instead of the server —
 * plus `opts.streaming === false` so the streamer does not try to fetch terrain
 * that has no server behind it.
 *
 * THE TAB WORKS WITH NO BUILT EXE. Nothing here calls the engine: the palette
 * comes from the materials.json the page already has loaded, and generation is
 * pure JS. Baking writes files through the ordinary /api/model route.
 */

import * as TG from './treegen.js';
import * as VOX from './vox.js';

let H = null;              // host hooks from tuner.html
let wv = null;             // WorldView
let params = null;         // the species being edited
let speciesName = '';
let seed = 0;
let quad = false;          // show four seeds at once, one per quadrant
let dirty = false;
let genTimer = 0;
let lastResult = null;     // the SUBJECT tree — what Save/Bake/Export act on
let placed = [];           // {res, tx, tz, origin} per tree currently in view
let els = {};

/* ---------------------------------------------------------------------------
 * UNDO
 *
 * A parameter editor without undo is a parameter editor you are afraid of, so
 * this is a plain snapshot stack over the whole `params` object. Snapshots and
 * not deltas because a species file is ~2 KB: a hundred of them is 200 KB, and
 * a delta log would have to understand the shape of every row it could touch.
 *
 * THE COALESCE WINDOW IS THE WHOLE TRICK. A range input fires `input` on every
 * pixel of a drag, so a naive push-per-change turns one slider drag into fifty
 * undo steps and Ctrl+Z appears not to work at all — you press it eight times
 * and the tree has not visibly moved. Consecutive edits to the SAME control
 * inside `COALESCE_MS` collapse into the single snapshot taken before the drag
 * began; touching a different control, or pausing, starts a new step.
 * ------------------------------------------------------------------------- */
const UNDO_MAX = 100;
const COALESCE_MS = 500;
let undoStack = [];
let redoStack = [];
let coalesceKey = null;
let coalesceUntil = 0;
/** Re-read every widget from `params`. Filled by buildPanel; an undo has to
 *  move the CONTROLS as well as the tree, or the two disagree silently. */
let widgets = [];

function snapshot(key) {
  const now = Date.now();
  if (key !== null && key === coalesceKey && now < coalesceUntil) {
    coalesceUntil = now + COALESCE_MS;
    return;
  }
  coalesceKey = key;
  coalesceUntil = now + COALESCE_MS;
  undoStack.push(JSON.stringify(params));
  if (undoStack.length > UNDO_MAX) undoStack.shift();
  redoStack.length = 0;
}

function refreshWidgets() {
  for (const w of widgets) { try { w(); } catch (e) {} }
}

function applySnapshot(json) {
  params = TG.normalizeParams(JSON.parse(json));
  coalesceKey = null;
  refreshWidgets();
  markDirty();
  regenerate(true);
}

function undo() {
  if (!undoStack.length) { H.toast('nothing to undo'); return; }
  redoStack.push(JSON.stringify(params));
  applySnapshot(undoStack.pop());
  H.toast('undo (' + undoStack.length + ' left)');
}

function redo() {
  if (!redoStack.length) { H.toast('nothing to redo'); return; }
  undoStack.push(JSON.stringify(params));
  applySnapshot(redoStack.pop());
  H.toast('redo (' + redoStack.length + ' left)');
}

const $ = (sel, root) => (root || document).querySelector(sel);

/* ===========================================================================
 * THE SCHEMA
 *
 * Same row vocabulary as assets/tuner_schema.js — {k, n, d, min, max, step} —
 * because the Tuning tab has already taught everyone what those mean. Rows
 * carry a PATH into the params object, so per-level arrays are addressed as
 * `levelsData.1.downAngle` and nothing here needs to know the shape.
 *
 * Every slider that exists is in this table, and nothing that is not in this
 * table is editable. That is deliberate: "what can I change about a tree" then
 * has exactly one answer, and adding a knob to treegen.js means adding a row.
 * ======================================================================== */

const GLOBAL_ROWS = [
  {k: 'shape', n: 'crown shape', min: 0, max: 7, step: 1, int: true,
   d: 'The Weber-Penn crown envelope, and the single most identity-defining ' +
      'knob here: it decides how long a branch may be as a function of where ' +
      'it leaves the trunk. 0 conical (spruce, fir) - 1 spherical - ' +
      '2 hemispherical (oak, most broadleaves) - 3 cylindrical - 4 tapered ' +
      'cylindrical (redwood: a column, not a cone) - 5 flame (birch, aspen) - ' +
      '6 inverse conical (willow) - 7 tend flame.'},
  {k: 'scale', n: 'height (m)', min: 0.4, max: 30, step: 0.1,
   d: 'Trunk length in metres. Voxels are 10 cm, so this reads true next to a ' +
      '1.7 m player.'},
  {k: 'scaleV', n: 'height variance (m)', min: 0, max: 6, step: 0.1,
   d: 'Per-tree jitter on the height. What makes two oaks in a stand not the ' +
      'same oak.'},
  {k: 'levels', n: 'branch levels', min: 1, max: 4, step: 1, int: true,
   retunes: true,
   d: 'Recursion depth. 0 is the trunk; 3 is trunk + boughs + branches + ' +
      'twigs. The deepest branch that exists is one BELOW this, which is what ' +
      '`lowest leafy level` is bounded against.'},
  {k: 'baseSize', n: 'bare trunk', min: 0, max: 0.9, step: 0.01,
   d: 'Fraction of the trunk carrying no branches at all. A forest tree ' +
      'self-prunes its lower limbs; a field oak does not.'},
  {k: 'ratio', n: 'trunk thickness', min: 0.008, max: 0.09, step: 0.001,
   d: 'Trunk radius as a fraction of its length. 0.02 is a slender birch, ' +
      '0.05 a redwood.'},
  {k: 'ratioPower', n: 'taper by level', min: 0.5, max: 2.5, step: 0.05,
   d: 'How fast radius falls at each branch level. Above 1 the children are ' +
      'much thinner than their parent.'},
  {k: 'flare', n: 'root flare', min: 0, max: 3, step: 0.05,
   d: 'Buttress at the base of the bole.'},
  {k: 'attractionUp', n: 'tropism', min: -1, max: 1, step: 0.02,
   d: 'Vertical pull applied along every stem. Positive lifts branch tips ' +
      'toward the sky (most trees); NEGATIVE droops them, which is the whole ' +
      'of a weeping willow.'},
  {k: 'whorlCount', n: 'whorls', min: 0, max: 8, step: 1, int: true,
   d: 'Branches emitted in rings of N rather than a continuous spiral. ' +
      'Vanilla Weber-Penn cannot make a convincing conifer without it; 0 is ' +
      'pure phyllotaxis.'},
  {k: 'lobes', n: 'trunk lobes', min: 0, max: 9, step: 1, int: true,
   d: 'Fluting on the bole cross-section, so a big trunk is not a cylinder.'},
  {k: 'lobeDepth', n: 'lobe depth', min: 0, max: 0.4, step: 0.01, d: ''},
  {k: 'barkNoise', n: 'bark relief', min: 0, max: 2, step: 0.05,
   d: 'Voxels of noise subtracted from the bark surface. Scaled down on thin ' +
      'stems automatically, or it would eat a twig alive.'},
  {k: 'variants', n: 'baked variants', min: 1, max: 8, step: 1, int: true,
   d: 'How many pre-voxelized trees this species ships. Variety in game is ' +
      'variants x 4 rotations x mirror, so 3 is already 24 appearances - and ' +
      'each one costs its own share of the atlas.'}
];

const LEVEL_ROWS = [
  {k: 'branches', n: 'children', min: 0, max: 60, step: 1, int: true,
   d: 'How many stems of THIS level each parent carries.'},
  {k: 'length', n: 'length x parent', min: 0.05, max: 1.2, step: 0.01, d: ''},
  {k: 'lengthV', n: 'length variance', min: 0, max: 0.6, step: 0.01, d: ''},
  {k: 'downAngle', n: 'down angle', min: 0, max: 130, step: 1, d:
   'Pitch away from the parent axis, in degrees. 0 continues the parent, 90 ' +
   'is horizontal.'},
  {k: 'downAngleV', n: 'down angle var', min: -80, max: 80, step: 1, d:
   'Positive is random jitter. NEGATIVE is the good trick: the pitch becomes ' +
   'a function of where on the parent the child sits, so lower branches lie ' +
   'flat and upper ones sweep up. Most of what makes a conifer read as one.'},
  {k: 'rotate', n: 'rotate', min: -180, max: 180, step: 0.5, d:
   'Phyllotaxis: the roll between successive children. 137.5 is the golden ' +
   'angle. NEGATIVE alternates sides instead of spiralling - the flat, ' +
   'fern-like spray a lot of conifers have.'},
  {k: 'rotateV', n: 'rotate variance', min: 0, max: 60, step: 1, d: ''},
  {k: 'curve', n: 'curve', min: -160, max: 160, step: 1, d:
   'Total bend over the whole stem, in degrees.'},
  {k: 'curveBack', n: 'curve back', min: -160, max: 160, step: 1, d:
   'Second-half bend the other way, which is what makes an S-curve out of an arc.'},
  {k: 'curveV', n: 'gnarl', min: 0, max: 150, step: 1, d:
   'Random per-segment wander. High values are what an old oak looks like.'},
  {k: 'curveRes', n: 'segments', min: 1, max: 16, step: 1, int: true, d:
   'Segments per stem. A RESOLUTION knob, not a shape knob: raising it gives ' +
   'a smoother version of the same stem.'},
  {k: 'taper', n: 'taper', min: 0, max: 1, step: 0.02, d:
   '0 is a cylinder, 1 tapers to a point.'},
  {k: 'segSplits', n: 'forks/segment', min: 0, max: 2, step: 0.05, d:
   'Dichotomous splits. Fractional: 0.35 means about a third of the segments ' +
   'fork. An oak scaffold is mostly this.'},
  {k: 'splitAngle', n: 'fork angle', min: 0, max: 70, step: 1, d: ''},
  {k: 'splitAngleV', n: 'fork angle var', min: 0, max: 40, step: 1, d: ''}
];

const FOLIAGE_ROWS = [
  // maxOf, because a fixed max here is a slider most of whose travel does
  // nothing. `levels` is the RECURSION DEPTH and the deepest stem that exists
  // is levels-1, so on a 3-level tree everything from 3 up is the same bare
  // trunk — six of the ten stops on the old 0..9 range were "no leaves", which
  // reads exactly like a broken control.
  {k: 'startLevel', n: 'lowest leafy level', min: 0, step: 1, int: true,
   max: 4, maxOf: (p) => Math.max(0, p.levels | 0),
   d: 'The lowest branch level that grows leaf clumps. The deepest branch a ' +
      'tree HAS is one below `branch levels`, so that value keeps foliage at ' +
      'the extremities — which is what makes a tree read as branch structure ' +
      'rather than a green ball. This slider stops one past it, and that last ' +
      'stop is a bare tree with no leaves at all.'},
  {k: 'clumpShape', n: 'clump shape', min: 0, max: 3, step: 1, int: true,
   d: 'What ONE lobe of foliage is. A crown built from a single primitive can ' +
      'only ever be a pile of that primitive, which is what makes a tree read ' +
      'as a bunch of grapes. 0 blob — a round lobe, broadleaf mass. ' +
      '1 plate — squashed along its axis into a disc: with an upright axis ' +
      'this is the tiered spray of a cedar or an old pine, and the gaps ' +
      'between tiers are the read. 2 spray — stretched along the axis and ' +
      'thin across, a leafy SHOOT rather than a ball; the way to get a lot of ' +
      'leaves with no blob anywhere. 3 cone — fat at the base tapering to a ' +
      'point, a spruce sprig. All four weld to each other, so mixing species ' +
      'in a stand costs nothing.'},
  {k: 'clumpAxis', n: 'clump follows twig', min: 0, max: 1, step: 0.05,
   d: 'What the clump’s axis IS. 0 points every lobe straight up (a canopy ' +
      'tier); 1 lays it along the twig it grows on (a shoot off a branch). ' +
      'Only shapes 1-3 have a visible axis — a blob is a blob either way.'},
  {k: 'hollow', n: 'hollow core', min: 0, max: 0.95, step: 0.05,
   d: 'Eat the CORE out of every lobe. A real crown is a surface: the inside ' +
      'is bare twig and shade, and no ray from outside ever reaches it. 0.7 ' +
      'leaves a shell of leaves a few voxels deep, which is far more leaf ' +
      'EDGE per voxel spent — and it deletes voxels the player cannot see.'},
  {k: 'clumpsPerStem', n: 'clumps per stem', min: 0, max: 6, step: 0.1,
   d: 'FRACTIONAL on purpose: the tip count is set by the branching, which is ' +
      'chosen for the silhouette, so the clump count has to be tunable ' +
      'independently. At 0.4 two stems in five carry a lobe - which is how a ' +
      'crown gets gaps without eroding every lobe into lace.'},
  {k: 'tipBias', n: 'tip bias', min: 0, max: 1, step: 0.02,
   d: 'Where along the stem clumps start. High keeps them at the tips.'},
  {k: 'radius', n: 'clump radius (m)', min: 0.08, max: 2, step: 0.02, d: ''},
  {k: 'radiusV', n: 'clump variance (m)', min: 0, max: 1, step: 0.02, d: ''},
  {k: 'elongation', n: 'clump elongation', min: 0.15, max: 3, step: 0.05,
   d: 'Above 1 flattens the clump into a needle plate (spruce, cedar); below ' +
      '1 makes it a hanging teardrop.'},
  {k: 'droop', n: 'clump droop', min: 0, max: 1.5, step: 0.02,
   d: 'How far the clump hangs below its stem. What makes a eucalypt.'},
  {k: 'density', n: 'density', min: 0.1, max: 1, step: 0.01,
   d: '1 is a solid lobe, 0.5 eats half the rim away. The GAPS are the ' +
      'identity of an old pine or a eucalypt, so this earns its slider.'},
  {k: 'noiseScale', n: 'erosion grain (vox)', min: 0.8, max: 6, step: 0.1,
   d: 'Feature size of the erosion. Per-VOXEL erosion (near 1) reads as green ' +
      'dust at any distance and multiplies the baked file size; erode in blobs.'},
  {k: 'sminK', n: 'clump weld', min: 0, max: 8, step: 0.1,
   d: 'Smooth-min radius between neighbouring clumps. 0 leaves them as ' +
      'separate beads; too much melts them back into one ball.'},
  {k: 'canopyShadeMix', n: 'shade: lobe vs canopy', min: 0, max: 1, step: 0.02,
   d: '0 shades every lobe by its own sphere normal (each reads as a separate ' +
      'ball); 1 shades by the whole canopy (the lobes vanish into one mass). ' +
      'The interesting range is 0.35-0.7, and this is most of the look.'},
  {k: 'depthShade', n: 'interior darkening', min: 0, max: 1, step: 0.02,
   d: 'How much deep-inside-a-clump goes to the dark tier of the leaf ramp.'}
];

const PLACEMENT_ROWS = [
  {k: 'biomes.forest', n: 'weight: forest', min: 0, max: 100, step: 1, int: true,
   d: 'Relative weight against every other species in this biome. Zero means ' +
      '"never here". The engine normalises across whatever is loaded, so ' +
      'adding a species dilutes the others rather than needing every table ' +
      'rewritten.'},
  {k: 'biomes.meadow', n: 'weight: meadow', min: 0, max: 100, step: 1, int: true, d: ''},
  {k: 'biomes.pine', n: 'weight: pine', min: 0, max: 100, step: 1, int: true, d: ''},
  {k: 'biomes.desert', n: 'weight: desert', min: 0, max: 100, step: 1, int: true, d: ''},
  {k: 'sparsity', n: 'rarity divisor', min: 1, max: 12, step: 1, int: true,
   d: 'Divides the weights above. 4 makes this species a quarter as common ' +
      'everywhere without retyping four numbers.'},
  {k: 'minY', n: 'lowest ground Y', min: -1, max: 300, step: 1, int: true,
   d: 'World Y band this species tolerates. -1 for no bound.'},
  {k: 'maxY', n: 'highest ground Y', min: -1, max: 300, step: 1, int: true,
   d: 'A per-species TREELINE: a spruce climbs higher than an oak. -1 for no ' +
      'bound.'},
  {k: 'maxSlope', n: 'max slope (Q8)', min: 0, max: 1024, step: 8, int: true,
   d: 'Steepest ground it will root on, in the engine’s Q8 slope units ' +
      '(256 == 1 voxel per voxel == the angle of repose). Read off the COARSE ' +
      'landform octaves, so it means "on the mountainside" and not "on this ' +
      'bump". 1024 for no bound.'},
  {k: 'shade', n: 'canopy shade', min: 0, max: 255, step: 1, int: true,
   d: 'How dark this species makes the forest floor, which is what the ' +
      'undergrowth layer reads to choose fern and moss over grass and ' +
      'flowers. 0 means "shades nothing" - the right answer for a bush, and ' +
      'what keeps a meadow full of shrubs from reading as closed forest.'},
  {k: 'autumnChance', n: 'autumn 1-in-N', min: 0, max: 40, step: 1, int: true,
   d: 'How often a tree of this species wears the autumn ramp instead of the ' +
      'green one. Per TREE, never per voxel - a stand turns together. 0 means ' +
      'this species never turns (no conifer should).',
   root: true}
];

/* ===========================================================================
 * params access by dotted path
 * ======================================================================== */
function getPath(obj, path) {
  return path.split('.').reduce((o, k) => (o == null ? o : o[k]), obj);
}
function setPath(obj, path, v) {
  const ks = path.split('.');
  const last = ks.pop();
  const t = ks.reduce((o, k) => o[k], obj);
  t[last] = v;
}

/* ===========================================================================
 * generation, debounced
 * ======================================================================== */
/* Local palette index -> engine material id, so the viewer colours the tree
 * with the SAME table the game does. Resolved by name, exactly as the C++
 * loader does at engine load, so an unknown material shows up here first. */
function toViewerCells(res, missing) {
  const mats = (H && H.materials && H.materials()) || [];
  const idOf = new Map();
  mats.forEach((m, i) => idOf.set(m.id, i + 1));
  const remap = new Uint16Array(res.names.length + 1);
  res.names.forEach((n, i) => {
    const id = idOf.get(n);
    if (id === undefined && missing.indexOf(n) < 0) missing.push(n);
    remap[i + 1] = id || 0;
  });
  const cells = new Uint16Array(res.cells.length);
  for (let i = 0; i < cells.length; i++) {
    const w = res.cells[i];
    cells[i] = w ? ((remap[w & 0xFFF] & 0xFFF) | (w & 0xF000)) : 0;
  }
  return cells;
}

/* QUAD VIEW: four seeds at once, in one scene.
 *
 * WHY IT IS FOUR REGIONS AND NOT ONE COMPOSITED GRID. The obvious build is a
 * 2x2 array with the four trees stamped into it, and it is the wrong one: a
 * quad of great oaks is (2*231)^2 * 150 = 32M cells of which the trees occupy
 * about an eighth, and both the Uint16Array and its R16UI texture pay for the
 * empty seven. WorldView already draws many regions at many origins — that is
 * what streaming IS — so handing it four grids costs four trees.
 *
 * WHY IT IS seed..seed+3 AND NOT A FRESH RANDOM ROLL. The whole point of the
 * seed box is that a tree you like can be found again; a set you cannot
 * reproduce is a screenshot, not a tool. Stepping the seed box by one rolls the
 * whole quad, which is the re-roll button without being a second control. */
const QUAD_GAP = 4;   // voxels of clear air between two trees' boxes

function layoutQuad(results) {
  let span = 1, high = 1;
  for (const r of results) {
    span = Math.max(span, r.dim.x, r.dim.z);
    high = Math.max(high, r.dim.y);
  }
  const pitch = span + QUAD_GAP;
  // Aligned by ANCHOR, not by box corner: the trunks land on an exact square,
  // so the four are comparable even when one variant grew a wider crown.
  return results.map((r, i) => ({
    res: r,
    tx: (i & 1) * pitch,
    tz: (i >> 1) * pitch,
    origin: [(i & 1) * pitch - r.anchor.x, 0, (i >> 1) * pitch - r.anchor.z]
  }));
}

function regenerate(now) {
  clearTimeout(genTimer);
  const run = () => {
    const t0 = performance.now();
    const seeds = quad ? [seed, seed + 1, seed + 2, seed + 3] : [seed];
    let results;
    try {
      results = seeds.map(s => TG.generateTree(params, s));
    } catch (e) {
      els.stats.textContent = 'generate failed: ' + (e && e.message || e);
      console.error(e);
      return;
    }
    // The FIRST tree stays the subject: Save, Bake and Export .vox all read
    // `lastResult`, and none of them should change meaning because a preview
    // toggle is on.
    lastResult = results[0];
    placed = layoutQuad(results);

    const missing = [];
    if (wv) {
      wv.setLocalRegions(placed.map(p => ({
        cells: toViewerCells(p.res, missing),
        nx: p.res.dim.x, ny: p.res.dim.y, nz: p.res.dim.z, origin: p.origin
      })));
      let span = 1, high = 1, wide = 0;
      for (const p of placed) {
        span = Math.max(span, p.res.dim.x, p.res.dim.z);
        high = Math.max(high, p.res.dim.y);
        wide = Math.max(wide, p.tx, p.tz);
      }
      wv.cam.mode = 'orbit';
      wv.cam.target = [wide / 2, high * 0.45, wide / 2];
      if (!wv._treeFramed) {
        wv.cam.dist = Math.max(wide + span, high) * 1.5;
        wv.cam.yaw = 0.7; wv.cam.pitch = -0.25;
        wv._treeFramed = true;
      }
      drawSkeleton();
    } else {
      results.forEach(r => toViewerCells(r, missing));
    }

    const m = lastResult.meta;
    let wood = 0, leaf = 0;
    for (const p of placed) { wood += p.res.meta.woodCount; leaf += p.res.meta.leafCount; }
    const truncated = placed.some(p => p.res.meta.truncated);
    const clipped = placed.some(p => p.res.meta.clipped);
    // A CANOPY THAT CAME OUT EMPTY NAMES ITSELF. "the leaves vanished and I do
    // not know which slider did it" was the actual report; the two ways to get
    // there are a startLevel past the deepest branch and a clumpsPerStem of 0,
    // and neither is visible in a voxel count of zero.
    const deepest = (params.levels | 0) - 1;
    const bald = leaf === 0 && params.foliage.clumpsPerStem > 0
        ? (params.foliage.startLevel > deepest
            ? ' · <span class="warn">NO FOLIAGE: lowest leafy level ' +
              params.foliage.startLevel + ' is past the deepest branch (' +
              deepest + ' at ' + params.levels + ' levels)</span>'
            : ' · <span class="warn">NO FOLIAGE</span>')
        : '';
    els.stats.innerHTML =
      (quad ? `<b>quad</b> seeds ${seeds.join(', ')} · ` : '') +
      `<b>${lastResult.dim.x}×${lastResult.dim.y}×${lastResult.dim.z}</b> · ` +
      `${(wood + leaf).toLocaleString()} voxels ` +
      `(${wood.toLocaleString()} wood, ${leaf.toLocaleString()} leaf) · ` +
      `${m.stems} stems, ${m.clumps} clumps · reach ${m.reachXZ}, ` +
      `above ${m.above} · ${Math.round(performance.now() - t0)} ms` + bald +
      (truncated ? ' · <span class="warn">TRUNCATED (hit the stem/clump cap)</span>' : '') +
      (clipped ? ' · <span class="warn">CLIPPED (over ' + TG.MAX_DIM + ' voxels on an axis)</span>' : '') +
      (missing.length ? ' · <span class="warn">materials.json has no ' +
        missing.join(', ') + '</span>' : '');
  };
  if (now) run(); else genTimer = setTimeout(run, 60);
}

/* The skeleton overlay: the stems the voxels were stamped from, drawn as lines
 * through WorldView's own line layer. Worth having because a tree that looks
 * wrong is usually wrong in the SKELETON — a branch angle, a split, a tropism —
 * and the voxels hide it. */
function drawSkeleton() {
  if (!wv) return;
  if (!els.skeleton.checked) { wv.onOverlay = null; return; }
  // Every tree currently laid out, not just the subject — a quad with one
  // skeleton in it looks like three trees failed to draw one.
  const set = placed.map(p => ({
    // A stem point is anchor-relative, and the region's origin already backs
    // the anchor out, so the trunk lands exactly on (tx, tz).
    gx: p.tx, gz: p.tz, stems: p.res.skeleton.stems
  }));
  wv.onOverlay = (push) => {
    for (const t of set) {
      for (const st of t.stems) {
        const c = st.level === 0 ? [1, 0.75, 0.2, 1]
                : st.level === 1 ? [0.4, 0.85, 1, 1]
                : [0.75, 0.45, 1, 1];
        for (let i = 0; i + 1 < st.pts.length; i++) {
          const a = st.pts[i], b = st.pts[i + 1];
          push(a[0] + t.gx + 0.5, a[1] + 0.5, a[2] + t.gz + 0.5,
               b[0] + t.gx + 0.5, b[1] + 0.5, b[2] + t.gz + 0.5, c);
        }
      }
    }
  };
}

/* ===========================================================================
 * UI
 * ======================================================================== */
// `base` is a dotted path from `params` ('' for the top level, 'foliage',
// 'placement', 'levelsData.2'), NOT the sub-object itself.
//
// Passing the object was the obvious thing and it was wrong: undo replaces
// `params` wholesale, and every row that had captured `params.foliage` was
// then reading and writing an orphan. The tree changed and none of the 98
// controls moved, which reads exactly like "Ctrl+Z does not work". Resolving
// from the live `params` on every access cannot go stale.
function row(container, r, base, prefix) {
  const el = H.el;
  const path = [base, prefix, r.k].filter(Boolean).join('.');
  const cur = getPath(params, path);
  const num = el('input', {type: 'number', class: 'tgnum', value: String(cur),
                           step: String(r.step), min: String(r.min), max: String(r.max)});
  const rng = el('input', {type: 'range', class: 'tgrng', value: String(cur),
                           step: String(r.step), min: String(r.min), max: String(r.max)});
  // A row may bound itself against another parameter (`startLevel` cannot
  // usefully exceed `levels`). Resolved on every read, never captured, for the
  // same reason `base` is a path and not an object: undo replaces `params`.
  const maxOf = () => (r.maxOf ? r.maxOf(params) : r.max);
  const commit = (v, key) => {
    let x = parseFloat(v);
    if (!isFinite(x)) return;
    x = Math.min(maxOf(), Math.max(r.min, x));
    if (r.int) x = Math.round(x);
    if (x === getPath(params, path)) return;  // a drag that landed on the same step
    snapshot(key);
    setPath(params, path, x);
    num.value = String(x); rng.value = String(x);
    markDirty();
    // A row that other rows are bounded BY has to move them too, or the
    // dependent slider keeps a travel that no longer exists.
    if (r.retunes) refreshWidgets();
    regenerate(false);
  };
  // The RANGE coalesces under the control's own path; the NUMBER box commits
  // once on change, so it gets its own undo step every time (null = never
  // coalesce).
  rng.addEventListener('input', () => commit(rng.value, path));
  num.addEventListener('change', () => commit(num.value, null));
  const line = el('div', {class: 'tgrow', title: r.d || ''},
                  el('label', {}, r.n), rng, num);
  container.append(line);
  const set = (v) => {
    const mx = String(maxOf());
    num.max = mx; rng.max = mx;
    num.value = String(v); rng.value = String(v);
  };
  set(cur);
  widgets.push(() => set(getPath(params, path)));
  return {set: set};
}

function markDirty() {
  dirty = true;
  if (H && H.onDirty) H.onDirty(true);
  if (els.save) els.save.disabled = false;
}

function section(title, note) {
  const el = H.el;
  const body = el('div', {class: 'tgsec-body'});
  const head = el('div', {class: 'tgsec-head'}, title);
  const wrap = el('div', {class: 'tgsec'}, head, body);
  if (note) body.append(el('div', {class: 'tghint'}, note));
  head.addEventListener('click', () => wrap.classList.toggle('closed'));
  return {wrap, body};
}

function buildPanel() {
  const el = H.el;
  const col = els.sliders;
  col.innerHTML = '';
  widgets = [];

  let s = section('Form', 'The silhouette. `crown shape` first — it is what ' +
                  'separates a conifer from an oak before anything else is touched.');
  GLOBAL_ROWS.forEach(r => row(s.body, r, ''));
  col.append(s.wrap);

  const names = ['trunk', 'boughs', 'branches', 'twigs'];
  for (let n = 0; n < 4; n++) {
    const sec = section('Level ' + n + ' — ' + names[n],
                        n >= params.levels ? 'Beyond `branch levels`: not grown.' : null);
    if (n >= params.levels) sec.wrap.classList.add('closed');
    LEVEL_ROWS.forEach(r => {
      // The trunk has no parent to be angled away from or spaced around.
      if (n === 0 && ['branches', 'downAngle', 'downAngleV', 'rotate', 'rotateV',
                      'length', 'lengthV'].includes(r.k)) return;
      row(sec.body, r, 'levelsData.' + n);
    });
    col.append(sec.wrap);
  }

  s = section('Foliage', 'Leaves live in LOBES around branch tips, never in one ' +
              'canopy-sized ball. `shade: lobe vs canopy` is most of the look.');
  FOLIAGE_ROWS.forEach(r => row(s.body, r, 'foliage'));
  col.append(s.wrap);

  s = section('Materials', 'By NAME, resolved against materials.json when the ' +
              'atlas loads. Each ramp is dark → plain → lit; the ' +
              'voxelizer picks between them from a baked normal, which is how a ' +
              'crown gets form for free.');
  ['bark', 'leaf', 'autumnLeaf'].forEach(kind => {
    const mats = (H.materials && H.materials()) || [];
    const opts = mats.map(m => m.id);
    for (let i = 0; i < 3; i++) {
      const sel = el('select', {class: 'tgnum'});
      opts.forEach(o => sel.append(el('option', {value: o}, o)));
      sel.value = params[kind][i];
      sel.addEventListener('change', () => {
        if (params[kind][i] === sel.value) return;
        snapshot(null);
        params[kind][i] = sel.value; markDirty(); regenerate(false);
      });
      widgets.push(() => { sel.value = params[kind][i]; });
      s.body.append(el('div', {class: 'tgrow'},
        el('label', {}, kind + ' ' + ['dark', 'plain', 'lit'][i]), sel));
    }
  });
  col.append(s.wrap);

  s = section('Placement', 'Where this species grows. Read by the ENGINE, not ' +
              'by the voxelizer — it rides along in the baked .svtree so ' +
              'that adding a tree touches one file.');
  PLACEMENT_ROWS.forEach(r => row(s.body, r, r.root ? '' : 'placement'));
  col.append(s.wrap);
}

/* ===========================================================================
 * files
 * ======================================================================== */
async function listSpecies() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const seen = new Set();
  return (j.files || [])
      .filter(f => f.dir === 'trees' && f.name.endsWith('.json'))
      .map(f => f.name.slice(0, -5))
      .filter(n => (seen.has(n) ? false : (seen.add(n), true)))
      .sort();
}

async function loadSpecies(name) {
  const r = await fetch('/api/model?path=trees/' + encodeURIComponent(name + '.json'),
                        {cache: 'no-store'});
  if (!r.ok) throw new Error('HTTP ' + r.status);
  params = TG.normalizeParams(await r.json());
  // Loading a different species is not an edit: an undo that silently swapped
  // the tree you are looking at for the last one would be worse than no undo.
  undoStack = []; redoStack = []; coalesceKey = null;
  speciesName = name;
  dirty = false;
  if (H && H.onDirty) H.onDirty(false);
  if (els.save) els.save.disabled = true;
  buildPanel();
  wv && (wv._treeFramed = false);
  regenerate(true);
}

async function saveSpecies(asName) {
  const name = asName || speciesName;
  if (!name) return;
  const body = JSON.stringify(params, null, 2) + '\n';
  const r = await fetch('/api/model?path=trees/' + encodeURIComponent(name + '.json'),
                        {method: 'POST', body});
  const j = await r.json();
  if (!j.ok) { H.toast('save failed: ' + (j.error || '?'), true); return; }
  speciesName = name;
  dirty = false;
  if (H && H.onDirty) H.onDirty(false);
  els.save.disabled = true;
  H.toast('saved trees/' + name + '.json — bake to make the engine see it');
  await refreshList(name);
}

async function bake() {
  els.bake.disabled = true;
  els.bake.textContent = 'baking…';
  // Yield a frame so the button repaints before a multi-second synchronous bake.
  await new Promise(r => setTimeout(r, 0));
  try {
    const t0 = performance.now();
    const out = TG.bakeAtlas(params);
    const r = await fetch('/api/model?path=trees/' +
                          encodeURIComponent(speciesName + '.svtree'),
                          {method: 'POST', body: out.buf});
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || '?');
    H.toast('baked ' + speciesName + '.svtree — ' + out.meta.variants +
            ' variants, ' + (out.meta.bytes / 1024).toFixed(0) + ' KiB, ' +
            Math.round(performance.now() - t0) + ' ms. The world hash MOVES: ' +
            'run --selftest --rebaseline.');
  } catch (e) {
    H.toast('bake failed: ' + (e && e.message || e), true);
  }
  els.bake.disabled = false;
  els.bake.textContent = 'Bake .svtree';
}

/* Export the current variant as a .vox prefab, for hand-placement with the
 * in-game prefab tool. Round-tripped before it is written, exactly as
 * editor.js does: a .vox that does not read back is a file that silently
 * places the wrong thing. */
async function exportVox() {
  if (!lastResult) return;
  const res = lastResult;
  if (res.dim.x > VOX.MAX_DIM || res.dim.y > VOX.MAX_DIM || res.dim.z > VOX.MAX_DIM) {
    H.toast('too big for .vox: ' + res.dim.x + '×' + res.dim.y + '×' +
            res.dim.z + ' exceeds ' + VOX.MAX_DIM + ' per axis. The engine’s ' +
            'own atlas has no such limit — bake instead.', true);
    return;
  }
  const mats = (H.materials && H.materials()) || [];
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
  const name = speciesName + '_' + seed;
  try {
    const buf = VOX.writeVox(VOX.gridToModel(g, name));
    const back = VOX.readVox(buf);
    if (!back || !back.prefab) throw new Error('round trip produced nothing');
    const r = await fetch('/api/model?path=prefabs/' + encodeURIComponent(name + '.vox'),
                          {method: 'POST', body: buf});
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || '?');
    H.toast('wrote prefabs/' + name + '.vox — press R in game to reload prefabs');
  } catch (e) {
    H.toast('.vox export failed: ' + (e && e.message || e), true);
  }
}

async function refreshList(select) {
  const names = await listSpecies();
  els.species.innerHTML = '';
  names.forEach(n => els.species.append(H.el('option', {value: n}, n)));
  if (select && names.includes(select)) els.species.value = select;
  return names;
}

/* ===========================================================================
 * mount
 * ======================================================================== */
// EVERY CLASS HERE IS PREFIXED 'tg', and that is not stylistic. 'trow', 'tnum'
// and 'tsec' are the TUNING tab's own global classes, and '.trow' carries a
// six-column grid -- so reusing the names laid these rows out on the Tuning
// tab's template and stacked each slider on top of its own number box. One
// page, one namespace: a tab that mounts into it owns a prefix, not a word.
//
// (This comment lives OUT here on purpose. Inside the template literal below,
// the backticks it used to quote those class names closed the string.)
const CSS = `
#view-trees{display:flex;gap:12px;height:calc(100vh - 150px);min-height:520px}
#view-trees .tgleft{width:360px;flex:0 0 360px;display:flex;flex-direction:column;gap:8px;min-height:0}
#view-trees .tgright{flex:1;display:flex;flex-direction:column;gap:8px;min-width:0}
#view-trees .tgbar{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
#view-trees .tgsliders{overflow-y:auto;flex:1;min-height:0;padding-right:4px}
#view-trees .tgsec{border:1px solid #2a3040;border-radius:6px;margin-bottom:6px;overflow:hidden}
#view-trees .tgsec-head{background:#1b2130;padding:6px 9px;font-weight:600;cursor:pointer;user-select:none}
#view-trees .tgsec-head:before{content:'\\25be ';opacity:.6}
#view-trees .tgsec.closed .tgsec-head:before{content:'\\25b8 '}
#view-trees .tgsec.closed .tgsec-body{display:none}
#view-trees .tgsec-body{padding:6px 8px}
#view-trees .tgrow{display:grid;grid-template-columns:130px minmax(0,1fr) 62px;gap:6px;align-items:center;margin:3px 0}
#view-trees .tgrow label{font-size:11px;color:#9fb0c8;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#view-trees .tgrng{width:100%;min-width:0;margin:0;accent-color:var(--acc,#5aa9e6);cursor:pointer}
#view-trees .tgnum{width:100%;min-width:0;box-sizing:border-box;background:#12161f;color:#dbe4f0;border:1px solid #2a3040;border-radius:4px;padding:2px 4px;font:11px monospace;text-align:right}
#view-trees .tghint{font-size:11px;color:#7c8ba3;margin:2px 0 6px;line-height:1.35}
#view-trees canvas{flex:1;min-height:0;width:100%;background:#0e1116;border:1px solid #2a3040;border-radius:6px}
#view-trees .tgstats{font:11px/1.5 monospace;color:#9fb0c8}
#view-trees .tgstats .warn{color:#ffb454}
`;

export function attach(hooks) {
  H = hooks;
  const el = H.el;
  const root = H.section;
  if (!root) return;

  const style = document.createElement('style');
  style.textContent = CSS;
  document.head.append(style);

  els.species = el('select', {class: 'tgnum', style: 'width:150px'});
  els.save = el('button', {disabled: true}, 'Save');
  els.saveAs = el('button', {}, 'Save as…');
  els.bake = el('button', {}, 'Bake .svtree');
  els.vox = el('button', {}, 'Export .vox');
  els.seed = el('input', {type: 'number', class: 'tgnum', value: '0',
                          style: 'width:56px', min: '0', max: '9999'});
  els.skeleton = el('input', {type: 'checkbox'});
  els.quad = el('input', {type: 'checkbox'});
  els.undo = el('button', {title: 'Undo the last parameter change (Ctrl+Z)'}, '↶');
  els.redo = el('button', {title: 'Redo (Ctrl+Shift+Z)'}, '↷');
  els.stats = el('div', {class: 'tgstats'}, 'loading…');
  els.sliders = el('div', {class: 'tgsliders'});
  const cv = el('canvas');

  els.species.addEventListener('change', () => {
    if (dirty && !confirm('Discard unsaved changes to ' + speciesName + '?')) {
      els.species.value = speciesName; return;
    }
    loadSpecies(els.species.value).catch(e => H.toast(String(e), true));
  });
  els.save.addEventListener('click', () => saveSpecies());
  els.saveAs.addEventListener('click', () => {
    const n = prompt('Species name (a file under assets/trees/):', speciesName);
    if (n) saveSpecies(n.trim().replace(/[^a-zA-Z0-9_-]/g, '_'));
  });
  els.bake.addEventListener('click', bake);
  els.vox.addEventListener('click', exportVox);
  els.seed.addEventListener('change', () => {
    seed = Math.max(0, parseInt(els.seed.value, 10) || 0);
    regenerate(true);
  });
  els.skeleton.addEventListener('change', () => {
    if (lastResult) drawSkeleton();
  });
  els.quad.addEventListener('change', () => {
    quad = els.quad.checked;
    // The scene it framed for is not the scene it is about to draw: one tree
    // and a 2x2 stand want completely different camera distances, and keeping
    // the old one puts the quad off the bottom of the canvas.
    if (wv) wv._treeFramed = false;
    regenerate(true);
  });
  els.undo.addEventListener('click', undo);
  els.redo.addEventListener('click', redo);

  // DOCUMENT-level, in the CAPTURE phase, and gated on this tab being visible.
  //
  // Capture because WorldView binds its own Ctrl+Z on the canvas (undo of a
  // VOXEL edit, which this tab does not do) and whichever had focus would
  // otherwise decide what Ctrl+Z means. Gated on visibility because the tuner
  // is one page: an unconditional document handler here would eat Ctrl+Z on
  // the Models tab as well.
  document.addEventListener('keydown', (e) => {
    if (!root.classList.contains('active')) return;
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = (e.key || '').toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); undo(); }
    else if ((k === 'z' && e.shiftKey) || k === 'y') {
      e.preventDefault(); e.stopImmediatePropagation(); redo();
    }
  }, true);

  root.append(
    el('div', {class: 'tgleft'},
      el('div', {class: 'tgbar'}, el('span', {class: 'hint'}, 'species'),
         els.species, els.save, els.saveAs),
      el('div', {class: 'tgbar'}, els.bake, els.vox, els.undo, els.redo,
         el('span', {class: 'hint'}, 'seed'), els.seed,
         el('label', {class: 'hint',
                      style: 'display:flex;align-items:center;gap:4px',
                      title: 'Show four seeds at once — this seed and the ' +
                             'next three — one per quadrant, under one ' +
                             'camera. Step the seed box to re-roll the set. ' +
                             'Save, Bake and Export still act on the first.'},
            els.quad, 'quad'),
         el('label', {class: 'hint',
                      style: 'display:flex;align-items:center;gap:4px'},
            els.skeleton, 'skeleton')),
      els.sliders),
    el('div', {class: 'tgright'}, cv, els.stats));

  // The viewer. streaming:false is what stops it asking a server for terrain
  // regions that do not exist here (assets/worldview.js update()).
  try {
    wv = new WorldView(cv, {streaming: false, seaY: 0});
    wv.view.levels = 1;
    wv.view.showFar = false;
    wv.view.showAxes = false;
    wv.view.grid = 0;
    wv.cam.mode = 'orbit';
    wv.tool.name = 'none';
    const mats = (H.materials && H.materials()) || [];
    if (mats.length) wv.loadPalette(paletteFromMaterials(mats));
    const loop = () => {
      if (wv && document.getElementById('view-trees')?.classList.contains('active')) {
        wv.render();
      }
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
  } catch (e) {
    console.error('trees: WebGL2 view failed', e);
    els.stats.textContent = 'the 3D preview needs WebGL2: ' + (e && e.message || e);
  }
}

/* The viewer wants /api/voxpalette's shape. That route is emitted by the ENGINE
 * from its compiled table, and this tab must work with no built exe — so build
 * the same shape out of the materials.json the page already holds. Index i is
 * material id i+1, which is the engine's own convention (air is synthesised at
 * index 0 by LoadAssets and is not in the file). */
function paletteFromMaterials(mats) {
  const CLASS = {solid: 0, powder: 1, liquid: 2, gas: 3};
  return {
    chunk: 16,
    materials: [{id: 0, name: 'air', colors: ['#000000'], class: 3, emission: 0,
                 opacity: 0, flags: 8}].concat(mats.map((m, i) => ({
      id: i + 1,
      name: m.id,
      colors: m.colors && m.colors.length ? m.colors : ['#ff00ff'],
      class: CLASS[m.class] !== undefined ? CLASS[m.class] : 0,
      emission: m.emission || 0,
      opacity: m.opacity === undefined ? 255 : m.opacity,
      flags: 0
    })))
  };
}

/** Called by the host when the tab becomes visible. */
export function activate() {
  if (!params) {
    refreshList().then(names => {
      const pick = names.includes('oak') ? 'oak' : names[0];
      if (!pick) {
        els.stats.textContent =
          'no species in assets/trees/ — run `node scripts/bake_trees.mjs --seed`';
        return;
      }
      return loadSpecies(pick);
    }).catch(e => {
      els.stats.textContent = 'could not list species: ' + (e && e.message || e);
    });
  }
}

/** Ctrl+S on this tab saves the species file, not the JSON trio. */
export function saveFromHost() { if (speciesName) saveSpecies(); }
export function isDirty() { return dirty; }

/** The WorldView instance, for assets/trees_test.html.
 *
 * A TEST SEAM, and a deliberate one: a canvas without preserveDrawingBuffer
 * reads back as zeros unless readPixels runs in the same task as a draw, so a
 * harness that cannot ask for a frame cannot tell "nothing was drawn" from
 * "nothing was retained" — and the first is the failure this tab would ship
 * with. Nothing in the tuner calls this. */
export function _view() { return wv; }

/** Undo-stack depth, for assets/trees_test.html. A test seam like _view(). */
export function _undoDepth() { return undoStack.length; }
