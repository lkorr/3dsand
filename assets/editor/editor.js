/* ============================================================================
   editor.js — the "Models" tab: a 3D voxel model editor inside the tuner.

   Contract (docs/PLAN_voxel_editor.md §D, wave 1b):
     - three.js view, one InstancedMesh of unit cubes, full rebuild per edit
     - hand-written DDA picking (cell + face normal), no raycaster
     - attach / erase / paint modes; voxel / box / face brushes; mirror X
     - palette fed from assets/materials/materials.json (index+1 == material ID)
     - per-stroke diff undo/redo
     - save through /api/model with a .vox write->read->compare assertion

   Design notes worth keeping:
     - The model is a dense Uint8Array of material IDs in ENGINE space. All
       axis conversion lives in vox.js; nothing here touches vox coordinates.
     - Everything the editor mutates goes through applyOps(), so undo, mirror
       and the dirty flag have exactly one place to hook into.
     - The tuner page must survive this file failing to load (no WebGL, no
       three.js): init() is lazy, guarded, and reports into the pane instead of
       throwing.

   Sections below, in order:
     1  imports + module state
     2  materials / palette
     3  model document (grid, ops, undo)
     4  three.js scene
     5  instance rebuild
     6  DDA picking
     7  brushes (voxel / box / face)
     8  input (mouse + keyboard)
     9  UI (toolbar, palette, file list)
    10  file I/O (open / save / new)
    11  public entry points
   ========================================================================== */

/* ==========================================================================
   1. imports + module state
   ========================================================================== */

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import {
  readVox, writeVox, roundTripTest, prefabRoundTripTest, axisSelfTest,
  gridToModel, prefabToVoxModels, tightenPrefab, makeGrid, gridGet, gridSet,
  paletteFromMaterials, gridColorGet, gridColorSet, gridColorLayer,
  ArtPalette, ART_SLOTS, isArtIndex, paletteColor,
} from './vox.js';

// Editable box cap, matching what the .vox format allows.
//
// This used to be 128, because the ±120 DebrisVoxel int8 bound applied to the
// authored lattice — one resolution served as both art and collider. Since the
// skin/collider split that bound applies only to the DERIVED collider, which
// the engine coarsens to fit (mob.h MobDef::physScale), so the art is free to
// be finer: mina is 136 skin voxels tall at skinScale 8.
const MAX_EDIT_DIM = 256;
// Instance budget for the viewport, independent of MAX_EDIT_DIM: a dense
// 256³ would be 16M cubes, but real models are shells — the "view capped"
// toast catches the pathological case instead of pre-paying for it. Unchanged
// by the cap going to 256 precisely because it was never a function of it.
const INSTANCE_CAP = 64 ** 3;

// --- document ------------------------------------------------------------
// A document is a PREFAB: an ordered list of models in engine space, each with
// its own offset inside the prefab box. Single-volume art is simply a prefab
// with one model at offset 0. Mobs are one model per limb (named via the .vox
// scene graph); flipbooks are one model per frame.
//
// `grid` always aliases doc.models[activeModel].grid so every existing edit
// path (brushes, undo, picking) keeps working unchanged on the active model.
let doc = null;               // { size:{x,y,z}, models:[{name,offset,dim,grid}] }
let activeModel = 0;
let grid = null;              // alias of the active model's grid
let docPath = null;           // "models/foo.vox", or null for an unsaved model
let docName = 'untitled';
let sidecar = null;           // parsed sidecar JSON, or null
let sidecarPath = null;
let docDirty = false;

// --- tools ---------------------------------------------------------------
const MODES = ['attach', 'erase', 'paint'];
let mode = 'attach';
const BRUSHES = ['voxel', 'box', 'face', 'select', 'noise', 'move'];
let brush = 'voxel';
let selection = null;         // { lo:[x,y,z], hi:[x,y,z], model } active-model-local
let mirror = { x: false, y: false, z: false };   // Y/Z wired, UI exposes X
let activeMat = 1;
// Whole-model mode [W]: edit the assembled prefab as one canvas — picking and
// brushes cross model boundaries and each write lands in the model that owns
// the cell. Off = classic per-model editing on the active model.
let wholeMode = false;
let brushSize = 1;            // spherical radius for voxel/noise brushes (1 = single cell)
let noiseDensity = 0.35;      // fraction of surface cells the noise brush hits

/* --- art colour ----------------------------------------------------------
   A voxel carries two independent facts: its MATERIAL (what it is made of —
   meat, wood, stone; this is what the sim reacts to, and what a dismembered
   limb becomes when its voxels land back in the grid) and its COLOUR (what
   it looks like). A creature is meat everywhere and painted all over, so the
   two cannot be the same channel.

   `artColor` is the brush colour, null meaning "paint the material's own
   colour", i.e. clear any art colour on the cell. `artAlpha` is coverage: at
   1 the brush colour replaces, below 1 it is mixed into whatever colour the
   voxel already shows, which is what makes layering translucent glazes work.
   The mix happens at WRITE time against the resolved colour of each cell —
   there is no stored alpha, because a voxel is opaque; only the brush is not. */
let artColor = null;          // "#rrggbb" or null (= material colour)
let artAlpha = 1;             // 0..1 brush coverage
let artPalette = new ArtPalette();   // per-document; indices live in grid.color
let recentColors = [];        // most-recent-first, deduped, capped
const kRecentMax = 12;
// Paint mode with a colour selected touches COLOUR ONLY, leaving the material
// alone. That is what you want almost always: a creature is meat everywhere
// and you are painting its skin, not restaining it into a different substance.
// Turn it off to repaint colour and material in one stroke.
let artColorOnly = true;

// --- undo ----------------------------------------------------------------
// Entries are {type, data}. Voxel: data is a flat array of
// [modelIndex, cellIndex, oldMat, newMat, oldArt, newArt] tuples — material
// and art colour move together so one Ctrl+Z takes back a whole brush stroke
// rather than half of it. Color: data is {matIndex, variant, oldHex, newHex}
// (the MATERIAL colour wheel, a different thing from per-voxel art colour).
// The typed wrapper lets these share one stack without changing the voxel
// path's flat-array perf.
const kOpStride = 6;
let undoStack = [], redoStack = [], stroke = null;

// --- clipboard -----------------------------------------------------------
let clipboard = null;  // { dim:{x,y,z}, data:Uint8Array }

// --- three ---------------------------------------------------------------
let renderer = null, scene = null, camera = null, controls = null;
let cubes = null;             // InstancedMesh
let ghost = null;             // hover highlight
let boundsBox = null;         // wireframe outline of the model bounds
let mirrorPlane = null;
let microGhost = null, microSubdiv = 0;
let gizmo = null, gizmoBall = null, gizmoArc = null, gizmoAxis = null;
// Three lines showing a SOCKET'S FRAME — which way the held item's own X/Y/Z
// end up pointing once socket x grip is composed. Separate from the rotation
// rings (which are drag handles at fixed world orientation): these are pure
// readout, and they turn with the grip so "roll 30" has a visible meaning.
let socketAxes = null;
let gridHelper = null, axes = null;
let selectionOutline = null;
let hiltBox = null;            // wireframe outline of the item's hilt region
// The swept blade of a previewed attack: one line per recorded tick, coloured
// by the stroke phase so the telegraph reads apart from the cut. A generic
// coloured-segment overlay rather than a stroke-specific object, because "draw
// me these lines" is the only thing rig.js needs and a second consumer should
// not need a second mesh.
let strokeTrail = null;
let resizeHandles = [];        // 6 spheres, one per bounding-box face
let canvas = null, host = null;
let initialised = false, initFailed = false;
let needsRebuild = false;

// --- picking / drag state -------------------------------------------------
let hover = null;             // { cell:[x,y,z], normal:[x,y,z], place:[x,y,z] }
let drag = null;              // box-brush drag: { anchor, plane, last }
let pointer = { x: 0, y: 0, inside: false };

// --- host page hooks (set by attach()) -----------------------------------
let hooks = { toast: (m) => console.log(m), materials: () => [], onDirty: () => {} };

/* ==========================================================================
   2. materials / palette

   The tuner's own material list is `mat.materials`, in ID order, where array
   index i is GPU material ID i+1 (id 0 is implicit air — see the "id#" column
   in the materials table, which prints i+1). We derive the palette the same
   way so a model painted here matches what the engine renders.
   ========================================================================== */

let materials = [];           // [{id, colors:[...]}, ...] index i == material ID i+1
let palette = new Uint8Array(1024);

function refreshMaterials() {
  materials = hooks.materials() || [];
  palette = paletteFromMaterials(materials);
  // Art colours live in the same RGBA chunk, above the material IDs — so the
  // palette has to be rebuilt whenever EITHER half changes. Doing it here
  // means every existing call site already does the right thing.
  artPalette.writeInto(palette);
}

const matName = id => (materials[id - 1]?.id) || ('#' + id);
const matColor = id => (id > 0 && materials[id - 1])
  ? ((materials[id - 1].colors || [])[0] || '#888888')
  : '#888888';

/* ---- art colour resolution ---------------------------------------------
   What a voxel actually LOOKS like: its art colour when it has been painted,
   otherwise its material's colour. Every display path (viewport, thumbnail,
   eyedropper, the blend below) goes through this one function, so painted
   and unpainted voxels can never disagree about what is on screen.       */

const hexToRgb = hex => {
  const s = String(hex || '#888888').replace('#', '');
  const n = parseInt(s.length === 3
    ? s[0] + s[0] + s[1] + s[1] + s[2] + s[2] : s.slice(0, 6), 16) | 0;
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};
const rgbToHex = (r, g, b) =>
  '#' + [r, g, b].map(v => Math.max(0, Math.min(255, Math.round(v)))
                            .toString(16).padStart(2, '0')).join('');

// Colour of a cell given its material id and art index (0 = unpainted).
function shownColor(mat, art) {
  if (art) {
    const c = artPalette.colorAt(art);
    if (c) return c;
  }
  return matColor(mat);
}

/** Mix `over` onto `under` at coverage `a`. Plain source-over on straight RGB. */
function blendHex(under, over, a) {
  if (a >= 1) return over;
  const u = hexToRgb(under), o = hexToRgb(over);
  return rgbToHex(u[0] + (o[0] - u[0]) * a,
                  u[1] + (o[1] - u[1]) * a,
                  u[2] + (o[2] - u[2]) * a);
}

function pushRecent(hex) {
  if (!hex) return;
  recentColors = [hex, ...recentColors.filter(c => c !== hex)].slice(0, kRecentMax);
}

/* ==========================================================================
   3. model document — grid, ops, undo

   applyOps() is the ONLY function that writes into grid.data. Mirroring is
   expanded here rather than at each call site, so every brush gets it free
   and undo records the mirrored cells too.
   ========================================================================== */

// The box the brushes work in: the active model's grid, or the whole prefab.
const editDim = () => (wholeMode && doc ? doc.size : grid.dim);

function inBounds(x, y, z) {
  const d = editDim();
  return x >= 0 && y >= 0 && z >= 0 && x < d.x && y < d.y && z < d.z;
}

// Material at an EDIT-SPACE cell (model-local, or prefab-space in whole mode).
function cellGet(x, y, z) {
  if (!wholeMode) return gridGet(grid, x, y, z);
  for (let mi = 0; mi < doc.models.length; mi++) {
    // Hidden limbs are transparent to the raycast too, so you can click
    // through a hidden torso onto the arm behind it.
    if (!modelVisible(mi)) continue;
    const m = doc.models[mi];
    const v = gridGet(m.grid, x - m.offset.x, y - m.offset.y, z - m.offset.z);
    if (v) return v;
  }
  return 0;
}

/**
 * Which model a WRITE at an edit-space cell lands in. The model that already
 * has a voxel there wins (paint/erase must hit what you see); an empty cell
 * goes to the active model if its box covers it, else the first box that
 * does — attach in whole mode cannot grow outside every box, which the help
 * text says out loud.
 */
function ownerOf(x, y, z) {
  if (!wholeMode) return { mi: activeModel, lx: x, ly: y, lz: z };
  let empty = null;
  for (let mi = 0; mi < doc.models.length; mi++) {
    // A hidden limb is not a paint target: writing into something invisible
    // is indistinguishable from the edit being dropped.
    if (!modelVisible(mi)) continue;
    const m = doc.models[mi];
    const lx = x - m.offset.x, ly = y - m.offset.y, lz = z - m.offset.z;
    const d = m.dim;
    if (lx < 0 || ly < 0 || lz < 0 || lx >= d.x || ly >= d.y || lz >= d.z) continue;
    if (m.grid.data[lx + ly * d.x + lz * d.x * d.y]) return { mi, lx, ly, lz };
    if (empty === null || mi === activeModel) empty = { mi, lx, ly, lz };
    if (mi === activeModel) break;    // active box wins among empties
  }
  return empty;
}

// Mirror image of a cell across each enabled axis of the EDIT box. Returns
// the original plus up to 7 reflections, deduplicated (a cell on the mirror
// plane maps to itself, and writing it twice would put a bogus no-op in the
// undo log).
function mirrored(x, y, z) {
  const d = editDim();
  const key = c => c[0] + c[1] * d.x + c[2] * d.x * d.y;
  let out = [[x, y, z]];
  if (mirror.x) out = out.concat(out.map(c => [d.x - 1 - c[0], c[1], c[2]]));
  if (mirror.y) out = out.concat(out.map(c => [c[0], d.y - 1 - c[1], c[2]]));
  if (mirror.z) out = out.concat(out.map(c => [c[0], c[1], d.z - 1 - c[2]]));
  const seen = new Set(), uniq = [];
  for (const c of out) {
    const k = key(c);
    if (seen.has(k)) continue;
    seen.add(k); uniq.push(c);
  }
  return uniq;
}

// Sentinel material for "keep whatever is already in the cell". Deliberately
// outside 0..255 so it can never collide with a real material ID.
const KEEP_MAT = -1;

function beginStroke() { stroke = []; }

/**
 * Write cells. `cells` is an array of [x,y,z] in EDIT space; `value` is the
 * material ID (0 = erase). Mirror expansion, bounds rejection, model routing,
 * no-op filtering and undo recording all happen here.
 *
 * `value` may be KEEP_MAT, meaning "leave the material as it is" — a
 * colour-only stroke. A KEEP_MAT write to an EMPTY cell does nothing: paint
 * needs a voxel to sit on and must never conjure one.
 *
 * `color` decides what happens to the ART COLOUR layer on each written cell:
 *   undefined  leave it alone (material-only edits: attach, plain paint)
 *   0          clear it — the voxel goes back to showing its material colour
 *   a function (mat, art) => artIndex, evaluated PER CELL. Blending needs
 *              this: the result depends on what colour that particular voxel
 *              is already showing, so one scalar cannot express it.
 * Erasing (value 0) always clears colour: paint must not outlive its voxel
 * and reappear when the cell is filled again.
 */
function applyOps(cells, value, color) {
  if (!stroke) beginStroke();
  for (const [cx, cy, cz] of cells) {
    for (const [x, y, z] of mirrored(cx, cy, cz)) {
      if (!inBounds(x, y, z)) continue;
      const o = ownerOf(x, y, z);
      if (!o) continue;                    // whole mode: outside every box
      const g = doc.models[o.mi].grid;
      const i = o.lx + o.ly * g.dim.x + o.lz * g.dim.x * g.dim.y;
      const old = g.data[i];
      const val = value === KEEP_MAT ? old : value;
      if (!val) {
        // Nothing here to paint on (or an erase of empty space): make sure a
        // colour-only stroke cannot leave orphan paint behind.
        if (value === KEEP_MAT) continue;
      }
      const oldArt = g.color ? g.color[i] : 0;
      let art = oldArt;
      if (val === 0) art = 0;
      else if (typeof color === 'function') art = color(old, oldArt) | 0;
      else if (color !== undefined) art = color | 0;
      if (old === val && art === oldArt) continue;  // no-op: keeps undo honest
      g.data[i] = val;
      if (art !== oldArt) gridColorLayer(g)[i] = art;
      stroke.push(o.mi, i, old, val, oldArt, art);
    }
  }
}

function endStroke() {
  if (stroke && stroke.length) {
    commitSidecarUndo();
    undoStack.push({ type: 'voxel', data: stroke });
    redoStack.length = 0;
    markDirty();
    needsRebuild = true;
  }
  stroke = null;
}

function clearUndo() {
  undoStack = []; redoStack = []; stroke = null; _sidecarBefore = null;
  _structDepth = 0; _structBefore = null;
}

/* ---- structural undo -----------------------------------------------------

   A STRUCTURAL edit is one that adds, removes or renumbers MODELS: add,
   duplicate, delete, split-to-model, the 2×/÷2 resamples, and the limb
   library's graft. Every one of them used to call clearUndo() and throw the
   entire history away, for the reason at the top of this file: the voxel log
   addresses cells by MODEL INDEX, and a splice renumbers every model after it.

   That reasoning is right about the log and wrong about the conclusion. The
   log does not have to SURVIVE the renumbering — it only has to be correct
   once the structural entry sitting above it has been undone. Undo is strictly
   LIFO, so a full before/after snapshot of the model list restores exactly the
   indices those older entries were written against, and the history below a
   structural edit becomes valid again the moment that edit is taken back.

   Two things a transaction must do beyond snapshotting:

   - COLLAPSE what happened inside it. splitSelectionToModel runs its erase
     through applyOps so the extraction is one operation, which pushes a voxel
     entry; left in the stack it would be a second Ctrl+Z that re-erases the
     voxels the first one just restored. Everything pushed between begin and
     end is already described by the after-snapshot, so it is dropped.

   - NEST. A limb swap is a graft PLUS sidecar surgery either side of it
     (rig.js removes the doomed limbs before and pushes the incoming ones
     after). An entry pushed inside graftModels alone would snapshot the
     sidecar mid-operation and undo to a skeleton that never existed, so
     rig.js opens the transaction around the whole thing and the graft's own
     begin/end become a no-op depth count.

   What a snapshot deliberately does NOT restore is the ART PALETTE. It is
   append-only (vox.js ArtPalette allocates dense from the top and never
   frees), so every index a restored grid holds was allocated before the
   snapshot and is still there: leaving it alone is always correct and only
   leaks unused swatches. Truncating it back is the unsafe option — a paint
   stroke made after the graft can hold a slot allocated after it, and redoing
   that stroke would then resolve a colour that no longer exists. */

// ~74 KB per snapshot for a humanoid (dense grids + colour layer), so this is
// a couple of MB at the ceiling. A resampled 2× document is 8× that; the cap
// is on the COUNT because that is the number a user can reason about.
const kMaxStructUndo = 30;
let _structDepth = 0, _structBefore = null, _structMark = 0;

function snapshotStructure() {
  return {
    active: activeModel,
    models: doc.models.map(m => ({
      name: m.name, offset: { ...m.offset }, dim: { ...m.dim },
      data: new Uint8Array(m.grid.data),
      color: m.grid.color ? new Uint8Array(m.grid.color) : null,
    })),
    // The sidecar rides along because a structural edit almost always moves it
    // too (a deleted model's limb entry, a grafted part's anchors), and the two
    // have to come back together or the skeleton refers to models that are gone.
    sidecar: sidecar ? JSON.stringify(sidecar) : null,
  };
}

function applyStructureSnapshot(s, label, which) {
  doc.models = s.models.map(p => {
    const g = makeGrid(p.dim);
    g.data.set(p.data);
    if (p.color) gridColorLayer(g).set(p.color);
    return { name: p.name, offset: { ...p.offset }, dim: { ...p.dim }, grid: g };
  });
  // Replaced IN PLACE, not reassigned: rig.js holds the object it got from
  // getSidecar() and would go on editing the old one (the same rule
  // applySidecarSnapshot follows, for the same reason).
  if (s.sidecar !== null && sidecar) {
    const restored = JSON.parse(s.sidecar);
    Object.keys(sidecar).forEach(k => delete sidecar[k]);
    Object.assign(sidecar, restored);
  }
  // reboundDoc() for `doc.size` ALONE, which is stored derived state: whole
  // mode edits against it and tightenPrefab hands it to the .vox writer, so a
  // stale one outlives the undo and gets SAVED. Restoring the models is not
  // enough — the first version of this left a creature reading 4x20x3 after an
  // undo back to 4x17x3.
  //
  // It cannot also shift anything: every mutation path ends in reboundDoc(),
  // so a snapshot is always of an already-rebased document and the min corner
  // it finds is 0.
  reboundDoc();
  setSelection(null);
  setActiveModel(s.active);
  needsRebuild = true;
  if (initialised) { updateResizeHandles(); updateMirrorPlane(); }
  // The heavier of the two hooks: models AND sidecar moved, so rig.js has to
  // rebuild its skeleton and drop selections that may name a limb that is gone.
  hooks.onSidecarChanged?.();
  hooks.onModelsChanged?.();
  hooks.toast(`${which} ${label}`);
}

/** Open a structural transaction. Safe to nest; only the outermost snapshots. */
function beginStructural() {
  if (_structDepth++ > 0 || !doc) return;
  commitSidecarUndo();          // flush anything pending from BEFORE this edit
  _structMark = undoStack.length;
  _structBefore = snapshotStructure();
}

/** Close it. `ok` false = the edit refused itself; record nothing. */
function endStructural(label, ok = true) {
  if (_structDepth > 0) _structDepth--;
  if (_structDepth > 0 || !_structBefore) return;
  const before = _structBefore;
  _structBefore = null;
  // The snapshot pair already carries the sidecar, so a separate entry for it
  // would be a second Ctrl+Z undoing half of one edit.
  discardSidecarUndo();
  if (!ok || !doc) return;
  undoStack.length = _structMark;               // collapse (see above)
  undoStack.push({ type: 'structure',
                   data: { label, before, after: snapshotStructure() } });
  redoStack.length = 0;
  let n = 0;
  for (const e of undoStack) if (e.type === 'structure') n++;
  // Oldest-first eviction: dropping a PREFIX of the stack keeps every entry
  // that remains valid, because undo only ever walks back from the top.
  while (n > kMaxStructUndo && undoStack.length)
    if (undoStack.shift().type === 'structure') n--;
}

/** Run `fn` as one undoable structural edit. Returns whatever `fn` returns;
 *  a `false` return is taken as "refused", the convention these ops use. */
function structuralEdit(label, fn) {
  beginStructural();
  let r;
  try { r = fn(); } catch (e) { endStructural(label, false); throw e; }
  endStructural(label, r !== false);
  return r;
}

function undoVoxel(s) {
  for (let i = s.length - kOpStride; i >= 0; i -= kOpStride) {
    const g = doc.models[s[i]].grid;
    g.data[s[i + 1]] = s[i + 2];
    if (s[i + 4] !== s[i + 5]) gridColorLayer(g)[s[i + 1]] = s[i + 4];
  }
  needsRebuild = true;
  const n = s.length / kOpStride;
  hooks.toast('undo (' + n + ' voxel' + (n === 1 ? '' : 's') + ')');
}
function redoVoxel(s) {
  for (let i = 0; i < s.length; i += kOpStride) {
    const g = doc.models[s[i]].grid;
    g.data[s[i + 1]] = s[i + 3];
    if (s[i + 4] !== s[i + 5]) gridColorLayer(g)[s[i + 1]] = s[i + 5];
  }
  needsRebuild = true;
  hooks.toast('redo (' + (s.length / kOpStride) + ' voxels)');
}
function undoColor(d) {
  const m = materials[d.matIndex];
  if (m && Array.isArray(m.colors)) m.colors[d.variant] = d.oldHex;
  hooks.touchMaterials?.();
  renderPalette();
  needsRebuild = true;
  hooks.toast('undo colour');
}
function redoColor(d) {
  const m = materials[d.matIndex];
  if (m && Array.isArray(m.colors)) m.colors[d.variant] = d.newHex;
  hooks.touchMaterials?.();
  renderPalette();
  needsRebuild = true;
  hooks.toast('redo colour');
}

function applyGrowSnapshot(d, which) {
  const snap = d[which];
  const m = doc.models[d.modelIndex];
  if (!m) return;
  m.grid = { dim: { ...snap.dim }, data: new Uint8Array(snap.data),
             color: snap.color ? new Uint8Array(snap.color) : null };
  m.dim = { ...snap.dim };
  m.offset = { ...snap.offset };
  if (d.modelIndex === activeModel) grid = m.grid;
  if (snap.anchors && sidecar && Array.isArray(sidecar.limbs))
    for (const [name, a] of snap.anchors) {
      const l = sidecar.limbs.find(l => l.name === name);
      if (l && Array.isArray(l.anchor)) { l.anchor[0] = a[0]; l.anchor[1] = a[1]; l.anchor[2] = a[2]; }
    }
  reboundDoc();
  if (initialised) { frameCamera(); updateResizeHandles(); }
  needsRebuild = true;
  hooks.onModelsChanged?.();
  hooks.toast(which === 'before' ? 'undo resize' : 'redo resize');
}

function applyMoveEntry(d, which) {
  const offsets = which === 'before' ? d.before : d.after;
  for (const e of offsets) {
    const m = doc.models[e.mi];
    if (!m) continue;
    m.offset.x = e.offset.x; m.offset.y = e.offset.y; m.offset.z = e.offset.z;
  }
  if (d.anchors) {
    const ancs = which === 'before' ? d.anchors.before : d.anchors.after;
    if (sidecar && Array.isArray(sidecar.limbs))
      for (const [name, a] of ancs) {
        const l = sidecar.limbs.find(x => x.name === name &&
          Array.isArray(x.anchor) && x.anchor.length === 3);
        if (l) { l.anchor[0] = a[0]; l.anchor[1] = a[1]; l.anchor[2] = a[2]; }
      }
  }
  needsRebuild = true;
  if (initialised) updateResizeHandles();
  hooks.onModelsChanged?.();
  hooks.toast(which === 'before' ? 'undo move' : 'redo move');
}

function applySidecarSnapshot(json) {
  if (!json) return;
  const restored = JSON.parse(json);
  Object.keys(sidecar).forEach(k => delete sidecar[k]);
  Object.assign(sidecar, restored);
  needsRebuild = true;
  hooks.onSidecarChanged?.();
}

function dispatchUndo(e) {
  if (e.type === 'voxel') undoVoxel(e.data);
  else if (e.type === 'color') undoColor(e.data);
  else if (e.type === 'grow') applyGrowSnapshot(e.data, 'before');
  else if (e.type === 'move') applyMoveEntry(e.data, 'before');
  else if (e.type === 'sidecar') applySidecarSnapshot(e.data.before);
  else if (e.type === 'structure')
    applyStructureSnapshot(e.data.before, e.data.label, 'undo');
}
function dispatchRedo(e) {
  if (e.type === 'voxel') redoVoxel(e.data);
  else if (e.type === 'color') redoColor(e.data);
  else if (e.type === 'grow') applyGrowSnapshot(e.data, 'after');
  else if (e.type === 'move') applyMoveEntry(e.data, 'after');
  else if (e.type === 'sidecar') applySidecarSnapshot(e.data.after);
  else if (e.type === 'structure')
    applyStructureSnapshot(e.data.after, e.data.label, 'redo');
}

function undo() {
  const e = undoStack.pop();
  if (!e) { hooks.toast('nothing to undo'); return; }
  dispatchUndo(e);
  redoStack.push(e);
  markDirty();
}

function redo() {
  const e = redoStack.pop();
  if (!e) { hooks.toast('nothing to redo'); return; }
  dispatchRedo(e);
  undoStack.push(e);
  markDirty();
}

function markDirty() { docDirty = true; hooks.onDirty(true); updateStatus(); }
function clearDirty() { docDirty = false; hooks.onDirty(false); updateStatus(); }

/* ---- selection + clipboard -------------------------------------------- */

function setSelection(sel) {
  selection = sel;
  hooks.onSelectionChanged?.(selection);
  if (selection && selectionOutline) {
    const lo = selection.lo, hi = selection.hi;
    const o = activeDef()?.offset || { x: 0, y: 0, z: 0 };
    const cx = o.x + (lo[0] + hi[0]) / 2 + 0.5;
    const cy = o.y + (lo[1] + hi[1]) / 2 + 0.5;
    const cz = o.z + (lo[2] + hi[2]) / 2 + 0.5;
    selectionOutline.visible = true;
    selectionOutline.scale.set(hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1);
    selectionOutline.position.set(cx, cy, cz);
  } else if (selectionOutline) {
    selectionOutline.visible = false;
  }
  updateStatus();
}

function selectAll() {
  if (!grid || !doc) return;
  const d = grid.dim;
  setSelection({ lo: [0, 0, 0], hi: [d.x - 1, d.y - 1, d.z - 1], model: activeModel });
  hooks.toast(`selected all (${d.x}×${d.y}×${d.z})`);
}

function deleteSelection() {
  if (!selection || !doc) {
    hooks.toast('no selection', true); return;
  }
  if (activeModel !== selection.model) setActiveModel(selection.model);
  const { lo, hi } = selection;
  const cells = [];
  for (let z = lo[2]; z <= hi[2]; z++)
    for (let y = lo[1]; y <= hi[1]; y++)
      for (let x = lo[0]; x <= hi[0]; x++)
        if (gridGet(grid, x, y, z)) cells.push([x, y, z]);
  if (!cells.length) { hooks.toast('selection is empty', true); return; }
  const savedMirror = mirror;
  mirror = { x: false, y: false, z: false };
  beginStroke();
  applyOps(cells, 0);
  endStroke();
  mirror = savedMirror;
  hooks.toast(`deleted ${cells.length} voxel${cells.length === 1 ? '' : 's'}`);
}

function copySelection() {
  if (!selection || !doc) {
    hooks.toast('no selection — Select [V], Ctrl+A, or click a limb', true); return;
  }
  if (activeModel !== selection.model) setActiveModel(selection.model);
  const { lo, hi } = selection;
  const dx = hi[0] - lo[0] + 1, dy = hi[1] - lo[1] + 1, dz = hi[2] - lo[2] + 1;
  const dim = { x: dx, y: dy, z: dz };
  const data = new Uint8Array(dx * dy * dz);
  // Colours travel as HEX, not as palette indices: the clipboard outlives the
  // document (paste into another model, or after a reload) and an index only
  // means something against the art palette that allocated it.
  const colors = new Array(dx * dy * dz).fill(null);
  let count = 0, painted = 0;
  for (let z = 0; z < dz; z++)
    for (let y = 0; y < dy; y++)
      for (let x = 0; x < dx; x++) {
        const v = gridGet(grid, lo[0] + x, lo[1] + y, lo[2] + z);
        const i = x + y * dx + z * dx * dy;
        data[i] = v;
        if (v) count++;
        const a = gridColorGet(grid, lo[0] + x, lo[1] + y, lo[2] + z);
        if (v && a) { colors[i] = artPalette.colorAt(a); painted++; }
      }
  if (!count) { hooks.toast('selection contains no voxels', true); return; }
  clipboard = { dim, data, colors: painted ? colors : null };
  hooks.toast(`copied ${count} voxel${count === 1 ? '' : 's'} (${dx}×${dy}×${dz})`);
}

function cutSelection() {
  copySelection();
  if (clipboard) deleteSelection();
}

function pasteClipboard() {
  if (!clipboard) { hooks.toast('clipboard empty — Ctrl+C first', true); return; }
  if (!grid || !doc) return;
  let ox = 0, oy = 0, oz = 0;
  if (hover) { const t = targetOf(hover); ox = t[0]; oy = t[1]; oz = t[2]; }
  const d = clipboard.dim;
  // Batch by (material, colour) so one applyOps call covers every cell that
  // shares both — a pasted block is usually a handful of such pairs, not one
  // call per voxel.
  const byPair = new Map();
  let clipped = 0;
  for (let z = 0; z < d.z; z++)
    for (let y = 0; y < d.y; y++)
      for (let x = 0; x < d.x; x++) {
        const i = x + y * d.x + z * d.x * d.y;
        const v = clipboard.data[i];
        if (!v) continue;
        const px = ox + x, py = oy + y, pz = oz + z;
        if (!inBounds(px, py, pz)) { clipped++; continue; }
        const hex = clipboard.colors ? clipboard.colors[i] : null;
        const k = v + '|' + (hex || '');
        if (!byPair.has(k)) byPair.set(k, { mat: v, hex, cells: [] });
        byPair.get(k).cells.push([px, py, pz]);
      }
  if (!byPair.size) {
    hooks.toast(`paste: all ${clipped} voxels outside bounds`, true); return;
  }
  const savedMirror = mirror;
  mirror = { x: false, y: false, z: false };
  beginStroke();
  for (const { mat, hex, cells } of byPair.values())
    applyOps(cells, mat, hex ? (allocArt(hex) ?? 0) : 0);
  endStroke();
  artFullWarned = false;
  mirror = savedMirror;
  let n = 0; for (const p of byPair.values()) n += p.cells.length;
  let msg = `pasted ${n} voxel${n === 1 ? '' : 's'}`;
  if (clipped) msg += ` (${clipped} clipped)`;
  hooks.toast(msg);
}

function fillSelection() {
  if (!selection || !doc) { hooks.toast('no selection', true); return; }
  if (activeModel !== selection.model) setActiveModel(selection.model);
  const { lo, hi } = selection;
  const cells = [];
  for (let z = lo[2]; z <= hi[2]; z++)
    for (let y = lo[1]; y <= hi[1]; y++)
      for (let x = lo[0]; x <= hi[0]; x++)
        cells.push([x, y, z]);
  beginStroke();
  applyOps(cells, activeMat, colorForBrush());
  endStroke();
  artFullWarned = false;
  hooks.toast(`filled ${cells.length} cells with ${matName(activeMat)}` +
              (artColor !== null ? ` · ${artColor}` : ''));
}

/* ---- multi-model document ---------------------------------------------- */

// Recompute the prefab bounding box from the models. The engine rebases to min
// corner 0 on load, so we keep offsets non-negative here to match what it will
// see; a model dragged to a negative offset shifts everything else instead.
function reboundDoc() {
  if (!doc || !doc.models.length) return;
  let mn = { x: Infinity, y: Infinity, z: Infinity };
  let mx = { x: -Infinity, y: -Infinity, z: -Infinity };
  for (const m of doc.models) {
    for (const a of ['x', 'y', 'z']) {
      mn[a] = Math.min(mn[a], m.offset[a]);
      mx[a] = Math.max(mx[a], m.offset[a] + m.dim[a]);
    }
  }
  if (mn.x || mn.y || mn.z)
    for (const m of doc.models)
      for (const a of ['x', 'y', 'z']) m.offset[a] -= mn[a];
  doc.size = { x: mx.x - mn.x, y: mx.y - mn.y, z: mx.z - mn.z };
}

/**
 * Translate a whole model by a voxel delta. Geometry lives in the model's own
 * grid, so moving a limb is purely a change of `offset` — no voxels are
 * rewritten and the shape cannot be clipped or resampled.
 *
 * The limb's `anchor` moves with it. Geometry (model.offset -> restOffset,
 * mob.cpp:692) and joints (sidecar anchor -> anchorLocal) are INDEPENDENT in
 * the format, so a move that touched only one would slide the joint out of the
 * mesh. Carrying both is what makes this a "move the limb" rather than a "move
 * the voxels"; a caller that wants the joint to stay put can pass
 * moveAnchor:false and drag the orange ball afterwards.
 *
 * reboundDoc() may rebase every offset afterwards (it keeps the prefab's min
 * corner at 0, matching the engine's load-time rebase). That shift applies to
 * models and anchors alike, so it is applied to both here — otherwise moving
 * one limb below the origin would silently desync every OTHER limb's anchor.
 */
function moveModel(i, d, moveAnchor = true) {
  if (!doc) return false;
  const m = doc.models[i];
  if (!m) return false;
  const dx = d.x | 0, dy = d.y | 0, dz = d.z | 0;
  if (!dx && !dy && !dz) return false;

  const anchorsOf = name => {
    if (!moveAnchor || !sidecar || !Array.isArray(sidecar.limbs)) return [];
    return sidecar.limbs.filter(l => l.name === name &&
      Array.isArray(l.anchor) && l.anchor.length === 3);
  };
  for (const l of anchorsOf(m.name)) {
    l.anchor[0] += dx; l.anchor[1] += dy; l.anchor[2] += dz;
  }

  const before = { x: m.offset.x, y: m.offset.y, z: m.offset.z };
  m.offset.x += dx; m.offset.y += dy; m.offset.z += dz;

  // Note the rebase by watching what reboundDoc did to THIS model, then apply
  // the same correction to every anchor in the file.
  const expect = { x: before.x + dx, y: before.y + dy, z: before.z + dz };
  reboundDoc();
  const rb = { x: m.offset.x - expect.x, y: m.offset.y - expect.y,
               z: m.offset.z - expect.z };
  if ((rb.x || rb.y || rb.z) && sidecar && Array.isArray(sidecar.limbs))
    for (const l of sidecar.limbs)
      if (Array.isArray(l.anchor) && l.anchor.length === 3) {
        l.anchor[0] += rb.x; l.anchor[1] += rb.y; l.anchor[2] += rb.z;
      }

  // The undo log survives a move: voxel entries are (modelIndex, flatGridIndex,
  // old, new), and a move rewrites no voxels or indices. The caller pushes a
  // 'move' entry on pointer-up; this helper is called per-pixel during drag.
  markDirty();
  needsRebuild = true;
  if (initialised) updateResizeHandles();
  hooks.onModelsChanged?.();
  updateStatus();
  return true;
}

function moveModels(indices, d) {
  if (!doc) return false;
  const dx = d.x | 0, dy = d.y | 0, dz = d.z | 0;
  if (!dx && !dy && !dz) return false;
  const names = new Set(indices.map(i => doc.models[i]?.name).filter(Boolean));
  for (const i of indices) {
    const m = doc.models[i];
    if (!m) continue;
    m.offset.x += dx; m.offset.y += dy; m.offset.z += dz;
    if (sidecar && Array.isArray(sidecar.limbs))
      for (const l of sidecar.limbs)
        if (l.name === m.name && Array.isArray(l.anchor) && l.anchor.length === 3) {
          l.anchor[0] += dx; l.anchor[1] += dy; l.anchor[2] += dz;
        }
  }
  const ref = doc.models[indices[0]];
  const expect = { x: ref.offset.x, y: ref.offset.y, z: ref.offset.z };
  reboundDoc();
  const rb = { x: ref.offset.x - expect.x, y: ref.offset.y - expect.y,
               z: ref.offset.z - expect.z };
  if ((rb.x || rb.y || rb.z) && sidecar && Array.isArray(sidecar.limbs))
    for (const l of sidecar.limbs)
      if (!names.has(l.name) && Array.isArray(l.anchor) && l.anchor.length === 3) {
        l.anchor[0] += rb.x; l.anchor[1] += rb.y; l.anchor[2] += rb.z;
      }
  markDirty();
  needsRebuild = true;
  if (initialised) updateResizeHandles();
  hooks.onModelsChanged?.();
  updateStatus();
  return true;
}

/**
 * Grow a model's box by `pad` voxels on each side that needs it, so there is
 * empty space to paint into. A model's grid is exactly its authored bounds, so
 * after moving a limb the gap it left is inside NO box — ownerOf returns null
 * there and applyOps silently drops the write. That reads as "the editor won't
 * let me paint", which is why this exists as an explicit, undo-clearing op.
 *
 * Voxels keep their world position: the grid is re-blitted at the new offset,
 * and the anchor is untouched because nothing moved on screen.
 */
function snapshotModel(i) {
  const m = doc.models[i];
  const snap = { dim: { ...m.dim }, offset: { ...m.offset },
                 data: new Uint8Array(m.grid.data),
                 color: m.grid.color ? new Uint8Array(m.grid.color) : null };
  if (sidecar && Array.isArray(sidecar.limbs)) {
    snap.anchors = sidecar.limbs
      .filter(l => Array.isArray(l.anchor) && l.anchor.length === 3)
      .map(l => [l.name, l.anchor.slice()]);
  }
  return snap;
}

function growModel(i, pad) {
  if (!doc) return false;
  const m = doc.models[i];
  if (!m) return false;
  const p = {
    lo: { x: pad.lo?.x | 0, y: pad.lo?.y | 0, z: pad.lo?.z | 0 },
    hi: { x: pad.hi?.x | 0, y: pad.hi?.y | 0, z: pad.hi?.z | 0 },
  };
  const dim = { x: m.dim.x + p.lo.x + p.hi.x, y: m.dim.y + p.lo.y + p.hi.y,
                z: m.dim.z + p.lo.z + p.hi.z };
  if (dim.x < 1 || dim.y < 1 || dim.z < 1) return false;

  const before = snapshotModel(i);

  const g = makeGrid(dim);
  let cropped = 0;
  for (let z = 0; z < m.dim.z; z++)
    for (let y = 0; y < m.dim.y; y++)
      for (let x = 0; x < m.dim.x; x++) {
        const v = gridGet(m.grid, x, y, z);
        if (!v) continue;
        const nx = x + p.lo.x, ny = y + p.lo.y, nz = z + p.lo.z;
        if (nx < 0 || ny < 0 || nz < 0 ||
            nx >= dim.x || ny >= dim.y || nz >= dim.z) { cropped++; continue; }
        gridSet(g, nx, ny, nz, v);
        const a = gridColorGet(m.grid, x, y, z);
        if (a) gridColorSet(g, nx, ny, nz, a);
      }
  if (cropped) hooks.toast(`cropped ${cropped} voxel` + (cropped === 1 ? '' : 's'), true);
  m.grid = g; m.dim = dim;
  m.offset.x -= p.lo.x; m.offset.y -= p.lo.y; m.offset.z -= p.lo.z;
  if (i === activeModel) grid = g;
  const expect = { x: m.offset.x, y: m.offset.y, z: m.offset.z };
  reboundDoc();
  const rb = { x: m.offset.x - expect.x, y: m.offset.y - expect.y,
               z: m.offset.z - expect.z };
  if ((rb.x || rb.y || rb.z) && sidecar && Array.isArray(sidecar.limbs))
    for (const l of sidecar.limbs)
      if (Array.isArray(l.anchor) && l.anchor.length === 3) {
        l.anchor[0] += rb.x; l.anchor[1] += rb.y; l.anchor[2] += rb.z;
      }

  const after = snapshotModel(i);
  commitSidecarUndo();
  undoStack.push({ type: 'grow', data: { modelIndex: i, before, after } });
  redoStack.length = 0;

  markDirty();
  needsRebuild = true;
  if (initialised) updateResizeHandles();
  hooks.onModelsChanged?.();
  updateStatus();
  return true;
}

function setActiveModel(i) {
  if (!doc || !doc.models.length) return;
  activeModel = Math.max(0, Math.min(doc.models.length - 1, i | 0));
  grid = doc.models[activeModel].grid;
  // Undo entries carry their model index, so the log survives switching the
  // active model; only an in-flight stroke is dropped.
  stroke = null;
  needsRebuild = true;
  if (initialised) updateResizeHandles();
  hooks.onModelsChanged?.();
  updateStatus();
}

const activeDef = () => (doc && doc.models[activeModel]) || null;

function uniqueModelName(base) {
  const taken = new Set((doc?.models || []).map(m => m.name));
  if (!taken.has(base)) return base;
  let k = 2;
  while (taken.has(base + '_' + k)) k++;
  return base + '_' + k;
}

function addModel(dx, dy, dz, name) {
  if (!doc) return;
  structuralEdit('add model', () => {
    const dim = {
      x: Math.max(1, Math.min(MAX_EDIT_DIM, dx | 0)),
      y: Math.max(1, Math.min(MAX_EDIT_DIM, dy | 0)),
      z: Math.max(1, Math.min(MAX_EDIT_DIM, dz | 0)),
    };
    doc.models.push({
      name: uniqueModelName(name || 'model'),
      offset: { x: 0, y: 0, z: 0 }, dim, grid: makeGrid(dim),
    });
    reboundDoc();
    setActiveModel(doc.models.length - 1);
    markDirty();
  });
}

function duplicateModel(i) {
  if (!doc || !doc.models[i]) return;
  structuralEdit('duplicate model', () => {
    const s = doc.models[i];
    const g = makeGrid(s.dim);
    g.data.set(s.grid.data);
    // The colour layer too. Copying only `data` duplicated the geometry and
    // silently dropped every painted voxel's art index, so the copy came back
    // in flat material colours.
    if (s.grid.color) gridColorLayer(g).set(s.grid.color);
    doc.models.splice(i + 1, 0, {     // the splice renumbers models after i
      name: uniqueModelName(s.name || 'model'),
      offset: { ...s.offset }, dim: { ...s.dim }, grid: g,
    });
    reboundDoc();
    setActiveModel(i + 1);
    markDirty();
  });
}

function removeModel(i) {
  if (!doc || doc.models.length <= 1) {
    hooks.toast('a file needs at least one model', true);
    return;
  }
  structuralEdit('delete model', () => {
    renameVisibility(doc.models[i].name, null);
    doc.models.splice(i, 1);   // the splice renumbers models after i
    reboundDoc();
    setActiveModel(Math.min(i, doc.models.length - 1));
    markDirty();
  });
}

/**
 * Graft foreign models into this document — the load half of the limb library
 * (assets/editor/limblib.js).
 *
 * `parts` are `{name, offset, dim, grid}` in the INCOMING part's own frame;
 * `at` is where that frame's origin goes in this document. `names` that are
 * already taken are the caller's problem (limblib.uniqueNames), because the
 * part's internal parent references have to be renamed in step.
 *
 * ART COLOUR IS THE WHOLE REASON THIS IS NOT A `push`. `grid.color` holds art
 * PALETTE INDICES, and an index is only meaningful next to the palette it was
 * allocated from — index 250 is a scythe's rust in one file and a hood's blue
 * in another. Pushing a foreign grid unchanged silently recolours the limb to
 * whatever this document happens to have in those slots. So every incoming
 * index is resolved to a hex through the SOURCE palette and re-allocated
 * against ours, which is also where an over-full palette degrades gracefully:
 * the voxel keeps its material and loses only its paint.
 *
 * `replace` is a list of model names to drop as the graft lands, so a swap is
 * ONE operation. Doing it as remove-then-add would trip removeModel's "a file
 * needs at least one model" guard when the part being swapped is the only one,
 * and would leave the document briefly limbless if the graft then failed.
 *
 * Returns `{added, shift}`. `shift` is what the rebase moved every model by —
 * the CALLER must add it to any anchor it places in the same frame as `at`,
 * because anchors live in the sidecar and nothing here can see them.
 */
function graftModels(parts, at, srcPalette, replace) {
  if (!doc || !parts.length) return [];
  // Usually a no-op depth count: the limb shelf opens the transaction around
  // the whole swap, because the sidecar surgery either side of this belongs in
  // the same undo step. Kept so a direct caller is still undoable.
  beginStructural();
  try {
  // Source art index -> our index. Cached per graft: a limb is thousands of
  // voxels over a handful of distinct colours.
  const remap = new Map();
  const mapArt = idx => {
    if (!idx) return 0;
    if (remap.has(idx)) return remap.get(idx);
    // paletteColor reads the source file's RGBA chunk at the index directly —
    // no need to rebuild an ArtPalette, whose dense-from-the-top packing would
    // have to be reconstructed for the whole used set to be meaningful.
    const hex = srcPalette ? paletteColor(srcPalette, idx) : null;
    const out = hex ? (artPalette.alloc(hex) || 0) : 0;
    remap.set(idx, out);
    return out;
  };

  for (const name of replace || []) {
    const k = doc.models.findIndex(m => m.name === name);
    if (k < 0) continue;
    renameVisibility(name, null);
    doc.models.splice(k, 1);
  }

  const added = [];
  for (const p of parts) {
    const g = makeGrid(p.dim);
    g.data.set(p.grid.data);
    if (p.grid.color) {
      const dst = gridColorLayer(g);
      for (let i = 0; i < p.grid.color.length; i++)
        if (p.grid.color[i]) dst[i] = mapArt(p.grid.color[i]);
    }
    doc.models.push({
      name: p.name,
      offset: { x: p.offset.x + at.x, y: p.offset.y + at.y, z: p.offset.z + at.z },
      dim: { ...p.dim }, grid: g,
    });
    added.push(p.name);
  }
  // reboundDoc() keeps the prefab's min corner at 0, so a part grafted at a
  // negative offset slides EVERY model — and anchors are not models, so they
  // do not slide with them. Same trap moveModel documents; here the caller
  // owns the sidecar, so the shift is returned rather than applied. Measured
  // against a model that existed before the rebase, since `at` is in the
  // pre-rebase frame the caller computed its anchors in.
  const probe = doc.models[doc.models.length - 1];
  const was = { ...probe.offset };
  reboundDoc();
  const shift = { x: probe.offset.x - was.x, y: probe.offset.y - was.y,
                  z: probe.offset.z - was.z };

  setActiveModel(doc.models.length - 1);
  renderArtPalette();          // the graft may have allocated new art slots
  markDirty();
  hooks.onModelsChanged?.();
  return { added, shift };
  } finally { endStructural('graft limb'); }
}

function renameModel(i, name) {
  const m = doc?.models[i];
  if (!m) return false;
  const n = String(name || '').trim();
  if (!n) return false;
  if (doc.models.some((o, k) => k !== i && o.name === n)) {
    hooks.toast(`a model named "${n}" already exists`, true);
    return false;
  }
  renameVisibility(m.name, n);
  m.name = n;
  markDirty();
  hooks.onModelsChanged?.();
  return true;
}

/**
 * Extract the current box selection into a NEW model in the same file, erasing
 * it from the source. This is how a single-volume sculpt becomes a mob: the
 * engine's format is one model per limb, so a limb has to become its own model
 * before it can carry an anchor and a joint.
 *
 * The new model is tightly re-fit to the voxels actually present in the box
 * (an empty margin would shift its offset and therefore its anchor), and its
 * offset is set so the extracted geometry does not move on screen.
 */
function splitSelectionToModel(name) {
  if (!doc || !selection) { hooks.toast('make a box selection first', true); return false; }
  if (!doc.models[selection.model]) return false;
  return structuralEdit('split to model', () => splitSelectionToModelInner(name));
}

function splitSelectionToModelInner(name) {
  const src = doc.models[selection.model];
  // applyOps below erases from the ACTIVE model; the selection's coordinates
  // are local to the model it was made on, so they must be the same model.
  if (activeModel !== selection.model) setActiveModel(selection.model);
  const { lo, hi } = selection;

  // Tight bounds over the non-empty cells inside the selection.
  let mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
  let count = 0;
  for (let z = lo[2]; z <= hi[2]; z++)
    for (let y = lo[1]; y <= hi[1]; y++)
      for (let x = lo[0]; x <= hi[0]; x++) {
        if (!gridGet(src.grid, x, y, z)) continue;
        count++;
        mn = [Math.min(mn[0], x), Math.min(mn[1], y), Math.min(mn[2], z)];
        mx = [Math.max(mx[0], x), Math.max(mx[1], y), Math.max(mx[2], z)];
      }
  if (!count) { hooks.toast('the selection contains no voxels', true); return false; }

  const dim = { x: mx[0] - mn[0] + 1, y: mx[1] - mn[1] + 1, z: mx[2] - mn[2] + 1 };
  const g = makeGrid(dim);
  beginStroke();
  const moved = [];
  for (let z = mn[2]; z <= mx[2]; z++)
    for (let y = mn[1]; y <= mx[1]; y++)
      for (let x = mn[0]; x <= mx[0]; x++) {
        const v = gridGet(src.grid, x, y, z);
        if (!v) continue;
        gridSet(g, x - mn[0], y - mn[1], z - mn[2], v);
        const a = gridColorGet(src.grid, x, y, z);
        if (a) gridColorSet(g, x - mn[0], y - mn[1], z - mn[2], a);
        moved.push([x, y, z]);
      }
  // Erase from the source through applyOps so the extraction is undoable.
  // Mirror must not apply here — this is a structural move, not a brush.
  const savedMirror = mirror;
  mirror = { x: false, y: false, z: false };
  applyOps(moved, 0);
  mirror = savedMirror;
  endStroke();

  doc.models.push({
    name: uniqueModelName(name || 'part'),
    offset: { x: src.offset.x + mn[0], y: src.offset.y + mn[1], z: src.offset.z + mn[2] },
    dim, grid: g,
  });
  // The erase above went through applyOps and pushed its own voxel entry.
  // Undoing just that would leave the voxels duplicated into the new part, so
  // the transaction collapses it into the one structural step (endStructural).
  reboundDoc();
  setSelection(null);
  setActiveModel(doc.models.length - 1);
  markDirty();
  hooks.toast(`split ${count} voxels into "${doc.models[activeModel].name}"`);
  return true;
}

/**
 * Double the document's resolution: every voxel becomes a 2×2×2 block and
 * every offset doubles, so the prefab keeps its exact shape at twice the
 * grid density. This is the geometry half of "add micro detail to an
 * existing mob" — rig.js doubles the sidecar (anchors, clip pos keys) and
 * bumps `scale` so the creature keeps its world size. rig.js opens one
 * transaction around both halves, so Ctrl+Z takes back the geometry and the
 * sidecar together.
 */
function upscaleDoc() {
  if (!doc) return false;
  for (const m of doc.models) {
    if (m.dim.x * 2 > MAX_EDIT_DIM || m.dim.y * 2 > MAX_EDIT_DIM ||
        m.dim.z * 2 > MAX_EDIT_DIM) {
      hooks.toast(`cannot upscale: "${m.name}" would exceed ${MAX_EDIT_DIM}³`, true);
      return false;
    }
  }
  beginStructural();
  for (const m of doc.models) {
    const nd = { x: m.dim.x * 2, y: m.dim.y * 2, z: m.dim.z * 2 };
    const g = makeGrid(nd);
    const od = m.dim, src = m.grid.data, srcC = m.grid.color;
    const dstC = srcC ? gridColorLayer(g) : null;
    for (let z = 0; z < od.z; z++)
      for (let y = 0; y < od.y; y++)
        for (let x = 0; x < od.x; x++) {
          const si = x + y * od.x + z * od.x * od.y;
          const v = src[si];
          if (!v) continue;
          const a = srcC ? srcC[si] : 0;
          for (let dz = 0; dz < 2; dz++)
            for (let dy = 0; dy < 2; dy++)
              for (let dx = 0; dx < 2; dx++) {
                const di = (x * 2 + dx) + (y * 2 + dy) * nd.x +
                           (z * 2 + dz) * nd.x * nd.y;
                g.data[di] = v;
                if (dstC) dstC[di] = a;
              }
        }
    m.dim = nd;
    m.grid = g;
    m.offset = { x: m.offset.x * 2, y: m.offset.y * 2, z: m.offset.z * 2 };
  }
  setSelection(null);
  reboundDoc();
  grid = doc.models[activeModel].grid;
  markDirty();
  if (initialised) { frameCamera(); updateMirrorPlane(); needsRebuild = true; }
  hooks.onModelsChanged?.();
  endStructural('2× detail');
  return true;
}

function downscaleDoc() {
  if (!doc) return false;
  for (const m of doc.models) {
    if (m.dim.x < 2 && m.dim.y < 2 && m.dim.z < 2) {
      hooks.toast(`cannot downscale: "${m.name}" is already 1×1×1`, true);
      return false;
    }
  }
  beginStructural();
  for (const m of doc.models) {
    const nd = {
      x: Math.max(1, Math.floor(m.dim.x / 2)),
      y: Math.max(1, Math.floor(m.dim.y / 2)),
      z: Math.max(1, Math.floor(m.dim.z / 2)),
    };
    const g = makeGrid(nd);
    const od = m.dim, src = m.grid.data, srcC = m.grid.color;
    const dstC = srcC ? gridColorLayer(g) : null;
    for (let z = 0; z < nd.z; z++)
      for (let y = 0; y < nd.y; y++)
        for (let x = 0; x < nd.x; x++) {
          // Take the most common non-zero material in the 2×2×2 source block,
          // and — independently — the most common colour among the cells that
          // survived. Colour is voted separately because a block can be one
          // material in two different colours, and picking the colour of the
          // winning material's first cell would make the choice order-dependent.
          const counts = {}, ccounts = {};
          let best = 0, bestN = 0, bestC = 0, bestCN = 0;
          for (let dz = 0; dz < 2; dz++)
            for (let dy = 0; dy < 2; dy++)
              for (let dx = 0; dx < 2; dx++) {
                const sx = x * 2 + dx, sy = y * 2 + dy, sz = z * 2 + dz;
                if (sx >= od.x || sy >= od.y || sz >= od.z) continue;
                const si = sx + sy * od.x + sz * od.x * od.y;
                const v = src[si];
                if (!v) continue;
                const c = (counts[v] = (counts[v] || 0) + 1);
                if (c > bestN) { bestN = c; best = v; }
                const a = srcC ? srcC[si] : 0;
                if (!a) continue;
                const ac = (ccounts[a] = (ccounts[a] || 0) + 1);
                if (ac > bestCN) { bestCN = ac; bestC = a; }
              }
          const di = x + y * nd.x + z * nd.x * nd.y;
          g.data[di] = best;
          if (dstC && best) dstC[di] = bestC;
        }
    m.dim = nd;
    m.grid = g;
    m.offset = {
      x: Math.round(m.offset.x / 2),
      y: Math.round(m.offset.y / 2),
      z: Math.round(m.offset.z / 2),
    };
  }
  setSelection(null);
  reboundDoc();
  grid = doc.models[activeModel].grid;
  markDirty();
  if (initialised) { frameCamera(); updateMirrorPlane(); needsRebuild = true; }
  hooks.onModelsChanged?.();
  endStructural('÷2 detail');
  return true;
}

// Fresh single-model document.
function newModel(dx, dy, dz, name = 'untitled') {
  dx = Math.max(1, Math.min(MAX_EDIT_DIM, dx | 0));
  dy = Math.max(1, Math.min(MAX_EDIT_DIM, dy | 0));
  dz = Math.max(1, Math.min(MAX_EDIT_DIM, dz | 0));
  const dim = { x: dx, y: dy, z: dz };
  doc = {
    size: { ...dim },
    models: [{ name: 'model', offset: { x: 0, y: 0, z: 0 }, dim, grid: makeGrid(dim) }],
  };
  activeModel = 0;
  grid = doc.models[0].grid;
  docPath = null; docName = name; sidecar = null; sidecarPath = null;
  clearUndo();               // also resets any open structural transaction
  setSelection(null);
  // Art indices are document-scoped, so a new document starts with an empty
  // palette; keeping the old one would leave indices resolving to colours from
  // a model that is no longer open.
  artPalette = new ArtPalette();
  setArtColor(null);
  clearDirty();
  if (initialised) { frameCamera(); needsRebuild = true; }
  hooks.onModelsChanged?.();
  hooks.onSidecarChanged?.();
  updateStatus();
}

/* ==========================================================================
   4. three.js scene

   One InstancedMesh with a fixed INSTANCE_CAP (262144 instances, ~17 MB of
   instance data) which is more than any real model needs but removes the need
   to reallocate on edit; `count` is set to the number of filled cells each
   rebuild, and overflowing the cap warns instead of reallocating.
   ========================================================================== */

function hasWebGL() {
  try {
    const c = document.createElement('canvas');
    return !!(window.WebGLRenderingContext &&
      (c.getContext('webgl2') || c.getContext('webgl')));
  } catch { return false; }
}

function buildScene() {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(0x0e1116);          // --bg

  camera = new THREE.PerspectiveCamera(50, 1, 0.05, 4000);

  renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));

  controls = new OrbitControls(camera, canvas);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;
  // Left mouse is the edit button, so orbit moves to the right button and
  // pan to the middle. Without this every paint stroke also spins the camera.
  controls.mouseButtons = {
    LEFT: null,
    MIDDLE: THREE.MOUSE.PAN,
    RIGHT: THREE.MOUSE.ROTATE,
  };

  scene.add(new THREE.AmbientLight(0xffffff, 1.05));
  const key = new THREE.DirectionalLight(0xffffff, 1.5);
  key.position.set(0.6, 1.0, 0.45);
  scene.add(key);
  const fill = new THREE.DirectionalLight(0x88aaff, 0.45);
  fill.position.set(-0.5, 0.3, -0.7);
  scene.add(fill);

  // Instanced cubes. Slightly under 1.0 so neighbouring voxels show a hairline
  // seam — it reads as a voxel grid rather than a smooth blob.
  const geo = new THREE.BoxGeometry(0.98, 0.98, 0.98);
  // Per-instance colour comes from setColorAt()/instanceColor alone. Do NOT
  // set vertexColors here: that defines USE_COLOR, whose `color` GEOMETRY
  // attribute BoxGeometry lacks, and the unbound attribute reads (0,0,0) —
  // every voxel renders black.
  const mtl = new THREE.MeshLambertMaterial();
  cubes = new THREE.InstancedMesh(geo, mtl, INSTANCE_CAP);
  cubes.frustumCulled = false;         // we never update the bounding sphere
  cubes.count = 0;
  cubes.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  scene.add(cubes);

  // Hover ghost.
  ghost = new THREE.Mesh(
    new THREE.BoxGeometry(1.01, 1.01, 1.01),
    new THREE.MeshBasicMaterial({
      color: 0x6ea8fe, transparent: true, opacity: 0.30,
      depthWrite: false,
    }));
  ghost.visible = false;
  scene.add(ghost);
  // Crisp edge on the ghost, so a single hovered cell is unmistakable.
  const gedge = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(1.01, 1.01, 1.01)),
    new THREE.LineBasicMaterial({ color: 0x9ec8ff }));
  ghost.add(gedge);

  // Bounds outline.
  boundsBox = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
    new THREE.LineBasicMaterial({ color: 0x39445a }));
  scene.add(boundsBox);

  // Selection outline (persistent green wireframe, separate from hover ghost).
  selectionOutline = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
    new THREE.LineBasicMaterial({ color: 0x7cf03a, transparent: true, opacity: 0.7 }));
  selectionOutline.renderOrder = 998;
  selectionOutline.visible = false;
  scene.add(selectionOutline);

  // Hilt box: wireframe showing where the hand closes on a held item.
  hiltBox = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
    new THREE.LineBasicMaterial({ color: 0xffb454, transparent: true, opacity: 0.85 }));
  hiltBox.renderOrder = 999;
  hiltBox.visible = false;
  scene.add(hiltBox);

  // Stroke trail: vertex-coloured line segments, rebuilt per frame while a
  // swing plays. Depth test OFF so a blade passing behind the torso still
  // draws its own arc — the point of the overlay is the SHAPE of the sweep.
  strokeTrail = new THREE.LineSegments(
    new THREE.BufferGeometry(),
    new THREE.LineBasicMaterial({ vertexColors: true, transparent: true,
                                  opacity: 0.9, depthTest: false }));
  strokeTrail.renderOrder = 1000;
  strokeTrail.visible = false;
  scene.add(strokeTrail);

  // Resize handles: 6 spheres on each face of the active model's bounding box.
  const FACE_DEFS = [
    { axis: 0, sign: +1 }, { axis: 0, sign: -1 },
    { axis: 1, sign: +1 }, { axis: 1, sign: -1 },
    { axis: 2, sign: +1 }, { axis: 2, sign: -1 },
  ];
  resizeHandles = FACE_DEFS.map(f => {
    const m = new THREE.Mesh(
      new THREE.SphereGeometry(0.55, 12, 10),
      new THREE.MeshBasicMaterial({
        color: 0x00cccc, depthTest: false, transparent: true, opacity: 0.9,
      }));
    m.renderOrder = 997;
    m.visible = false;
    m.userData = f;
    scene.add(m);
    return m;
  });

  // Mirror plane (translucent quad, both sides).
  mirrorPlane = new THREE.Mesh(
    new THREE.PlaneGeometry(1, 1),
    new THREE.MeshBasicMaterial({
      color: 0xffb454, transparent: true, opacity: 0.10,
      side: THREE.DoubleSide, depthWrite: false,
    }));
  mirrorPlane.visible = false;
  scene.add(mirrorPlane);

  // Micro-brick context: when the model is exactly 2/4/8 cubed it represents
  // ONE world cell subdivided, and authors need to see that boundary or they
  // model at the wrong scale. Drawn as a bright wireframe unit cell.
  microGhost = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
    new THREE.LineBasicMaterial({ color: 0x7cf03a }));
  microGhost.visible = false;
  scene.add(microGhost);

  // Joint anchor gizmo: a sphere at the anchor plus a rotation arc around the
  // limb's axis. Bad anchors are the #1 cause of wrong-looking limb animation,
  // so this renders on top of everything (depthTest off).
  gizmo = new THREE.Group();
  gizmo.visible = false;
  gizmoBall = new THREE.Mesh(
    new THREE.SphereGeometry(0.45, 16, 12),
    new THREE.MeshBasicMaterial({ color: 0xffb454, depthTest: false, transparent: true, opacity: 0.95 }));
  gizmoBall.renderOrder = 999;
  gizmo.add(gizmoBall);
  gizmoArc = new THREE.Line(
    new THREE.BufferGeometry(),
    new THREE.LineBasicMaterial({ color: 0xffd08a, depthTest: false, transparent: true, opacity: 0.9 }));
  gizmoArc.renderOrder = 999;
  gizmo.add(gizmoArc);
  gizmoAxis = new THREE.Line(
    new THREE.BufferGeometry(),
    new THREE.LineBasicMaterial({ color: 0x6ea8fe, depthTest: false, transparent: true, opacity: 0.9 }));
  gizmoAxis.renderOrder = 999;
  gizmo.add(gizmoAxis);
  scene.add(gizmo);

  buildRotRings();
  buildSocketAxes();

  gridHelper = new THREE.GridHelper(1, 1, 0x39445a, 0x232c3b);
  scene.add(gridHelper);

  axes = new THREE.AxesHelper(1);
  scene.add(axes);

  frameCamera();
}

// Micro context, two flavours:
// - a lone 2/4/8-cubed model is a micro BRICK (one world cell subdivided,
//   for a material's "micro" block) — outline that cell;
// - a document whose sidecar carries "skinScale" 2/4/8 (or the older "scale")
//   is a micro-unit MOB — every `skinScale` editor voxels are one world cell,
//   so outline one world cell at the active model's corner as a reference.
function updateMicroGhost() {
  if (!microGhost || !doc) return;
  const d = activeDef()?.dim;
  const brick = d && d.x === d.y && d.y === d.z && [2, 4, 8].includes(d.x) ? d.x : 0;
  const scl = +(sidecar?.skinScale ?? sidecar?.scale) || 1;
  const sub = brick || (scl > 1 ? scl : 0);
  microGhost.visible = !!sub;
  microSubdiv = brick;
  if (!sub) return;
  const o = activeDef().offset;
  microGhost.scale.setScalar(sub);
  microGhost.position.set(o.x + sub / 2, o.y + sub / 2, o.z + sub / 2);
}

/** rig.js pokes this when the sidecar's scale changes. */
export function refreshMicroGhost() { updateMicroGhost(); updateStatus(); }

function updateResizeHandles() {
  if (!resizeHandles.length || !doc) return;
  const m = activeDef();
  if (!m) { resizeHandles.forEach(h => { h.visible = false; }); return; }
  const o = m.offset, d = m.dim;
  const cx = o.x + d.x / 2, cy = o.y + d.y / 2, cz = o.z + d.z / 2;
  const pos = [
    [o.x + d.x, cy, cz],  [o.x,       cy, cz],       // +X, -X
    [cx, o.y + d.y, cz],  [cx,       o.y, cz],        // +Y, -Y
    [cx, cy, o.z + d.z],  [cx,       cy, o.z],         // +Z, -Z
  ];
  for (let i = 0; i < 6; i++) {
    const h = resizeHandles[i];
    h.position.set(pos[i][0], pos[i][1], pos[i][2]);
    h.visible = true;
  }
}

function resizeHandleHitTest(px, py) {
  if (!resizeHandles.length || !resizeHandles[0].visible) return -1;
  const r = canvas.getBoundingClientRect();
  let best = -1, bestD = 22;
  for (let i = 0; i < resizeHandles.length; i++) {
    _v3.copy(resizeHandles[i].position).project(camera);
    const sx = r.left + ((_v3.x + 1) / 2) * r.width;
    const sy = r.top + ((1 - _v3.y) / 2) * r.height;
    const dd = Math.hypot(px - sx, py - sy);
    if (dd < bestD) { bestD = dd; best = i; }
  }
  return best;
}

// Re-fit helpers and camera to the current grid dimensions.
function frameCamera() {
  const d = (doc && doc.size) || grid.dim;
  const c = new THREE.Vector3(d.x / 2, d.y / 2, d.z / 2);
  const r = Math.max(d.x, d.y, d.z);

  boundsBox.scale.set(d.x, d.y, d.z);
  boundsBox.position.copy(c);
  updateMicroGhost();
  updateResizeHandles();

  const span = Math.max(d.x, d.z) * 2;
  gridHelper.scale.set(span, 1, span);
  gridHelper.position.set(d.x / 2, 0, d.z / 2);

  axes.scale.setScalar(Math.max(3, r * 0.35));
  // The mirror plane's own transform is owned by updateMirrorPlane(), which
  // has to pick an orientation per axis; setting it here too would just drift.

  camera.position.set(c.x + r * 1.5, c.y + r * 1.15, c.z + r * 1.75);
  camera.far = Math.max(2000, r * 40);
  camera.updateProjectionMatrix();
  controls.target.copy(c);
  controls.update();
}

function updateBoundsAndGrid() {
  const d = (doc && doc.size) || grid.dim;
  const c = new THREE.Vector3(d.x / 2, d.y / 2, d.z / 2);
  const r = Math.max(d.x, d.y, d.z);
  boundsBox.scale.set(d.x, d.y, d.z);
  boundsBox.position.copy(c);
  updateMicroGhost();
  const span = Math.max(d.x, d.z) * 2;
  gridHelper.scale.set(span, 1, span);
  gridHelper.position.set(d.x / 2, 0, d.z / 2);
  axes.scale.setScalar(Math.max(3, r * 0.35));
  camera.far = Math.max(2000, r * 40);
  camera.updateProjectionMatrix();
}

function updateMirrorPlane() {
  const on = mirror.x || mirror.y || mirror.z;
  mirrorPlane.visible = on;
  if (!on) return;
  const d = editDim();
  // Only one plane is drawn; X wins when several are on, which matches the
  // UI (X is the only toggle exposed in v1).
  if (mirror.x) {
    mirrorPlane.position.set(d.x / 2, d.y / 2, d.z / 2);
    mirrorPlane.rotation.set(0, Math.PI / 2, 0);
    mirrorPlane.scale.set(Math.max(d.z, d.y) * 1.3, Math.max(d.z, d.y) * 1.3, 1);
  } else if (mirror.y) {
    mirrorPlane.position.set(d.x / 2, d.y / 2, d.z / 2);
    mirrorPlane.rotation.set(Math.PI / 2, 0, 0);
    mirrorPlane.scale.set(Math.max(d.x, d.z) * 1.3, Math.max(d.x, d.z) * 1.3, 1);
  } else {
    mirrorPlane.position.set(d.x / 2, d.y / 2, d.z / 2);
    mirrorPlane.rotation.set(0, 0, 0);
    mirrorPlane.scale.set(Math.max(d.x, d.y) * 1.3, Math.max(d.x, d.y) * 1.3, 1);
  }
}

function resize() {
  if (!renderer || !host) return;
  const w = Math.max(1, host.clientWidth);
  const h = Math.max(1, host.clientHeight);
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
}

let lastFrameMs = 0;

function animate(nowMs) {
  if (!renderer) return;
  requestAnimationFrame(animate);
  const dt = lastFrameMs ? Math.min(0.1, (nowMs - lastFrameMs) / 1000) : 0;
  lastFrameMs = nowMs;
  // Playback and gait preview advance here and set needsRebuild themselves
  // when the pose actually changed, so a static model still costs nothing.
  hooks.tick?.(dt);
  // Only rebuild when something actually changed — a 64³ rebuild every frame
  // would be pointless work on a model that is not being edited.
  if (needsRebuild) { rebuildInstances(); needsRebuild = false; }
  controls.update();
  renderer.render(scene, camera);
}

/* ==========================================================================
   5. instance rebuild

   Full rebuild on every edit. At 64³ the worst case is 262k instances and the
   loop is a few milliseconds; a partial-update scheme would be faster and much
   easier to get subtly wrong, and edits are human-paced.
   ========================================================================== */

const _m4 = new THREE.Matrix4();
const _col = new THREE.Color();
const _v3 = new THREE.Vector3();

// Dimming factor for models that are not the active one, and for parts not in
// the current part selection. Keeps context visible without competing with the
// volume you are actually editing.
const DIM_INACTIVE = 0.28;
const DIM_UNSELECTED = 0.16;

let cappedWarned = false;

/* ---- per-model visibility (hide / solo) --------------------------------
   Keyed by model NAME, not index: models get added, removed and reordered,
   and an index-keyed set would silently start hiding the wrong limb. Purely
   a view state — never saved, and it does not touch the grids.

   Hidden models are also unpickable, so painting cannot land in a limb you
   cannot see. Solo is the inverse selection and wins over hide. */
let hiddenModels = new Set();
let soloModels = new Set();

/** True when this model should be drawn and picked. */
function modelVisible(mi) {
  const m = doc?.models[mi];
  if (!m) return false;
  if (soloModels.size) return soloModels.has(m.name);
  return !hiddenModels.has(m.name);
}

export const isModelHidden = name => hiddenModels.has(name);
export const isModelSolo = name => soloModels.has(name);
export const anySolo = () => soloModels.size > 0;

/**
 * Keep the name-keyed visibility sets in step with the models they name.
 * Keying by name survives reorders (the reason it is not by index), but it
 * means a rename orphans the entry and a delete leaks it: the stale name sits
 * in the set until some later model happens to be called that, at which point
 * it comes back hidden for no visible reason. `to` null = the model is gone.
 */
function renameVisibility(from, to) {
  for (const s of [hiddenModels, soloModels])
    if (s.delete(from) && to) s.add(to);
}

export function toggleModelHidden(name) {
  if (hiddenModels.has(name)) hiddenModels.delete(name);
  else {
    hiddenModels.add(name); soloModels.delete(name);
    warnIfEditingHidden(name);
  }
  needsRebuild = true;
  hooks.onModelsChanged?.();
  updateStatus();
}

/**
 * Outside whole mode every brush writes into the ACTIVE model by definition —
 * cellGet/ownerOf short-circuit to it and never consult visibility, because
 * there is no other model to pick. So hiding the active model there leaves it
 * invisible but still fully paintable: you paint into empty space and the
 * ghost floats over nothing. Whole mode has no such gap (both loops skip
 * hidden models), so this is only reachable with W off.
 *
 * Say it rather than silently blocking the edit — refusing to paint with no
 * explanation is the worse failure, and hiding the thing you are working on is
 * legitimate right before switching models.
 */
function warnIfEditingHidden(name) {
  if (wholeMode || !doc?.models[activeModel]) return;
  if (doc.models[activeModel].name !== name) return;
  hooks.toast(`"${name}" is the model you are editing — hidden, but brushes ` +
    'still paint into it. Turn on Whole [W] or pick another model.', true);
}

/**
 * Solo `name`. EXCLUSIVE and SELF-CANCELLING by default: soloing replaces the
 * whole solo set, and soloing something already soloed clears it, so one click
 * on a lit S always brings the whole body back.
 *
 * It used to be purely additive, which is the version that reads as broken:
 * solo an arm, solo a hand to compare, then click either one off and you are
 * still looking at a single limb with nothing on screen explaining why. The
 * number of clicks needed to undo a solo was the number you had accumulated,
 * and the only control that actually restored the body was "show all" in the
 * panel header — which is not where you soloed from and only appears once
 * something is already hidden.
 *
 * Additive is still reachable (`add`, wired to shift-click) because comparing
 * two limbs side by side is the reason multi-solo existed. Clearing is still
 * all-or-nothing there: shift-clicking the last soloed limb off empties the set
 * the same way, since "no limb is soloed" and "every limb is soloed" mean the
 * same picture.
 */
export function toggleModelSolo(name, add = false) {
  const was = soloModels.has(name);
  if (add) {
    if (was) soloModels.delete(name); else soloModels.add(name);
  } else {
    // Replace, don't accumulate. A second click on the same limb lands here
    // with `was` true and leaves the set empty — the whole body is back.
    soloModels = new Set(was ? [] : [name]);
  }
  if (!was) hiddenModels.delete(name);
  // Solo hides everything NOT soloed, so it can bury the active model just as
  // hide can — same gap, same warning (see warnIfEditingHidden).
  const act = doc?.models[activeModel];
  if (act && !modelVisible(activeModel)) warnIfEditingHidden(act.name);
  needsRebuild = true;
  hooks.onModelsChanged?.();
  updateStatus();
}

/** Clear both sets — the "show everything again" escape hatch. */
export function showAllModels() {
  if (!hiddenModels.size && !soloModels.size) return false;
  hiddenModels = new Set();
  soloModels = new Set();
  needsRebuild = true;
  hooks.onModelsChanged?.();
  updateStatus();
  return true;
}

function rebuildInstances() {
  if (!cubes || !doc) return;
  let n = 0;
  const cap = cubes.instanceMatrix.count;

  // Parts highlighting: when a limb is selected in the rig panel, everything
  // else drops right back so the limb reads clearly.
  const sel = hooks.selectedPart?.() || null;
  const selMulti = hooks.selectedParts?.() || null;

  // One model's grid appended at an arbitrary offset/brightness/transform.
  // Shared by the normal editing view and the composed preview below.
  const drawModel = (m, off, bright, xf) => {
    const d = m.dim, data = m.grid.data, cols = m.grid.color;
    for (let z = 0; z < d.z; z++) {
      for (let y = 0; y < d.y; y++) {
        const row = y * d.x + z * d.x * d.y;
        for (let x = 0; x < d.x; x++) {
          const v = data[row + x];
          if (!v || n >= cap) continue;
          if (xf) {
            // Rotate about the joint anchor, then translate — the same
            // composition the engine applies (see gait preview in rig.js).
            _v3.set(x + 0.5 + off.x, y + 0.5 + off.y, z + 0.5 + off.z);
            _v3.sub(xf.pivot).applyQuaternion(xf.quat).add(xf.pivot).add(xf.pos);
            _m4.makeTranslation(_v3.x, _v3.y, _v3.z);
          } else {
            _m4.makeTranslation(x + 0.5 + off.x, y + 0.5 + off.y, z + 0.5 + off.z);
          }
          cubes.setMatrixAt(n, _m4);
          _col.set(shownColor(v, cols ? cols[row + x] : 0));
          if (bright < 1) _col.multiplyScalar(bright);
          cubes.setColorAt(n, _col);
          n++;
        }
      }
    }
  };

  // rig.js can take over what the viewport shows:
  //   { entries: [{model, offset?, xfModel?}] } — flipbook playback draws the
  //     COMPOSED frame (a rig with one part's model swapped, or a plain
  //     flipbook file showing only the current frame), full brightness.
  //   { allBright: true } — gait / clip preview: the whole rig at full
  //     brightness so the motion reads, still one entry per model.
  //   null — normal editing view: active model bright, the rest dimmed.
  const plan = hooks.viewPlan?.() || null;

  if (plan && plan.entries) {
    for (const e of plan.entries) {
      const m = doc.models[e.model];
      if (!m) continue;
      // Hide/solo is a deliberate view state, so it survives into the preview
      // too — otherwise pressing K brings every hidden limb back while the
      // status bar still says SOLO and the row buttons stay lit.
      if (!modelVisible(e.model)) continue;
      // xfModel: the model whose animation transform this entry follows — a
      // swapped-in flipbook frame moves with the PART it replaces.
      const xf = hooks.modelTransform?.(e.xfModel ?? e.model) || null;
      drawModel(m, e.offset || m.offset, 1, xf);
    }
  } else {
    for (let mi = 0; mi < doc.models.length; mi++) {
      const m = doc.models[mi];
      if (!modelVisible(mi)) continue;
      // Gait preview supplies a per-model transform; without it models sit at
      // their static prefab offsets.
      const xf = hooks.modelTransform?.(mi) || null;
      const isActive = mi === activeModel;
      // Whole mode edits everything, so everything reads at full strength.
      let dim = (isActive || wholeMode || (plan && plan.allBright)) ? 1 : DIM_INACTIVE;
      if (selMulti && selMulti.size) dim = selMulti.has(m.name) ? 1 : DIM_UNSELECTED;
      else if (sel) dim = (m.name === sel) ? 1 : DIM_UNSELECTED;
      drawModel(m, m.offset, dim, xf);
    }
  }

  // Onion skin ghosts are appended as extra instances of the same mesh, so
  // they cost no extra draw call. rig.js decides which frames and what tint.
  const liveCount = n;
  n = hooks.appendOnionInstances?.(cubes, n, cap, _m4, _col) ?? n;
  // Same mechanism for geometry that is NOT in this document at all: the held
  // item preview draws an item's own .vox hanging off a socket. It cannot go
  // through the loop above, which walks doc.models — the item is a separate
  // file with its own origin, which is the entire point of the item/rig split.
  n = hooks.appendExtraInstances?.(cubes, n, cap, _m4, _col) ?? n;

  // Hitting the cap silently drops voxels, which looks like data loss even
  // though the document is intact. Say so once per occurrence rather than
  // every frame.
  if (n >= cap && !cappedWarned) {
    cappedWarned = true;
    hooks.toast(`view capped at ${cap} cubes — some voxels are hidden ` +
      '(turn off onion skin or reduce model count)', true);
  } else if (n < cap && liveCount < cap) {
    cappedWarned = false;
  }

  cubes.count = n;
  cubes.instanceMatrix.needsUpdate = true;
  if (cubes.instanceColor) cubes.instanceColor.needsUpdate = true;
  updateStatus();
}

/* ==========================================================================
   6. DDA picking — Amanatides & Woo

   three is used only to unproject the mouse into a world ray. The traversal
   itself is hand-written against grid.data so it returns exactly what an
   editor needs: the first solid cell AND the face it was entered through.

   The ray is first advanced to the grid's bounding box (a slab test), because
   the camera is essentially always outside the model and stepping from the
   camera would waste thousands of iterations on empty space.
   ========================================================================== */

const _ndc = new THREE.Vector2();
const _rayO = new THREE.Vector3();
const _rayD = new THREE.Vector3();

function mouseRay(px, py) {
  const r = canvas.getBoundingClientRect();
  _ndc.set(((px - r.left) / r.width) * 2 - 1, -((py - r.top) / r.height) * 2 + 1);
  _rayO.copy(camera.position);
  _rayD.set(_ndc.x, _ndc.y, 0.5).unproject(camera).sub(_rayO).normalize();
  // Everything downstream (the DDA, brushes, undo) works in EDIT-SPACE
  // coordinates: active-model-local normally, prefab space in whole mode
  // (where scene space IS edit space and no shift is needed). Shifting the
  // ray origin once here beats sprinkling offset arithmetic through the
  // traversal.
  const off = wholeMode ? null : activeDef()?.offset;
  if (off) _rayO.sub(_v3.set(off.x, off.y, off.z));
  return { o: _rayO, d: _rayD };
}

// Edit-space cell -> prefab/world position, for ghosts and gizmos.
function toWorld(x, y, z) {
  const o = (wholeMode ? null : activeDef()?.offset) || { x: 0, y: 0, z: 0 };
  return { x: x + o.x, y: y + o.y, z: z + o.z };
}

// Slab test against the edit box; returns entry/exit t or null.
function boxEnter(o, d) {
  const dim = editDim();
  let t0 = 0, t1 = Infinity;
  const oc = [o.x, o.y, o.z], dc = [d.x, d.y, d.z], hi = [dim.x, dim.y, dim.z];
  for (let a = 0; a < 3; a++) {
    if (Math.abs(dc[a]) < 1e-9) {
      if (oc[a] < 0 || oc[a] > hi[a]) return null;
      continue;
    }
    const inv = 1 / dc[a];
    let a0 = (0 - oc[a]) * inv, a1 = (hi[a] - oc[a]) * inv;
    if (a0 > a1) { const t = a0; a0 = a1; a1 = t; }
    if (a0 > t0) t0 = a0;
    if (a1 < t1) t1 = a1;
    if (t0 > t1) return null;
  }
  return { t0, t1 };
}

/**
 * March the grid. Returns { cell:[x,y,z], normal:[x,y,z], place:[x,y,z] } for
 * the first solid cell, or a "ground" hit on the y=0 plane inside the box
 * footprint (so an empty model still has somewhere to place the first voxel),
 * or null.
 */
function pickCell(px, py) {
  if (!grid) return null;
  const { o, d } = mouseRay(px, py);
  const hit = boxEnter(o, d);

  if (hit) {
    // Nudge inside so the entry cell is unambiguous on a face-on hit.
    const t = Math.max(hit.t0, 0) + 1e-4;
    let x = Math.floor(o.x + d.x * t);
    let y = Math.floor(o.y + d.y * t);
    let z = Math.floor(o.z + d.z * t);
    const dim = editDim();
    x = Math.max(0, Math.min(dim.x - 1, x));
    y = Math.max(0, Math.min(dim.y - 1, y));
    z = Math.max(0, Math.min(dim.z - 1, z));

    const step = [d.x >= 0 ? 1 : -1, d.y >= 0 ? 1 : -1, d.z >= 0 ? 1 : -1];
    const oc = [o.x, o.y, o.z], dc = [d.x, d.y, d.z];
    const cell = [x, y, z];
    const tDelta = [0, 0, 0], tMax = [0, 0, 0];
    for (let a = 0; a < 3; a++) {
      if (Math.abs(dc[a]) < 1e-9) { tDelta[a] = Infinity; tMax[a] = Infinity; continue; }
      tDelta[a] = Math.abs(1 / dc[a]);
      const bound = cell[a] + (step[a] > 0 ? 1 : 0);
      tMax[a] = (bound - oc[a]) / dc[a];
    }

    // The entry face: whichever slab we entered through. Derived from the
    // entry point rather than tracked, because step 0 has no previous axis.
    let normal = [0, 0, 0];
    {
      const p = [o.x + d.x * t, o.y + d.y * t, o.z + d.z * t];
      const dist = [
        Math.min(Math.abs(p[0] - 0), Math.abs(p[0] - dim.x)),
        Math.min(Math.abs(p[1] - 0), Math.abs(p[1] - dim.y)),
        Math.min(Math.abs(p[2] - 0), Math.abs(p[2] - dim.z)),
      ];
      let a = 0;
      if (dist[1] < dist[a]) a = 1;
      if (dist[2] < dist[a]) a = 2;
      normal[a] = -step[a];
    }

    // Hard cap: 3 * max dimension is the worst-case step count for a diagonal
    // ray, and a runaway loop here would hang the page.
    const cap = 3 * (dim.x + dim.y + dim.z) + 8;
    for (let i = 0; i < cap; i++) {
      if (cell[0] < 0 || cell[1] < 0 || cell[2] < 0 ||
          cell[0] >= dim.x || cell[1] >= dim.y || cell[2] >= dim.z) break;
      if (cellGet(cell[0], cell[1], cell[2])) {
        return {
          cell: [cell[0], cell[1], cell[2]],
          normal,
          place: [cell[0] + normal[0], cell[1] + normal[1], cell[2] + normal[2]],
        };
      }
      // Step along the axis whose next boundary is nearest.
      let a = 0;
      if (tMax[1] < tMax[a]) a = 1;
      if (tMax[2] < tMax[a]) a = 2;
      cell[a] += step[a];
      tMax[a] += tDelta[a];
      normal = [0, 0, 0];
      normal[a] = -step[a];
    }
  }

  // Fall back to the floor plane so an empty model is not unpaintable.
  if (Math.abs(d.y) > 1e-9) {
    const t = (0 - o.y) / d.y;
    if (t > 0) {
      const ed = editDim();
      const gx = Math.floor(o.x + d.x * t), gz = Math.floor(o.z + d.z * t);
      if (gx >= 0 && gz >= 0 && gx < ed.x && gz < ed.z) {
        return { cell: [gx, -1, gz], normal: [0, 1, 0], place: [gx, 0, gz] };
      }
    }
  }
  return null;
}

// The cell an operation targets in the current mode: attach builds outward
// from the hit face, erase/paint act on the cell itself.
const targetOf = h => (mode === 'attach' ? h.place : h.cell);

/* ==========================================================================
   6b. anchor gizmo

   The gizmo shows a limb's joint anchor in PREFAB-local engine coords — the
   exact space `anchor` uses in the sidecar (src/game/mob.cpp reads it as
   anchorRoot, then derives anchorLimb = anchor - restOffset). Dragging snaps
   to half-voxel steps because that is the granularity the engine's float
   anchors are authored at (see dummy.json: 2.5, 11.0, 0.5).
   ========================================================================== */

const GIZMO_SNAP = 0.5;

function normQuat(q) {
  const l = Math.hypot(q.x, q.y, q.z, q.w);
  return l < 1e-12 ? { x: 0, y: 0, z: 0, w: 1 }
                   : { x: q.x / l, y: q.y / l, z: q.z / l, w: q.w / l };
}

let gizmoState = null;        // { anchor:[x,y,z], axis:[x,y,z], onChange }

export function setGizmo(state) {
  gizmoState = state;
  updateGizmo();
}

function updateGizmo() {
  if (!gizmo) return;
  if (!gizmoState) { gizmo.visible = false; updateRotRings(); return; }
  const a = gizmoState.anchor;
  gizmo.visible = true;
  gizmo.position.set(a[0], a[1], a[2]);
  updateRotRings();

  // Rotation arc in the plane perpendicular to the joint axis, so the author
  // can see WHICH WAY the limb will swing.
  const ax = new THREE.Vector3(...(gizmoState.axis || [1, 0, 0]));
  if (ax.lengthSq() < 1e-6) ax.set(1, 0, 0);
  ax.normalize();
  const up = Math.abs(ax.y) > 0.9 ? new THREE.Vector3(1, 0, 0) : new THREE.Vector3(0, 1, 0);
  const u = new THREE.Vector3().crossVectors(up, ax).normalize();
  const v = new THREE.Vector3().crossVectors(ax, u).normalize();
  const R = 1.6, pts = [];
  for (let i = 0; i <= 32; i++) {
    const t = (-0.9 + (1.8 * i) / 32);       // ±~50°, the useful swing range
    pts.push(new THREE.Vector3()
      .addScaledVector(u, Math.cos(t) * R)
      .addScaledVector(v, Math.sin(t) * R));
  }
  gizmoArc.geometry.dispose();
  gizmoArc.geometry = new THREE.BufferGeometry().setFromPoints(pts);

  gizmoAxis.geometry.dispose();
  gizmoAxis.geometry = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3().addScaledVector(ax, -2.2),
    new THREE.Vector3().addScaledVector(ax, 2.2),
  ]);
}

/* ---- rotation rings (clip posing) --------------------------------------
   Three axis-aligned rings around the selected part's anchor. Dragging one
   rotates about that axis; the delta is reported as a quaternion so rig.js can
   drop it straight into a clip key. Separate from the anchor ball above: that
   one MOVES the joint, these ROTATE the limb about it. */

let rotRings = null;                 // THREE.Group of 3 rings
let rotState = null;                 // { quat, onChange } while posing

const RING_COLORS = [0xff6b6b, 0x7cf03a, 0x6ea8fe];   // X, Y, Z
const RING_AXES = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

function buildRotRings() {
  rotRings = new THREE.Group();
  rotRings.visible = false;
  RING_AXES.forEach((ax, i) => {
    const geo = new THREE.TorusGeometry(2.2, 0.06, 8, 64);
    const mtl = new THREE.MeshBasicMaterial({
      color: RING_COLORS[i], depthTest: false, transparent: true, opacity: 0.85,
    });
    const ring = new THREE.Mesh(geo, mtl);
    ring.renderOrder = 998;
    // Torus lies in its own XY plane; rotate so its NORMAL is the axis.
    if (i === 0) ring.rotation.y = Math.PI / 2;        // normal = X
    else if (i === 1) ring.rotation.x = Math.PI / 2;   // normal = Y
    ring.userData.axis = ax;
    ring.userData.ringIndex = i;
    rotRings.add(ring);
  });
  scene.add(rotRings);
}

export function setRotGizmo(state) {
  rotState = state;
  updateRotRings();
}

/* ---- socket frame axes --------------------------------------------------
   Three lines out of a socket showing the frame a held item is placed in.
   RED = the item's own +X, GREEN = +Y, BLUE = +Z, the same colour convention
   as AxesHelper and as the rotation rings above, so the two read together.

   This is a READOUT, not a handle. It exists because "rotation: [0,-90,0]" in
   a JSON file says nothing about whether the blade ends up along the forearm
   or across it, and that is precisely the mistake the numbers keep hiding. */

const SOCKET_AXIS_COLORS = [0xff6b6b, 0x7cf03a, 0x6ea8fe];   // X, Y, Z
const SOCKET_AXIS_LEN = 4.0;                                 // world voxels

function buildSocketAxes() {
  socketAxes = new THREE.Group();
  socketAxes.visible = false;
  for (let i = 0; i < 3; i++) {
    const line = new THREE.Line(
      new THREE.BufferGeometry().setFromPoints(
        [new THREE.Vector3(), new THREE.Vector3()]),
      new THREE.LineBasicMaterial({
        color: SOCKET_AXIS_COLORS[i], depthTest: false,
        transparent: true, opacity: 0.95,
      }));
    line.renderOrder = 1000;
    socketAxes.add(line);
    // A cone at the far end so +X reads as distinct from -X. Without it the
    // three lines are ambiguous about direction, which is half the question.
    const tip = new THREE.Mesh(
      new THREE.ConeGeometry(0.22, 0.7, 10),
      new THREE.MeshBasicMaterial({
        color: SOCKET_AXIS_COLORS[i], depthTest: false,
        transparent: true, opacity: 0.95,
      }));
    tip.renderOrder = 1000;
    socketAxes.add(tip);
  }
  scene.add(socketAxes);
}

/**
 * Show the socket frame at `pos`, turned by quaternion `q`.
 * Pass null to hide. rig.js calls this from its gizmo binding.
 */
export function setSocketAxes(state) {
  if (!socketAxes) return;
  if (!state) { socketAxes.visible = false; return; }
  socketAxes.visible = true;
  const q = state.quat || { x: 0, y: 0, z: 0, w: 1 };
  const _q = new THREE.Quaternion(q.x, q.y, q.z, q.w).normalize();
  socketAxes.position.set(state.pos[0], state.pos[1], state.pos[2]);
  const len = state.len || SOCKET_AXIS_LEN;
  for (let i = 0; i < 3; i++) {
    const dir = new THREE.Vector3(i === 0 ? 1 : 0, i === 1 ? 1 : 0, i === 2 ? 1 : 0)
      .applyQuaternion(_q);
    const line = socketAxes.children[i * 2];
    line.geometry.dispose();
    line.geometry = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(),
      dir.clone().multiplyScalar(len),
    ]);
    const tip = socketAxes.children[i * 2 + 1];
    tip.position.copy(dir.clone().multiplyScalar(len));
    // The cone's own axis is +Y; aim it down the line.
    tip.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir);
  }
}

/**
 * Show/hide the hilt-box wireframe. `state` is
 *   { pos:[x,y,z], size:[x,y,z], quat:{x,y,z,w} }
 * where pos is the box CENTRE in viewport coords and size is the full extent.
 * Pass null to hide.
 */
export function setHiltBox(state) {
  if (!hiltBox) return;
  if (!state) { hiltBox.visible = false; return; }
  hiltBox.visible = true;
  hiltBox.position.set(state.pos[0], state.pos[1], state.pos[2]);
  hiltBox.scale.set(state.size[0], state.size[1], state.size[2]);
  const q = state.quat || { x: 0, y: 0, z: 0, w: 1 };
  hiltBox.quaternion.set(q.x, q.y, q.z, q.w).normalize();
}

/**
 * Draw a list of coloured line segments in viewport space, or hide the overlay.
 *
 * `segs` is [{ a:[x,y,z], b:[x,y,z], color:0xRRGGBB, alpha? }]. Rebuilt whole
 * on every call: a stroke trail is at most a hundred short lines and a
 * per-frame rebuild of that is cheaper than the bookkeeping to update one in
 * place — the same trade the frame strip's thumbnails make.
 */
export function setStrokeTrail(segs) {
  if (!strokeTrail) return;
  if (!segs || !segs.length) { strokeTrail.visible = false; return; }
  const pos = new Float32Array(segs.length * 6);
  const col = new Float32Array(segs.length * 6);
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i], o = i * 6;
    pos[o] = s.a[0]; pos[o + 1] = s.a[1]; pos[o + 2] = s.a[2];
    pos[o + 3] = s.b[0]; pos[o + 4] = s.b[1]; pos[o + 5] = s.b[2];
    // Alpha rides the COLOUR, not the material: one material draws the whole
    // trail, so a per-segment fade has to be baked into rgb against the dark
    // viewport background.
    const k = s.alpha === undefined ? 1 : Math.max(0, Math.min(1, s.alpha));
    const c = s.color | 0;
    col[o] = ((c >> 16) & 255) / 255 * k;
    col[o + 1] = ((c >> 8) & 255) / 255 * k;
    col[o + 2] = (c & 255) / 255 * k;
    col[o + 3] = col[o]; col[o + 4] = col[o + 1]; col[o + 5] = col[o + 2];
  }
  strokeTrail.geometry.dispose();
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  g.setAttribute('color', new THREE.BufferAttribute(col, 3));
  strokeTrail.geometry = g;
  strokeTrail.visible = true;
}

function updateRotRings() {
  if (!rotRings) return;
  if (!rotState || !gizmoState) { rotRings.visible = false; return; }
  rotRings.visible = true;
  const a = gizmoState.anchor;
  rotRings.position.set(a[0], a[1], a[2]);
}

// Which ring (if any) is under the cursor. Projects each ring's circle to
// screen space and takes the nearest within a pixel tolerance.
function ringHitTest(px, py) {
  if (!rotRings || !rotRings.visible) return -1;
  const r = canvas.getBoundingClientRect();
  let best = -1, bestD = 12;                 // px tolerance
  for (const ring of rotRings.children) {
    const i = ring.userData.ringIndex;
    const ax = new THREE.Vector3(...RING_AXES[i]);
    const up = Math.abs(ax.y) > 0.9 ? new THREE.Vector3(1, 0, 0) : new THREE.Vector3(0, 1, 0);
    const u = new THREE.Vector3().crossVectors(up, ax).normalize();
    const v = new THREE.Vector3().crossVectors(ax, u).normalize();
    for (let k = 0; k < 48; k++) {
      const t = (k / 48) * Math.PI * 2;
      const p = new THREE.Vector3()
        .copy(rotRings.position)
        .addScaledVector(u, Math.cos(t) * 2.2)
        .addScaledVector(v, Math.sin(t) * 2.2)
        .project(camera);
      const sx = r.left + ((p.x + 1) / 2) * r.width;
      const sy = r.top + ((1 - p.y) / 2) * r.height;
      const d = Math.hypot(px - sx, py - sy);
      if (d < bestD) { bestD = d; best = i; }
    }
  }
  return best;
}

// Angle of the cursor around the ring's axis, in the ring's own plane.
function ringAngle(px, py, axisIndex) {
  const r = canvas.getBoundingClientRect();
  _ndc.set(((px - r.left) / r.width) * 2 - 1, -((py - r.top) / r.height) * 2 + 1);
  const o = camera.position.clone();
  const d = new THREE.Vector3(_ndc.x, _ndc.y, 0.5).unproject(camera).sub(o).normalize();
  const n = new THREE.Vector3(...RING_AXES[axisIndex]);
  const p0 = rotRings.position;
  const denom = n.dot(d);
  if (Math.abs(denom) < 1e-6) return null;
  const t = n.dot(p0.clone().sub(o)) / denom;
  if (t <= 0) return null;
  const hit = o.addScaledVector(d, t).sub(p0);
  const up = Math.abs(n.y) > 0.9 ? new THREE.Vector3(1, 0, 0) : new THREE.Vector3(0, 1, 0);
  const u = new THREE.Vector3().crossVectors(up, n).normalize();
  const v = new THREE.Vector3().crossVectors(n, u).normalize();
  return Math.atan2(hit.dot(v), hit.dot(u));
}

// Drag the gizmo on the plane through the anchor most nearly facing the
// camera, then snap. Returns true when the pointer event was consumed.
function gizmoHitTest(px, py) {
  if (!gizmoState || !gizmo.visible) return false;
  const r = canvas.getBoundingClientRect();
  _v3.copy(gizmo.position).project(camera);
  const sx = r.left + ((_v3.x + 1) / 2) * r.width;
  const sy = r.top + ((1 - _v3.y) / 2) * r.height;
  return Math.hypot(px - sx, py - sy) < 14;   // px radius, generous on purpose
}

/**
 * Ray/plane intersection in EDIT space for the move brush. The plane passes
 * through the grabbed cell and faces the camera, which is the same trick
 * gizmoDrag uses — it keeps the model tracking the cursor at any orbit angle
 * instead of being confined to one world axis.
 */
function movePlanePoint(px, py, h, nOverride, p0Override) {
  const r = canvas.getBoundingClientRect();
  _ndc.set(((px - r.left) / r.width) * 2 - 1, -((py - r.top) / r.height) * 2 + 1);
  const o = camera.position.clone();
  const d = new THREE.Vector3(_ndc.x, _ndc.y, 0.5).unproject(camera).sub(o).normalize();
  const n = nOverride || camera.getWorldDirection(new THREE.Vector3()).negate();
  const c = h ? toWorld(h.cell[0], h.cell[1], h.cell[2]) : null;
  const p0 = p0Override || new THREE.Vector3(c.x + 0.5, c.y + 0.5, c.z + 0.5);
  const denom = n.dot(d);
  if (Math.abs(denom) < 1e-6) return null;
  const t = n.dot(p0.clone().sub(o)) / denom;
  if (t <= 0) return null;
  return o.addScaledVector(d, t);
}

function gizmoDrag(px, py) {
  const r = canvas.getBoundingClientRect();
  _ndc.set(((px - r.left) / r.width) * 2 - 1, -((py - r.top) / r.height) * 2 + 1);
  const o = camera.position.clone();
  const d = new THREE.Vector3(_ndc.x, _ndc.y, 0.5).unproject(camera).sub(o).normalize();
  // Plane through the current anchor, facing the camera.
  const n = camera.getWorldDirection(new THREE.Vector3()).negate();
  const p0 = new THREE.Vector3(...gizmoState.anchor);
  const denom = n.dot(d);
  if (Math.abs(denom) < 1e-6) return;
  const t = n.dot(p0.clone().sub(o)) / denom;
  if (t <= 0) return;
  const hit = o.addScaledVector(d, t);
  const snap = v => Math.round(v / GIZMO_SNAP) * GIZMO_SNAP;
  gizmoState.anchor = [snap(hit.x), snap(hit.y), snap(hit.z)];
  updateGizmo();
  gizmoState.onChange?.(gizmoState.anchor);
}

/* ==========================================================================
   7. brushes
   ========================================================================== */

// --- box -----------------------------------------------------------------
// The drag plane is fixed by the first click's face normal so the box grows
// across a surface instead of following the camera into the model. The second
// endpoint is the picked cell projected back onto that plane.
function boxCells(a, b) {
  const out = [];
  const lo = [Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.min(a[2], b[2])];
  const hi = [Math.max(a[0], b[0]), Math.max(a[1], b[1]), Math.max(a[2], b[2])];
  for (let z = lo[2]; z <= hi[2]; z++)
    for (let y = lo[1]; y <= hi[1]; y++)
      for (let x = lo[0]; x <= hi[0]; x++) out.push([x, y, z]);
  return out;
}

// Project a picked cell onto the drag's working plane: the two free axes come
// from the pick, the locked axis stays at the anchor's value.
function onDragPlane(anchor, axis, cell) {
  const c = cell.slice();
  c[axis] = anchor[axis];
  return c;
}

// --- face flood ----------------------------------------------------------
// 4-connected flood across the visible face: cells sharing the seed's material
// AND exposed on the same side (the cell in front of them along the normal is
// empty). That "same exposure" test is what stops the fill from wrapping
// around a corner onto a face you cannot see.
function faceCells(seed, normal, limit = 4096) {
  const want = cellGet(seed[0], seed[1], seed[2]);
  if (!want) return [];
  const nAxis = normal[0] ? 0 : normal[1] ? 1 : 2;
  const free = [0, 1, 2].filter(a => a !== nAxis);
  const exposed = c =>
    cellGet(c[0] + normal[0], c[1] + normal[1], c[2] + normal[2]) === 0;

  const out = [], seen = new Set();
  const D = editDim();
  const key = c => c[0] + c[1] * D.x + c[2] * D.x * D.y;
  const q = [seed];
  seen.add(key(seed));
  while (q.length && out.length < limit) {
    const c = q.pop();
    if (cellGet(c[0], c[1], c[2]) !== want) continue;
    if (!exposed(c)) continue;
    out.push(c);
    for (const a of free) {
      for (const s of [-1, 1]) {
        const n = c.slice();
        n[a] += s;
        if (!inBounds(n[0], n[1], n[2])) continue;
        const k = key(n);
        if (seen.has(k)) continue;
        seen.add(k);
        q.push(n);
      }
    }
  }
  return out;
}

// A voxel with at least one empty 6-neighbour — the shading noise only makes
// sense on the skin, not buried in the interior.
function isExposed(x, y, z) {
  return !cellGet(x + 1, y, z) || !cellGet(x - 1, y, z) ||
         !cellGet(x, y + 1, z) || !cellGet(x, y - 1, z) ||
         !cellGet(x, y, z + 1) || !cellGet(x, y, z - 1);
}

// Integer offsets of a Euclidean sphere of the given brush size (1 = just the
// origin). Cached — the set is asked for on every pointermove of a drag.
const _sphereCache = new Map();
function sphereOffsets(size) {
  let s = _sphereCache.get(size);
  if (s) return s;
  const r = size - 1, r2 = r * r + 1e-6;
  s = [];
  for (let z = -r; z <= r; z++)
    for (let y = -r; y <= r; y++)
      for (let x = -r; x <= r; x++)
        if (x * x + y * y + z * z <= r2) s.push([x, y, z]);
  _sphereCache.set(size, s);
  return s;
}

// The cells a click acts on, for the current mode + brush. Filtering by mode
// matters once the brush is bigger than one cell: paint/erase/noise must only
// touch EXISTING voxels (a fat paint brush must not conjure a sphere out of
// air) and attach only fills EMPTY cells.
function brushCells(h) {
  const t = targetOf(h);
  if (brush === 'face') {
    // Face flood is defined on an existing surface, so it always seeds from
    // the hit cell; in attach mode the flood is then offset outward.
    if (h.cell[1] < 0) return [t];                 // floor-plane hit: no surface
    const cells = faceCells(h.cell, h.normal);
    if (mode !== 'attach') return cells;
    return cells.map(c => [c[0] + h.normal[0], c[1] + h.normal[1], c[2] + h.normal[2]]);
  }
  if (brush === 'noise') {
    // Noise scatters the active material over exposed surface voxels around
    // the HIT cell (never the place cell — it recolours, it doesn't build).
    if (h.cell[1] < 0) return [];
    const out = [];
    for (const [dx, dy, dz] of sphereOffsets(brushSize)) {
      const x = h.cell[0] + dx, y = h.cell[1] + dy, z = h.cell[2] + dz;
      if (!cellGet(x, y, z)) continue;
      if (!isExposed(x, y, z)) continue;
      if (Math.random() >= noiseDensity) continue;
      out.push([x, y, z]);
    }
    return out;
  }
  if (brushSize <= 1) return [t];
  const out = [];
  for (const [dx, dy, dz] of sphereOffsets(brushSize)) {
    const x = t[0] + dx, y = t[1] + dy, z = t[2] + dz;
    const filled = cellGet(x, y, z) !== 0;
    if (mode === 'attach' ? filled : !filled) continue;
    out.push([x, y, z]);
  }
  return out;
}

// Copy the colour layer along with a block of cells. Used by the clipboard
// and by any op that relocates voxels: paint belongs to the voxel, so it has
// to travel with it or a pasted limb comes back grey.
function colorAtCell(x, y, z) {
  if (!wholeMode) return gridColorGet(grid, x, y, z);
  for (let mi = 0; mi < doc.models.length; mi++) {
    if (!modelVisible(mi)) continue;
    const m = doc.models[mi];
    const lx = x - m.offset.x, ly = y - m.offset.y, lz = z - m.offset.z;
    if (gridGet(m.grid, lx, ly, lz)) return gridColorGet(m.grid, lx, ly, lz);
  }
  return 0;
}

/**
 * The material an op writes. KEEP_MAT means "whatever is already there":
 * painting COLOUR onto a voxel must not change what it is made of, and a
 * brush covers many cells of possibly different materials, so the decision
 * has to be per cell rather than one value chosen up front.
 */
function valueForMode() {
  if (mode === 'erase') return 0;
  if (mode === 'paint' && artColor !== null && artColorOnly) return KEEP_MAT;
  return activeMat;
}

/**
 * The `color` argument for applyOps under the current brush settings.
 *
 * At full alpha this is a constant index, so it costs one palette lookup for
 * the whole stroke. Below full alpha it must be a per-cell function: the
 * result is the brush colour mixed into whatever THAT voxel already shows,
 * which is the whole point of glazing translucent paint over existing work.
 *
 * Returns undefined when colour should be left alone entirely, so an attach
 * or a plain material paint does not disturb an existing paint job.
 */
function colorForBrush() {
  if (mode === 'erase') return 0;
  // No colour selected in paint mode means "paint the material's own colour",
  // i.e. strip any art colour off the cell so the material shows through.
  if (artColor === null) return mode === 'paint' ? 0 : undefined;
  if (artAlpha >= 1) {
    const idx = allocArt(artColor);
    return idx === null ? undefined : idx;
  }
  return (mat, art) => {
    const idx = allocArt(blendHex(shownColor(mat, art), artColor, artAlpha));
    return idx === null ? art : idx;
  };
}

/**
 * Art-palette index for `hex`, or null when the palette is full (the caller
 * then leaves the cell alone rather than painting an arbitrary colour). The
 * ceiling is reported once per stroke, not once per voxel.
 */
let artFullWarned = false;
function allocArt(hex) {
  const idx = artPalette.alloc(hex);
  if (idx) return idx;
  if (!artFullWarned) {
    artFullWarned = true;
    hooks.toast(`art palette full (${ART_SLOTS} colours) — pick an existing ` +
                'colour, or lower opacity blends less', true);
  }
  return null;
}

/* ==========================================================================
   8. input
   ========================================================================== */

function updateHover(ev) {
  hover = pickCell(ev.clientX, ev.clientY);
  if (!hover) { ghost.visible = false; return; }

  if (drag) {
    // Box preview: scale the ghost to span the drag rectangle.
    const b = onDragPlane(drag.anchor, drag.axis, targetOf(hover));
    drag.last = b;
    const lo = [0, 1, 2].map(a => Math.min(drag.anchor[a], b[a]));
    const hi = [0, 1, 2].map(a => Math.max(drag.anchor[a], b[a]));
    const c = toWorld((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2);
    ghost.visible = true;
    ghost.scale.set(hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1);
    ghost.position.set(c.x + 0.5, c.y + 0.5, c.z + 0.5);
    return;
  }

  const t = targetOf(hover);
  const w = toWorld(t[0], t[1], t[2]);
  ghost.visible = inBounds(t[0], t[1], t[2]);
  ghost.scale.set(1, 1, 1);
  ghost.position.set(w.x + 0.5, w.y + 0.5, w.z + 0.5);
  ghost.material.color.set(mode === 'erase' ? 0xff6b6b
    : mode === 'paint' ? 0xffb454 : 0x6ea8fe);
}

function onPointerDown(ev) {
  if (ev.button !== 0) return;               // right/middle belong to Orbit
  canvas.focus();

  // The gizmos outrank painting: they are drawn on top, so clicking one must
  // manipulate it rather than punching a voxel through the model behind it.
  // Anchor ball first (it sits at the centre, inside the rings).
  if (gizmoHitTest(ev.clientX, ev.clientY)) {
    ev.preventDefault();
    drag = { gizmo: true };
    canvas.setPointerCapture(ev.pointerId);
    return;
  }
  {
    const ring = ringHitTest(ev.clientX, ev.clientY);
    if (ring >= 0) {
      const a0 = ringAngle(ev.clientX, ev.clientY, ring);
      if (a0 !== null) {
        ev.preventDefault();
        drag = { ring, a0, startQuat: { ...rotState.quat } };
        canvas.setPointerCapture(ev.pointerId);
        return;
      }
    }
  }

  // Resize handles: grab one of the 6 face spheres to grow/shrink the model.
  {
    const ri = resizeHandleHitTest(ev.clientX, ev.clientY);
    if (ri >= 0) {
      ev.preventDefault();
      const m = activeDef();
      const f = resizeHandles[ri].userData;  // { axis, sign }
      const handlePos = resizeHandles[ri].position.clone();
      drag = {
        resize: true, face: f, handleIndex: ri,
        startDim: { ...m.dim }, startOffset: { ...m.offset },
        beforeResize: snapshotModel(activeModel),
        p0: handlePos,
        n: new THREE.Vector3(0, 0, 0).setComponent(f.axis, 1),
      };
      canvas.setPointerCapture(ev.pointerId);
      return;
    }
  }

  const h = pickCell(ev.clientX, ev.clientY);
  if (!h) return;
  ev.preventDefault();

  // Select brush: drag a box and hand it to rig.js ("Make Part" / "split to
  // model"). It never writes voxels, so it takes no undo entry.
  if (brush === 'select') {
    const t = h.cell[1] < 0 ? h.place : h.cell;
    drag = { select: true, anchor: t, last: t };
    canvas.setPointerCapture(ev.pointerId);
    return;
  }

  // Move brush: grab the model under the cursor and slide it. Picking by the
  // clicked voxel rather than using activeModel means you grab the limb you
  // are pointing at, which is the whole point of a direct-manipulation move.
  if (brush === 'move') {
    // pickCell works in EDIT space, which is the active model's box unless
    // whole mode is on; ownerOf is the one place that maps a cell back to the
    // model that actually owns it. Outside whole mode that is activeModel by
    // definition, so grabbing a specific limb means turning whole mode on.
    const own = h.cell[1] < 0 ? null : ownerOf(h.cell[0], h.cell[1], h.cell[2]);
    const mi = own ? own.mi : activeModel;
    const m = doc.models[mi];
    if (!m) return;
    // Build the set of model indices to move together. If the grabbed model
    // is part of a multi-selection, move them all; otherwise just the one.
    const selParts = hooks.selectedParts?.() || null;
    const moveIndices = [mi];
    if (selParts && selParts.size > 0 && selParts.has(m.name)) {
      for (let j = 0; j < doc.models.length; j++)
        if (j !== mi && selParts.has(doc.models[j].name)) moveIndices.push(j);
    }
    // Snapshot all moved models' positions for undo.
    const beforeMove = moveIndices.map(j => ({
      mi: j, offset: { ...doc.models[j].offset },
    }));
    let anchorsBefore = null;
    if (sidecar && Array.isArray(sidecar.limbs)) {
      anchorsBefore = sidecar.limbs
        .filter(l => Array.isArray(l.anchor) && l.anchor.length === 3)
        .map(l => [l.name, l.anchor.slice()]);
    }
    drag = {
      move: true, mi, moveIndices,
      start: { x: m.offset.x, y: m.offset.y, z: m.offset.z },
      applied: { x: 0, y: 0, z: 0 },
      beforeMove, anchorsBefore,
      // Drag on the plane through the grabbed cell facing the camera, and
      // remember where on it the grab started, so the model tracks the cursor
      // instead of jumping its centre there.
      p0: movePlanePoint(ev.clientX, ev.clientY, h),
      n: camera.getWorldDirection(new THREE.Vector3()).negate(),
    };
    if (mi !== activeModel) setActiveModel(mi);
    canvas.setPointerCapture(ev.pointerId);
    return;
  }

  // Alt-click is the eyedropper in every mode. It picks up BOTH facts about
  // the voxel: what it is made of and what colour it is wearing. An unpainted
  // voxel clears the brush colour, so alt-clicking bare material and painting
  // is how you get back to "no art colour here".
  if (ev.altKey) {
    const v = cellGet(h.cell[0], h.cell[1], h.cell[2]);
    if (v) {
      activeMat = v;
      const a = colorAtCell(h.cell[0], h.cell[1], h.cell[2]);
      setArtColor(a ? artPalette.colorAt(a) : null);
      renderPalette();
      hooks.toast('picked ' + matName(v) + (artColor ? ' · ' + artColor : ''));
    }
    return;
  }

  if (brush === 'box') {
    const t = targetOf(h);
    drag = {
      anchor: t,
      axis: h.normal[0] ? 0 : h.normal[1] ? 1 : 2,   // locked axis = face normal
      last: t,
    };
    canvas.setPointerCapture(ev.pointerId);
    return;
  }

  // Noise always paints the active material, whatever the mode says — unless
  // a colour is selected, in which case it scatters COLOUR over the surface
  // and leaves the material alone (speckling a skin, not restaining it).
  const val = brush === 'noise'
    ? (artColor !== null && artColorOnly ? KEEP_MAT : activeMat)
    : valueForMode();
  beginStroke();
  applyOps(brushCells(h), val, colorForBrush());
  endStroke();
  artFullWarned = false;
  // Voxel and noise brushes drag along a surface; face brush is a one-shot.
  if (brush === 'voxel' || brush === 'noise') {
    drag = { paint: true, seen: new Set() };
    canvas.setPointerCapture(ev.pointerId);
  }
}

function onPointerMove(ev) {
  pointer.x = ev.clientX; pointer.y = ev.clientY; pointer.inside = true;
  if (drag && drag.gizmo) { gizmoDrag(ev.clientX, ev.clientY); return; }
  if (drag && drag.ring !== undefined) {
    const a = ringAngle(ev.clientX, ev.clientY, drag.ring);
    if (a !== null && rotState) {
      // Delta about the ring axis, composed onto the pose the drag started
      // from. Composing on the LEFT keeps the rotation in the part's parent
      // frame, which is the frame clip keys are authored in.
      let d = a - drag.a0;
      const ax = RING_AXES[drag.ring];
      const half = d / 2, s = Math.sin(half);
      const dq = { x: ax[0] * s, y: ax[1] * s, z: ax[2] * s, w: Math.cos(half) };
      const q0 = drag.startQuat;
      rotState.quat = normQuat({
        x: dq.w * q0.x + dq.x * q0.w + dq.y * q0.z - dq.z * q0.y,
        y: dq.w * q0.y - dq.x * q0.z + dq.y * q0.w + dq.z * q0.x,
        z: dq.w * q0.z + dq.x * q0.y - dq.y * q0.x + dq.z * q0.w,
        w: dq.w * q0.w - dq.x * q0.x - dq.y * q0.y - dq.z * q0.z,
      });
      rotState.onChange?.(rotState.quat, false);
      needsRebuild = true;
    }
    return;
  }
  if (drag && drag.resize) {
    // Intersect with a plane that CONTAINS the drag axis and faces the camera,
    // so screen-space movement along the axis maps to world-space movement.
    const r = canvas.getBoundingClientRect();
    _ndc.set(((ev.clientX - r.left) / r.width) * 2 - 1,
             -((ev.clientY - r.top) / r.height) * 2 + 1);
    const ro = camera.position.clone();
    const rd = new THREE.Vector3(_ndc.x, _ndc.y, 0.5).unproject(camera).sub(ro).normalize();
    const camDir = camera.getWorldDirection(new THREE.Vector3());
    // Plane normal = cross(axis, camDir), then cross(axis, that) gives a normal
    // perpendicular to the axis but facing the camera. Simpler: use the camera-
    // facing plane and read only the axis component of the hit.
    const n = camDir.clone().negate();
    const denom = n.dot(rd);
    if (Math.abs(denom) < 1e-6) return;
    const t = n.dot(drag.p0.clone().sub(ro)) / denom;
    if (t <= 0) return;
    const hit = ro.addScaledVector(rd, t);
    const delta = Math.round(
      (hit.getComponent(drag.face.axis) - drag.p0.getComponent(drag.face.axis))
      * drag.face.sign) * drag.face.sign;
    if (!delta) return;
    const zero = { x: 0, y: 0, z: 0 };
    const pad = { lo: { ...zero }, hi: { ...zero } };
    const ax = ['x', 'y', 'z'][drag.face.axis];
    if (drag.face.sign > 0) pad.hi[ax] = delta;
    else pad.lo[ax] = -delta;
    const m = activeDef();
    if (!m) return;
    const newDim = m.dim[ax] + (drag.face.sign > 0 ? delta : -delta);
    if (newDim < 1) return;
    if (growModel(activeModel, pad)) {
      drag.p0.setComponent(drag.face.axis,
        drag.p0.getComponent(drag.face.axis) + delta);
      updateResizeHandles();
      updateBoundsAndGrid();
      hooks.onModelsChanged?.();
    }
    return;
  }
  if (drag && drag.move) {
    if (!drag.p0) return;
    const p = movePlanePoint(ev.clientX, ev.clientY, null, drag.n, drag.p0);
    if (!p) return;
    // Snap the CUMULATIVE delta from the grab point, not a per-event delta:
    // rounding each frame would let sub-voxel remainders accumulate and the
    // model would creep away from the cursor over a long drag.
    const want = {
      x: Math.round(p.x - drag.p0.x),
      y: Math.round(p.y - drag.p0.y),
      z: Math.round(p.z - drag.p0.z),
    };
    if (want.x === drag.applied.x && want.y === drag.applied.y &&
        want.z === drag.applied.z) return;
    // moveModel takes a relative step, so send the difference from what is
    // already applied. It also rebases, which is why `start` is not simply
    // written back here.
    const step = { x: want.x - drag.applied.x, y: want.y - drag.applied.y,
                   z: want.z - drag.applied.z };
    // Deliberately NOT onSidecarChanged: that hook means "a different sidecar
    // was loaded" and resets the rig panel's selected part, which would drop
    // the limb being dragged. moveModel already fired onModelsChanged.
    // Move all selected models together.
    if (drag.moveIndices.length > 1) {
      if (moveModels(drag.moveIndices, step)) drag.applied = want;
    } else {
      if (moveModel(drag.mi, step)) drag.applied = want;
    }
    return;
  }
  if (drag && drag.select) {
    const h = pickCell(ev.clientX, ev.clientY);
    if (h) {
      drag.last = h.cell[1] < 0 ? h.place : h.cell;
      const lo = [0, 1, 2].map(a => Math.min(drag.anchor[a], drag.last[a]));
      const hi = [0, 1, 2].map(a => Math.max(drag.anchor[a], drag.last[a]));
      const c = toWorld((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2);
      ghost.visible = true;
      ghost.material.color.set(0x7cf03a);
      ghost.scale.set(hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1);
      ghost.position.set(c.x + 0.5, c.y + 0.5, c.z + 0.5);
    }
    return;
  }
  if (drag && drag.paint) {
    // Continuous stroke: one undo entry for the whole drag. Deduped by the
    // hovered cell so a slow drag does not restamp (which would matter for
    // noise — restamping converges every surface voxel to the material).
    const h = pickCell(ev.clientX, ev.clientY);
    if (h) {
      const t = targetOf(h);
      const k = t.join(',');
      if (!drag.seen.has(k)) {
        drag.seen.add(k);
        if (!stroke) beginStroke();
        const v = brush === 'noise'
          ? (artColor !== null && artColorOnly ? KEEP_MAT : activeMat)
          : valueForMode();
        applyOps(brushCells(h), v, colorForBrush());
        needsRebuild = true;
      }
    }
  }
  updateHover(ev);
}

function onPointerUp(ev) {
  if (!drag) return;
  try { canvas.releasePointerCapture(ev.pointerId); } catch { /* not captured */ }
  if (drag.gizmo) { drag = null; commitSidecarUndo(); markDirty(); return; }
  if (drag.ring !== undefined) {
    drag = null;
    // `true` = the drag finished, which is what auto-key listens for.
    rotState?.onChange?.(rotState.quat, true);
    return;
  }
  if (drag.resize) {
    // Replace all per-increment grow entries with one coalesced entry for the
    // whole drag gesture.
    const before = drag.beforeResize;
    // Pop every grow entry that growModel pushed during this drag.
    while (undoStack.length && undoStack[undoStack.length - 1].type === 'grow')
      undoStack.pop();
    const after = snapshotModel(activeModel);
    // Only push if something actually changed.
    if (before.dim.x !== after.dim.x || before.dim.y !== after.dim.y ||
        before.dim.z !== after.dim.z) {
      undoStack.push({ type: 'grow', data: { modelIndex: activeModel, before, after } });
      redoStack.length = 0;
    }
    drag = null;
    updateResizeHandles();
    return;
  }
  if (drag.move) {
    const d = drag.applied, name = doc.models[drag.mi]?.name || 'model';
    if (d.x || d.y || d.z) {
      const afterMove = drag.moveIndices.map(j => ({
        mi: j, offset: { ...doc.models[j].offset },
      }));
      let anchorsAfter = null;
      if (sidecar && Array.isArray(sidecar.limbs)) {
        anchorsAfter = sidecar.limbs
          .filter(l => Array.isArray(l.anchor) && l.anchor.length === 3)
          .map(l => [l.name, l.anchor.slice()]);
      }
      commitSidecarUndo();
      undoStack.push({ type: 'move', data: {
        before: drag.beforeMove, after: afterMove,
        anchors: (drag.anchorsBefore || anchorsAfter) ? {
          before: drag.anchorsBefore || [], after: anchorsAfter || [],
        } : null,
      }});
      redoStack.length = 0;
      const scl = +(sidecar?.skinScale ?? sidecar?.scale) || 1;
      const count = drag.moveIndices.length;
      hooks.toast(`moved ${count > 1 ? count + ' parts' : name} by ${d.x},${d.y},${d.z}` +
        (scl > 1 ? ` (${(d.x / scl).toFixed(2)},${(d.y / scl).toFixed(2)},` +
                   `${(d.z / scl).toFixed(2)} world voxels)` : ''));
    }
    drag = null;
    return;
  }
  if (drag.select) {
    const sel = {
      lo: [0, 1, 2].map(a => Math.min(drag.anchor[a], drag.last[a])),
      hi: [0, 1, 2].map(a => Math.max(drag.anchor[a], drag.last[a])),
      model: activeModel,
    };
    drag = null;
    setSelection(sel);
    const n = (sel.hi[0] - sel.lo[0] + 1) *
              (sel.hi[1] - sel.lo[1] + 1) *
              (sel.hi[2] - sel.lo[2] + 1);
    hooks.toast(`selected ${n} cells — Ctrl+C copy · Del delete · Ctrl+Shift+F fill`);
    return;
  }
  if (drag.paint) { endStroke(); drag = null; updateHover(ev); return; }
  const b = drag.last;
  beginStroke();
  applyOps(boxCells(drag.anchor, b), valueForMode(), colorForBrush());
  endStroke();
  artFullWarned = false;
  drag = null;
  updateHover(ev);
}

// --- keyboard ------------------------------------------------------------
// Guard: never steal a key from a text field anywhere on the page. The tuner
// is full of inputs and a stray "e" while renaming a material would otherwise
// flip the editor into erase mode.
function typingInField(ev) {
  const t = ev.target;
  if (!t) return false;
  if (t.isContentEditable) return true;
  const tag = (t.tagName || '').toLowerCase();
  return tag === 'input' || tag === 'textarea' || tag === 'select';
}

function onKeyDown(ev) {
  if (!isActive() || typingInField(ev)) return;

  const ctrl = ev.ctrlKey || ev.metaKey;
  const k = ev.key.toLowerCase();

  if (ctrl && k === 'z') {
    ev.preventDefault(); ev.shiftKey ? redo() : undo(); return;
  }
  if (ctrl && k === 'y') { ev.preventDefault(); redo(); return; }
  if (ctrl && k === 'a') { ev.preventDefault(); selectAll(); return; }
  if (ctrl && k === 'c') { ev.preventDefault(); copySelection(); return; }
  if (ctrl && k === 'v') { ev.preventDefault(); pasteClipboard(); return; }
  if (ctrl && k === 'x') { ev.preventDefault(); cutSelection(); return; }
  if (ctrl && ev.shiftKey && k === 'f') { ev.preventDefault(); fillSelection(); return; }
  if (ctrl || ev.metaKey || ev.altKey) return;   // Ctrl+S stays the page's

  // Timeline / animation keys belong to rig.js; give it first refusal so the
  // two key maps stay in one place each instead of interleaved here.
  if (hooks.onKey?.(ev)) { ev.preventDefault(); return; }

  if (k === 't') { setMode('attach'); ev.preventDefault(); }
  else if (k === 'r') { setMode('erase'); ev.preventDefault(); }
  else if (k === 'e') { setMode('paint'); ev.preventDefault(); }
  else if (k === 'b') { setBrush('voxel'); ev.preventDefault(); }
  else if (k === 'g') { setBrush('box'); ev.preventDefault(); }
  else if (k === 'f') { setBrush('face'); ev.preventDefault(); }
  else if (k === 'v') { setBrush('select'); ev.preventDefault(); }
  else if (k === 'x') { setBrush('move'); ev.preventDefault(); }
  else if (k === 'n') { setBrush('noise'); ev.preventDefault(); }
  else if (k === 'w') { setWholeMode(!wholeMode); ev.preventDefault(); }
  else if (k === 'c') { toggleArtWheel(); ev.preventDefault(); }
  else if (k === 'm') { mirror.x = !mirror.x; updateMirrorPlane(); renderToolbar(); ev.preventDefault(); }
  else if (k === 'escape') { setSelection(null); ev.preventDefault(); }
  else if (k === 'delete') { deleteSelection(); ev.preventDefault(); }
  else if (k >= '1' && k <= '9') {
    const i = +k;                       // 1..9 -> material ID 1..9
    if (materials[i - 1]) { activeMat = i; renderPalette(); }
    ev.preventDefault();
  }
}

function setMode(m) {
  if (!MODES.includes(m)) return;
  mode = m; renderToolbar();
  if (pointer.inside) updateHover({ clientX: pointer.x, clientY: pointer.y });
}
function setBrush(b) {
  if (!BRUSHES.includes(b)) return;
  if (b === 'select' && wholeMode) {
    hooks.toast('select works on one model — turn off Whole [W] first', true);
    return;
  }
  // Move is the mirror of select: select needs ONE model's coordinate space,
  // move needs to see every box so ownerOf can tell which limb was grabbed.
  // Turning whole mode on here rather than refusing keeps it one click.
  if (b === 'move' && !wholeMode) {
    setWholeMode(true);
    hooks.toast('Move: whole mode on so every limb is grabbable — ' +
      'drag a limb to slide it, its anchor follows');
  }
  brush = b; drag = null; renderToolbar();
}

function setWholeMode(on) {
  wholeMode = !!on;
  drag = null;
  if (wholeMode && selection) setSelection(null);
  if (wholeMode && brush === 'select') brush = 'voxel';
  // The mirror of the select guard above. Move needs every box visible so
  // ownerOf can say which limb was grabbed; with whole mode off ownerOf
  // short-circuits to the active model, so every click would silently drag
  // THAT one instead of the limb under the cursor.
  if (!wholeMode && brush === 'move') {
    brush = 'voxel';
    hooks.toast('Move needs Whole mode — brush back to Voxel', true);
  }
  updateMirrorPlane();
  renderToolbar();
  needsRebuild = true;
  updateStatus();
  if (wholeMode)
    hooks.toast('WHOLE mode: brushes work across every model; attach stays ' +
      'inside the existing model boxes');
}

/* ==========================================================================
   9. UI

   Built with the page's own el() helper so the markup and the class names
   match the rest of the tuner exactly. The helper is passed in by attach()
   rather than duplicated here.
   ========================================================================== */

let el = null;                       // set in attach()
let ui = {};                         // cached elements

function renderToolbar() {
  if (!ui.modes) return;
  for (const b of ui.modes.children) b.classList.toggle('on', b.dataset.mode === mode);
  for (const b of ui.brushes.children) b.classList.toggle('on', b.dataset.brush === brush);
  ui.mirrorBtn.classList.toggle('on', mirror.x);
  ui.wholeBtn.classList.toggle('on', wholeMode);
  ui.modeInd.textContent = (wholeMode ? 'WHOLE·' : '') + mode.toUpperCase();
  ui.modeInd.className = 'medind ' + mode;
}

/**
 * Set the brush colour. null = "no art colour": paint then strips whatever
 * colour a voxel wears and lets its material show through again.
 */
function setArtColor(hex) {
  artColor = hex ? String(hex).toLowerCase() : null;
  if (artColor) pushRecent(artColor);
  renderPalette();
  if (ui.artHex) ui.artHex.value = artColor || '';
}

function setArtAlpha(a) {
  artAlpha = Math.max(0, Math.min(1, a));
  if (ui.alphaVal) ui.alphaVal.textContent = Math.round(artAlpha * 100) + '%';
  if (ui.alphaSlider) ui.alphaSlider.value = String(Math.round(artAlpha * 100));
}

function renderPalette() {
  if (!ui.palette) return;
  ui.palette.innerHTML = '';
  if (!materials.length) {
    ui.palette.append(el('span', { class: 'hint' },
      'no materials loaded — open materials.json (Overview tab) first; the ' +
      'palette maps material ID → colour, and painting without it writes ' +
      'IDs you cannot see'));
  }
  materials.forEach((m, i) => {
    const id = i + 1;                                   // material ID
    const sw = el('button', {
      class: 'msw-cell' + (id === activeMat ? ' on' : ''),
      title: `${id}. ${m.id}` + (id <= 9 ? '  [' + id + ']' : ''),
      onclick: () => { activeMat = id; renderPalette(); },
    });
    sw.style.background = (m.colors || [])[0] || '#888';
    ui.palette.append(sw);
  });
  renderArtPalette();
  if (ui.matLabel) {
    ui.matLabel.textContent = activeMat + '. ' + matName(activeMat);
    ui.matChip.style.background = matColor(activeMat);
  }
  // Follow the active material while the wheel is open. The title guard
  // breaks the applyWheelColor -> renderPalette -> loadWheelFrom cycle: when
  // the material has not changed, nothing re-seeds.
  if (ui.wheelPanel && ui.wheelPanel.style.display !== 'none' && materials.length) {
    const want = activeMat + '. ' + matName(activeMat);
    if (ui.wheelTitle.textContent !== want) {
      ui.wheelTitle.textContent = want;
      wheel.variant = 0;
      renderWheelSwatches();
      loadWheelFrom(wheelColors()[0]);
    }
  }
}

/* ---- art colour palette ------------------------------------------------
   The colours this DOCUMENT paints with, as opposed to the material colours
   above. Three rows, in the order you reach for them:

     in use   every colour already painted somewhere in this model
     recent   colours picked this session, most recent first
     [none]   clear the brush colour — paint the material's own colour

   Clicking a swatch selects it; the wheel below edits the SELECTED colour's
   hue/brightness. This is the one place in the editor where a colour is not
   a material: nothing here touches materials.json.                       */

function renderArtPalette() {
  if (!ui.artRow) return;
  ui.artRow.innerHTML = '';

  const swatch = (hex, title, on, onclick) => {
    const b = el('button', { class: 'msw-cell' + (on ? ' on' : ''), title, onclick });
    if (hex) b.style.background = hex;
    else {
      // "no colour" reads as a slash over the material colour beneath it.
      b.style.background =
        'linear-gradient(135deg,#2a2f3a 44%,var(--red) 44%,var(--red) 56%,#2a2f3a 56%)';
    }
    return b;
  };

  ui.artRow.append(el('span', { class: 'hint edartlbl' }, 'colour'));
  ui.artRow.append(swatch(null, 'no art colour — paint shows the material colour',
                          artColor === null, () => setArtColor(null)));

  const inUse = documentColors();
  for (const hex of inUse)
    ui.artRow.append(swatch(hex, hex + '  (in use in this model)',
                            artColor === hex, () => setArtColor(hex)));

  const fresh = recentColors.filter(c => !inUse.includes(c));
  if (fresh.length) {
    ui.artRow.append(el('span', { class: 'hint edartlbl' }, 'recent'));
    for (const hex of fresh)
      ui.artRow.append(swatch(hex, hex + '  (recent)',
                              artColor === hex, () => setArtColor(hex)));
  }

  // A brand-new colour comes from the wheel, so the + just opens it.
  ui.artRow.append(el('button', {
    class: 'msw-cell', title: 'pick a new colour',
    onclick: () => toggleArtWheel(true),
  }, '+'));

  if (ui.artChip) {
    ui.artChip.style.background = artColor || 'transparent';
    ui.artChip.style.borderStyle = artColor ? 'solid' : 'dashed';
  }
  if (ui.artCount)
    ui.artCount.textContent = `${artPalette.size}/${ART_SLOTS}`;
}

/**
 * Rebuild `artPalette` from a freshly parsed file, so the art indices sitting
 * in its grids resolve to the colours they were saved with. Indices are
 * preserved exactly (ArtPalette.fromPalette fills gaps rather than compacting)
 * — remapping them would mean rewriting every grid, and a colour landing on
 * the wrong voxels is precisely the bug this avoids.
 */
function adoptArtPalette(rgba) {
  const used = new Set();
  if (doc) for (const m of doc.models) {
    if (!m.grid.color) continue;
    for (const v of m.grid.color) if (v) used.add(v);
  }
  artPalette = rgba && used.size ? ArtPalette.fromPalette(rgba, used) : new ArtPalette();
  setArtColor(null);
  artPalette.writeInto(palette);
}

/** Every art colour actually painted somewhere in the document, in palette order. */
function documentColors() {
  const seen = new Set();
  if (doc) for (const m of doc.models) {
    if (!m.grid.color) continue;
    const data = m.grid.data, col = m.grid.color;
    for (let i = 0; i < col.length; i++) if (col[i] && data[i]) seen.add(col[i]);
  }
  return [...seen].sort((a, b) => b - a)
    .map(i => artPalette.colorAt(i)).filter(Boolean);
}

/* ---- material colour wheel -------------------------------------------
   Voxels store a MATERIAL ID, not a colour, so "picking a colour" here means
   editing the active material's `colors[]` variants — the same array the
   Materials tab edits, the engine hash-picks between per voxel, and R
   hot-reloads in-game. The wheel writes straight into the tuner's materials
   object and marks materials.json dirty via hooks.touchMaterials.       */

let wheel = { variant: 0, h: 0, s: 0, v: 1 };
let wheelDragStart = null; // {matIndex, variant, hex} snapshot before a drag

function commitWheelUndo() {
  if (!wheelDragStart) return;
  const colors = wheelColors();
  const newHex = colors ? colors[Math.min(wheelDragStart.variant, colors.length - 1)] : null;
  if (newHex && newHex !== wheelDragStart.hex) {
    commitSidecarUndo();
    undoStack.push({
      type: 'color',
      data: {
        matIndex: wheelDragStart.matIndex,
        variant: wheelDragStart.variant,
        oldHex: wheelDragStart.hex,
        newHex,
      },
    });
    redoStack.length = 0;
  }
  wheelDragStart = null;
}

function snapshotWheel() {
  const colors = wheelColors();
  if (!colors) return;
  wheelDragStart = {
    matIndex: activeMat - 1,
    variant: Math.min(wheel.variant, colors.length - 1),
    hex: colors[Math.min(wheel.variant, colors.length - 1)],
  };
}

function hsvToHex(h, s, v) {
  const f = n => {
    const k = (n + h * 6) % 6;
    const c = v - v * s * Math.max(0, Math.min(k, 4 - k, 1));
    return Math.round(c * 255).toString(16).padStart(2, '0');
  };
  return '#' + f(5) + f(3) + f(1);
}

function hexToHsv(hex) {
  const s = String(hex || '#888888').replace('#', '');
  const n = parseInt(s.length === 3
    ? s[0] + s[0] + s[1] + s[1] + s[2] + s[2] : s.slice(0, 6), 16) | 0;
  const r = ((n >> 16) & 255) / 255, g = ((n >> 8) & 255) / 255, b = (n & 255) / 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
  let h = 0;
  if (d > 1e-6) {
    if (mx === r) h = ((g - b) / d + 6) % 6;
    else if (mx === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  return { h, s: mx < 1e-6 ? 0 : d / mx, v: mx };
}

const wheelMat = () => materials[activeMat - 1] || null;

function wheelColors() {
  const m = wheelMat();
  if (!m) return null;
  if (!Array.isArray(m.colors) || !m.colors.length) m.colors = ['#888888'];
  return m.colors;
}

function drawWheel() {
  const cv = ui.wheelCanvas;
  if (!cv) return;
  const ctx = cv.getContext('2d');
  const W = cv.width, R = W / 2;
  const img = ctx.createImageData(W, W);
  for (let py = 0; py < W; py++)
    for (let px = 0; px < W; px++) {
      const dx = (px - R) / R, dy = (py - R) / R;
      const r = Math.hypot(dx, dy);
      const o = (py * W + px) * 4;
      if (r > 1) { img.data[o + 3] = 0; continue; }
      const h = (Math.atan2(dy, dx) / (2 * Math.PI) + 1) % 1;
      const hex = hsvToHex(h, Math.min(r, 1), wheel.v);
      const n = parseInt(hex.slice(1), 16);
      img.data[o] = (n >> 16) & 255;
      img.data[o + 1] = (n >> 8) & 255;
      img.data[o + 2] = n & 255;
      img.data[o + 3] = 255;
    }
  ctx.putImageData(img, 0, 0);
  // marker at the current hue/sat
  const a = wheel.h * 2 * Math.PI, rr = wheel.s * R;
  ctx.beginPath();
  ctx.arc(R + Math.cos(a) * rr, R + Math.sin(a) * rr, 5, 0, 2 * Math.PI);
  ctx.strokeStyle = wheel.v > 0.55 ? '#000' : '#fff';
  ctx.lineWidth = 2;
  ctx.stroke();
}

function applyWheelColor() {
  const colors = wheelColors();
  if (!colors) return;
  const hex = hsvToHex(wheel.h, wheel.s, wheel.v);
  colors[Math.min(wheel.variant, colors.length - 1)] = hex;
  ui.wheelHex.value = hex;
  hooks.touchMaterials?.();          // marks materials.json dirty in the tuner
  renderPalette();
  renderWheelSwatches();
  needsRebuild = true;               // the viewport tints by colors[0]
}

function loadWheelFrom(hex) {
  wheel = { ...wheel, ...hexToHsv(hex) };
  ui.wheelVal.value = String(Math.round(wheel.v * 100));
  ui.wheelHex.value = hex;
  drawWheel();
}

function renderWheelSwatches() {
  if (!ui.wheelSwatches) return;
  ui.wheelSwatches.innerHTML = '';
  const colors = wheelColors();
  if (!colors) return;
  colors.forEach((c, i) => {
    const b = el('button', {
      class: 'msw-cell' + (i === wheel.variant ? ' on' : ''),
      title: `variant ${i}` + (i === 0 ? ' (viewport shows this one)' : ''),
      onclick: () => { wheel.variant = i; loadWheelFrom(colors[i]); renderWheelSwatches(); },
    });
    b.style.background = c;
    ui.wheelSwatches.append(b);
  });
  if (colors.length < 4) {
    ui.wheelSwatches.append(el('button', {
      class: 'msw-cell', title: 'add a shade variant (engine hash-picks per voxel)',
      onclick: () => {
        colors.push(colors[colors.length - 1]);
        wheel.variant = colors.length - 1;
        hooks.touchMaterials?.();
        renderWheelSwatches();
      },
    }, '+'));
  }
}

function toggleWheel(show) {
  const on = show ?? ui.wheelPanel.style.display === 'none';
  ui.wheelPanel.style.display = on ? '' : 'none';
  if (!on) return;
  const m = wheelMat();
  if (!m) { hooks.toast('load materials.json first (Overview tab)', true); return; }
  ui.wheelTitle.textContent = activeMat + '. ' + matName(activeMat);
  wheel.variant = 0;
  renderWheelSwatches();
  loadWheelFrom(wheelColors()[0]);
}

function buildWheelPanel() {
  ui.wheelCanvas = el('canvas', { class: 'edwheelcv', width: '140', height: '140' });
  const pick = ev => {
    const r = ui.wheelCanvas.getBoundingClientRect();
    const dx = (ev.clientX - r.left - r.width / 2) / (r.width / 2);
    const dy = (ev.clientY - r.top - r.height / 2) / (r.height / 2);
    wheel.h = (Math.atan2(dy, dx) / (2 * Math.PI) + 1) % 1;
    wheel.s = Math.min(Math.hypot(dx, dy), 1);
    drawWheel();
    applyWheelColor();
  };
  ui.wheelCanvas.addEventListener('pointerdown', ev => {
    ev.preventDefault();
    snapshotWheel();
    pick(ev);
    const mv = e => pick(e);
    const up = () => { commitWheelUndo();
                       window.removeEventListener('pointermove', mv);
                       window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', mv);
    window.addEventListener('pointerup', up);
  });

  ui.wheelVal = el('input', { type: 'range', min: '0', max: '100', value: '100',
                              class: 'edslider', title: 'brightness' });
  ui.wheelVal.addEventListener('pointerdown', () => snapshotWheel());
  ui.wheelVal.addEventListener('input', () => {
    wheel.v = +ui.wheelVal.value / 100;
    drawWheel();
    applyWheelColor();
  });
  ui.wheelVal.addEventListener('change', () => commitWheelUndo());

  ui.wheelHex = el('input', { class: 'cell id edwheelhex', placeholder: '#rrggbb' });
  ui.wheelHex.addEventListener('change', () => {
    if (!/^#?[0-9a-f]{6}$/i.test(ui.wheelHex.value.trim())) return;
    snapshotWheel();
    const hex = '#' + ui.wheelHex.value.trim().replace('#', '').toLowerCase();
    loadWheelFrom(hex);
    applyWheelColor();
    commitWheelUndo();
  });

  ui.wheelSwatches = el('div', { class: 'edwheelsw' });
  ui.wheelTitle = el('b', {}, '');

  ui.wheelPanel = el('div', { class: 'edwheel' },
    el('div', { class: 'edwheelhead' }, ui.wheelTitle,
      el('span', { class: 'spacer' }),
      el('button', { class: 'icon', title: 'close', onclick: () => toggleWheel(false) }, '✕')),
    el('div', { class: 'edwheelbody' },
      ui.wheelCanvas,
      el('div', { class: 'edwheelside' },
        el('span', { class: 'hint' }, 'shade variants (engine picks per voxel)'),
        ui.wheelSwatches,
        el('span', { class: 'hint' }, 'brightness'), ui.wheelVal, ui.wheelHex,
        el('span', { class: 'hint' },
          'edits materials.json — R hot-reloads in-game; save on the Overview tab'))));
  ui.wheelPanel.style.display = 'none';
  return ui.wheelPanel;
}

/* ---- art colour wheel + opacity ---------------------------------------
   Same wheel widget as the material one, but it feeds the BRUSH rather than
   materials.json, and it carries the opacity slider — the two belong side by
   side because "which colour" and "how much of it" are one decision.     */

let artWheel = { h: 0, s: 0.7, v: 0.8 };

function drawArtWheel() {
  const cv = ui.artCanvas;
  if (!cv) return;
  const ctx = cv.getContext('2d');
  const W = cv.width, R = W / 2;
  const img = ctx.createImageData(W, W);
  for (let py = 0; py < W; py++)
    for (let px = 0; px < W; px++) {
      const dx = (px - R) / R, dy = (py - R) / R;
      const r = Math.hypot(dx, dy);
      const o = (py * W + px) * 4;
      if (r > 1) { img.data[o + 3] = 0; continue; }
      const h = (Math.atan2(dy, dx) / (2 * Math.PI) + 1) % 1;
      const n = parseInt(hsvToHex(h, Math.min(r, 1), artWheel.v).slice(1), 16);
      img.data[o] = (n >> 16) & 255;
      img.data[o + 1] = (n >> 8) & 255;
      img.data[o + 2] = n & 255;
      img.data[o + 3] = 255;
    }
  ctx.putImageData(img, 0, 0);
  const a = artWheel.h * 2 * Math.PI, rr = artWheel.s * R;
  ctx.beginPath();
  ctx.arc(R + Math.cos(a) * rr, R + Math.sin(a) * rr, 5, 0, 2 * Math.PI);
  ctx.strokeStyle = artWheel.v > 0.55 ? '#000' : '#fff';
  ctx.lineWidth = 2;
  ctx.stroke();
}

function toggleArtWheel(show) {
  const on = show ?? ui.artPanel.style.display === 'none';
  ui.artPanel.style.display = on ? '' : 'none';
  if (!on) return;
  if (artColor) artWheel = { ...artWheel, ...hexToHsv(artColor) };
  ui.artVal.value = String(Math.round(artWheel.v * 100));
  drawArtWheel();
  renderArtPreview();
}

/** Show what the brush will actually lay down over the current hover cell. */
function renderArtPreview() {
  if (!ui.artPreview) return;
  const hex = hsvToHex(artWheel.h, artWheel.s, artWheel.v);
  let under = null;
  if (hover) {
    const t = hover.cell;
    const m = cellGet(t[0], t[1], t[2]);
    if (m) under = shownColor(m, colorAtCell(t[0], t[1], t[2]));
  }
  ui.artPreview.innerHTML = '';
  const chip = (c, label) => el('span', { class: 'edartprev' },
    Object.assign(el('i', {}), { style: `background:${c}` }),
    el('span', { class: 'hint' }, label));
  if (under && artAlpha < 1) {
    ui.artPreview.append(chip(under, 'under'),
                         chip(blendHex(under, hex, artAlpha), 'result'));
  } else {
    ui.artPreview.append(chip(hex, artAlpha < 1 ? 'brush (hover a voxel)' : 'brush'));
  }
}

function buildArtPanel() {
  ui.artCanvas = el('canvas', { class: 'edwheelcv', width: '120', height: '120' });
  const pick = ev => {
    const r = ui.artCanvas.getBoundingClientRect();
    const dx = (ev.clientX - r.left - r.width / 2) / (r.width / 2);
    const dy = (ev.clientY - r.top - r.height / 2) / (r.height / 2);
    artWheel.h = (Math.atan2(dy, dx) / (2 * Math.PI) + 1) % 1;
    artWheel.s = Math.min(Math.hypot(dx, dy), 1);
    drawArtWheel();
    // Live-update the brush colour, but only push to `recent` on release —
    // a drag across the wheel would otherwise fill the recents with 40 hues.
    artColor = hsvToHex(artWheel.h, artWheel.s, artWheel.v);
    ui.artHex.value = artColor;
    renderArtPreview();
    if (ui.artChip) { ui.artChip.style.background = artColor;
                      ui.artChip.style.borderStyle = 'solid'; }
  };
  ui.artCanvas.addEventListener('pointerdown', ev => {
    ev.preventDefault();
    pick(ev);
    const mv = e => pick(e);
    const up = () => { setArtColor(artColor);
                       window.removeEventListener('pointermove', mv);
                       window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', mv);
    window.addEventListener('pointerup', up);
  });

  ui.artVal = el('input', { type: 'range', min: '0', max: '100', value: '80',
                            class: 'edslider', title: 'brightness' });
  ui.artVal.addEventListener('input', () => {
    artWheel.v = +ui.artVal.value / 100;
    drawArtWheel();
    artColor = hsvToHex(artWheel.h, artWheel.s, artWheel.v);
    ui.artHex.value = artColor;
    renderArtPreview();
  });
  ui.artVal.addEventListener('change', () => setArtColor(artColor));

  ui.artHex = el('input', { class: 'cell id edwheelhex', placeholder: '#rrggbb' });
  ui.artHex.addEventListener('change', () => {
    const t = ui.artHex.value.trim();
    if (!t) { setArtColor(null); return; }
    if (!/^#?[0-9a-f]{6}$/i.test(t)) return;
    const hex = '#' + t.replace('#', '').toLowerCase();
    artWheel = { ...artWheel, ...hexToHsv(hex) };
    ui.artVal.value = String(Math.round(artWheel.v * 100));
    drawArtWheel();
    setArtColor(hex);
    renderArtPreview();
  });

  // Opacity. 100% replaces the colour outright; below that each stroke mixes
  // toward the brush colour, so repeated passes converge on it — which is
  // exactly how glazing works with real paint.
  ui.alphaVal = el('span', { class: 'hint edslideval' }, '100%');
  ui.alphaSlider = el('input', {
    type: 'range', min: '5', max: '100', step: '5', value: '100',
    class: 'edslider',
    title: 'opacity: how much of the brush colour each stroke lays down',
  });
  ui.alphaSlider.addEventListener('input', () => {
    setArtAlpha(+ui.alphaSlider.value / 100);
    renderArtPreview();
  });

  ui.artPreview = el('div', { class: 'edartprevrow' });
  ui.artCount = el('span', { class: 'hint' }, '0/' + ART_SLOTS);

  const onlyBtn = el('button', {
    class: 'small' + (artColorOnly ? ' on' : ''),
    title: 'paint colour only, leaving each voxel\'s material alone — a ' +
      'creature stays meat everywhere while you paint its skin',
    onclick: () => {
      artColorOnly = !artColorOnly;
      onlyBtn.classList.toggle('on', artColorOnly);
    },
  }, 'colour only');

  ui.artPanel = el('div', { class: 'edwheel edart' },
    el('div', { class: 'edwheelhead' }, el('b', {}, 'brush colour'),
      el('span', { class: 'hint' }, '— per-voxel art, not a material'),
      el('span', { class: 'spacer' }), ui.artCount,
      el('button', { class: 'icon', title: 'close',
                     onclick: () => toggleArtWheel(false) }, '✕')),
    el('div', { class: 'edwheelbody' },
      ui.artCanvas,
      el('div', { class: 'edwheelside' },
        el('span', { class: 'hint' }, 'brightness'), ui.artVal,
        el('span', { class: 'hint' }, 'opacity'),
        el('span', { class: 'edsliders' }, ui.alphaSlider, ui.alphaVal),
        el('span', { class: 'edartrow2' }, ui.artHex, onlyBtn),
        ui.artPreview,
        el('span', { class: 'hint' },
          'paints into this model only — colour rides in the .vox beside the ' +
          'material, and a dismembered limb still becomes its material'))));
  ui.artPanel.style.display = 'none';
  return ui.artPanel;
}

function updateStatus() {
  if (!ui.status || !grid) return;
  let filled = 0;
  for (let i = 0; i < grid.data.length; i++) if (grid.data[i]) filled++;
  const d = grid.dim;
  const nm = activeDef()?.name || '';
  const scl = +(sidecar?.skinScale ?? sidecar?.scale) || 1;
  const ws = doc?.size || d;
  ui.status.textContent =
    `${docName}${docDirty ? ' *' : ''}  ·  ` +
    (wholeMode ? `WHOLE ${ws.x}×${ws.y}×${ws.z}` : `${nm} ${d.x}×${d.y}×${d.z}`) +
    (doc && doc.models.length > 1 ? ` (${activeModel + 1}/${doc.models.length})` : '') +
    (microSubdiv ? `  ·  micro ${microSubdiv}³` : '') +
    (scl > 1 ? `  ·  scale ${scl} → world ${Math.ceil(ws.x / scl)}×` +
               `${Math.ceil(ws.y / scl)}×${Math.ceil(ws.z / scl)}` : '') +
    `  ·  ${filled} voxel${filled === 1 ? '' : 's'}  ·  ${undoStack.length} undo` +
    // Say it out loud: a hidden limb looks exactly like deleted geometry.
    (soloModels.size ? `  ·  SOLO ${soloModels.size}` : '') +
    (hiddenModels.size ? `  ·  ${hiddenModels.size} hidden` : '');
}

/** Panel mounts for rig.js. */
export const panels = () => ({ side: ui.side, timeline: ui.timeline });

function buildUI(section) {
  // --- toolbar row 1: modes, brushes, mirror ---
  const mkBtn = (label, attrs) => el('button', { class: 'small', ...attrs }, label);

  ui.modes = el('div', { class: 'btngroup' },
    mkBtn('Attach [T]', { 'data-mode': 'attach', onclick: () => setMode('attach') }),
    mkBtn('Erase [R]', { 'data-mode': 'erase', onclick: () => setMode('erase') }),
    mkBtn('Paint [E]', { 'data-mode': 'paint', onclick: () => setMode('paint') }));

  ui.brushes = el('div', { class: 'btngroup' },
    mkBtn('Voxel [B]', { 'data-brush': 'voxel', onclick: () => setBrush('voxel') }),
    mkBtn('Box [G]', { 'data-brush': 'box', onclick: () => setBrush('box') }),
    mkBtn('Face [F]', { 'data-brush': 'face', onclick: () => setBrush('face') }),
    mkBtn('Select [V]', { 'data-brush': 'select', onclick: () => setBrush('select') }),
    mkBtn('Move [X]', {
      'data-brush': 'move', onclick: () => setBrush('move'),
      title: 'drag a whole model (limb) around — its anchor moves with it',
    }),
    mkBtn('Noise [N]', {
      'data-brush': 'noise',
      title: 'scatter the active material over exposed surface voxels — ' +
        'shading noise the engine way (per-voxel data IS the material)',
      onclick: () => setBrush('noise'),
    }));

  ui.mirrorBtn = mkBtn('Mirror X [M]', {
    onclick: () => { mirror.x = !mirror.x; updateMirrorPlane(); renderToolbar(); },
  });

  ui.wholeBtn = mkBtn('Whole [W]', {
    title: 'edit the assembled prefab as one canvas — brushes cross model ' +
      'boundaries and each write lands in the model that owns the cell',
    onclick: () => setWholeMode(!wholeMode),
  });

  // Brush size (voxel/noise) and noise density, as compact toolbar sliders.
  ui.sizeVal = el('span', { class: 'hint edslideval' }, '1');
  const sizeSlider = el('input', {
    type: 'range', min: '1', max: '6', step: '1', value: '1', class: 'edslider',
    title: 'brush size (voxel + noise brushes)',
  });
  sizeSlider.addEventListener('input', () => {
    brushSize = +sizeSlider.value;
    ui.sizeVal.textContent = sizeSlider.value;
  });
  ui.densVal = el('span', { class: 'hint edslideval' }, Math.round(noiseDensity * 100) + '%');
  const densSlider = el('input', {
    type: 'range', min: '5', max: '100', step: '5',
    value: String(Math.round(noiseDensity * 100)), class: 'edslider',
    title: 'noise density: chance each surface voxel in the brush is hit',
  });
  densSlider.addEventListener('input', () => {
    noiseDensity = +densSlider.value / 100;
    ui.densVal.textContent = densSlider.value + '%';
  });
  ui.sliders = el('span', { class: 'edsliders' },
    el('span', { class: 'hint' }, 'size'), sizeSlider, ui.sizeVal,
    el('span', { class: 'hint' }, 'noise'), densSlider, ui.densVal);

  ui.modeInd = el('span', { class: 'medind attach' }, 'ATTACH');

  ui.help = buildHelpPanel();
  const bar1 = el('div', { class: 'toolbar edbar' },
    ui.modeInd, ui.modes, ui.brushes, ui.mirrorBtn, ui.wholeBtn, ui.sliders,
    el('span', { class: 'spacer' }),
    mkBtn('Undo', { onclick: undo }), mkBtn('Redo', { onclick: redo }),
    mkBtn('?', {
      title: 'help / cheat sheet (full guide: docs/EDITOR_GUIDE.md)',
      onclick: () => {
        ui.help.style.display = ui.help.style.display === 'none' ? '' : 'none';
      },
    }));

  // --- toolbar row 2: file ops ---
  ui.fileSel = el('select', { class: 'sortsel', onchange: () => openPath(ui.fileSel.value) });
  const presets = el('select', {
    class: 'sortsel',
    onchange: (e) => { applyPreset(e.target.value); e.target.value = ''; },
  },
    el('option', { value: '' }, 'new…'),
    el('option', { value: '8' }, '8³'),
    el('option', { value: '16' }, '16³'),
    el('option', { value: '32' }, '32³'),
    el('option', { value: '64' }, '64³'),
    el('option', { value: 'micro2' }, 'micro brick 2³'),
    el('option', { value: 'micro4' }, 'micro brick 4³'),
    el('option', { value: 'micro8' }, 'micro brick 8³'),
    el('option', { value: 'custom' }, 'custom…'));

  ui.matChip = el('span', {
    class: 'matchip', title: 'colour wheel: edit this material\'s colours',
    onclick: () => toggleWheel(),
  });
  ui.matLabel = el('span', {
    class: 'hint', style: 'cursor:pointer',
    title: 'colour wheel: edit this material\'s colours',
    onclick: () => toggleWheel(),
  }, '');
  ui.status = el('span', { class: 'hint edstatus' }, '');

  // Brush colour chip, beside the material chip: the two facts a brush writes.
  ui.artChip = el('span', {
    class: 'matchip edartchip',
    title: 'brush colour + opacity [C]',
    onclick: () => toggleArtWheel(),
  });
  const artLabel = el('span', {
    class: 'hint', style: 'cursor:pointer', title: 'brush colour + opacity [C]',
    onclick: () => toggleArtWheel(),
  }, 'colour');

  const bar2 = el('div', { class: 'toolbar edbar' },
    el('span', { class: 'hint' }, 'open'), ui.fileSel,
    mkBtn('↻', { title: 'refresh list', onclick: refreshFileList }),
    presets,
    mkBtn('Save', { class: 'small primary', onclick: () => save(false) }),
    mkBtn('Save as…', { class: 'small', onclick: () => save(true) }),
    el('span', { class: 'spacer' }),
    ui.matChip, ui.matLabel,
    el('span', { class: 'hsep' }), ui.artChip, artLabel,
    el('span', { class: 'hsep' }), ui.status);

  // --- palette: materials on top, art colours under them ---
  ui.palette = el('div', { class: 'edpalette' });
  ui.artRow = el('div', { class: 'edpalette edartpal' });

  // --- viewport, with the rig side panel beside it ---
  canvas = el('canvas', { class: 'edcanvas', tabindex: '0' });
  host = el('div', { class: 'edviewport' }, canvas);
  ui.side = el('div', { class: 'edside' });          // rig.js fills this
  ui.grip = buildSideGrip();                         // drag its left edge to resize
  ui.timeline = el('div', { class: 'edtimeline' });  // ...and this

  ui.note = el('div', { class: 'hint edhelp' },
    'left-drag paints · X moves · right-drag orbits · mid-drag pans · ' +
    'Ctrl+A select all · Ctrl+C/V/X copy/paste/cut · Del delete · ' +
    'Ctrl+Shift+F fill · Ctrl+Z/Y undo · ? cheat sheet');

  section.append(bar1, bar2, ui.help, buildWheelPanel(), buildArtPanel(),
    ui.palette, ui.artRow,
    el('div', { class: 'edmain' }, host, ui.grip, ui.side),
    ui.timeline, ui.note);
}

/* The rig panel's width is a working preference, not model data: a rigging
   pass wants it wide enough to read limb names, a painting pass wants the
   viewport back. So it drags, and the width persists per browser.

   Pointer events (not mouse) with setPointerCapture, so the drag survives the
   pointer crossing the WebGL canvas — which swallows mousemove for painting —
   and so a pen/touch drag works the same way. */
const kSideWKey = 'sandvox.editor.sideW';
const kSideWMin = 200;

function clampSideW(px, mainW) {
  // Leave the viewport at least its CSS min-width, so dragging cannot collapse
  // the thing being edited. mainW is unknown before first layout; fall back to
  // a generous cap rather than clamping to nothing.
  const max = Math.max(kSideWMin, (mainW || window.innerWidth) - 240);
  return Math.round(Math.max(kSideWMin, Math.min(max, px)));
}

function setSideW(px, mainW) {
  if (!ui.side) return;
  ui.side.style.width = clampSideW(px, mainW) + 'px';
  // The canvas is CSS-sized (width:100%), so the drawing buffer only follows
  // if we tell it to — window 'resize' does not fire for a flex reflow.
  resize();
}

function buildSideGrip() {
  const grip = el('div', {
    class: 'edgrip',
    title: 'drag to resize the panel · double-click to reset',
  });

  grip.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    e.preventDefault();
    const main = grip.parentElement;
    const startX = e.clientX;
    const startW = ui.side.getBoundingClientRect().width;
    const mainW = main ? main.getBoundingClientRect().width : 0;
    grip.setPointerCapture(e.pointerId);
    grip.classList.add('dragging');
    document.body.classList.add('edresizing');

    // Dragging LEFT widens the panel, hence the negated delta.
    const move = ev => setSideW(startW - (ev.clientX - startX), mainW);
    const up = ev => {
      grip.releasePointerCapture?.(ev.pointerId);
      grip.classList.remove('dragging');
      document.body.classList.remove('edresizing');
      grip.removeEventListener('pointermove', move);
      grip.removeEventListener('pointerup', up);
      grip.removeEventListener('pointercancel', up);
      try { localStorage.setItem(kSideWKey, String(Math.round(
        ui.side.getBoundingClientRect().width))); } catch (_) { /* private mode */ }
    };
    grip.addEventListener('pointermove', move);
    grip.addEventListener('pointerup', up);
    grip.addEventListener('pointercancel', up);
  });

  // Reset to the CSS default in tuner.html (.edside width), not to some other
  // number: "reset" that lands somewhere the panel has never been is not one.
  grip.addEventListener('dblclick', () => {
    setSideW(420, grip.parentElement?.getBoundingClientRect().width || 0);
    try { localStorage.removeItem(kSideWKey); } catch (_) { /* private mode */ }
  });

  return grip;
}

// Re-apply the stored width once the tab is actually laid out. Called from
// init(); before the Models tab is shown every rect is 0 and clamping the
// stored value against a 0-wide parent would throw the preference away.
function restoreSideW() {
  let saved = 0;
  try { saved = +localStorage.getItem(kSideWKey) || 0; } catch (_) { /* ignore */ }
  if (!saved || !ui.side) return;
  const mainW = ui.side.parentElement?.getBoundingClientRect().width || 0;
  if (mainW <= 0) return;            // not visible yet; leave the CSS default
  setSideW(saved, mainW);
}

// The in-app cheat sheet behind the [?] button. The full walkthrough lives in
// docs/EDITOR_GUIDE.md; this is the version you glance at mid-edit.
function buildHelpPanel() {
  const row = (k, txt) => el('div', { class: 'edhelprow' },
    el('b', {}, k), el('span', {}, txt));
  const p = el('div', { class: 'edhelppanel' },
    row('sculpt', 'T attach · R erase · E paint — left-drag applies. ' +
      'Brushes: B single voxel · G box (drag along the clicked face) · ' +
      'F face flood · V select · N noise. The size slider makes voxel/noise ' +
      'spherical; noise scatters the active material over the surface at the ' +
      'density slider\'s chance. M mirrors across X. W = WHOLE mode: edit ' +
      'every model as one canvas (paint/erase/noise cross limb boundaries). ' +
      'Alt-click = eyedropper, 1-9 = material, Ctrl+Z / Ctrl+Y = undo / ' +
      'redo, Ctrl+S = save. Ctrl+A select all · Ctrl+C copy · Ctrl+V paste · ' +
      'Ctrl+X cut · Del delete selection · Ctrl+Shift+F fill selection.'),
    row('colour', 'Click the material chip for the colour wheel: it edits the ' +
      'ACTIVE MATERIAL\'s shade variants (voxels store a material ID, not a ' +
      'colour — the engine hash-picks a variant per voxel). Changes land in ' +
      'materials.json; R hot-reloads them in-game. Wheel changes are undoable ' +
      'with Ctrl+Z.'),
    row('camera', 'right-drag orbit · middle-drag pan · wheel zoom. The left ' +
      'button never moves the camera — it always edits.'),
    row('parts', 'Select [V] a box around a limb → "Split to model" extracts ' +
      'it as its own named model (the engine needs one model per limb). ' +
      'Clicking a limb in the Models panel selects it entirely (ready for ' +
      'Ctrl+C / Del / fill). Cyan spheres on the bounding-box faces are resize ' +
      'handles — drag them to grow or shrink the box. "sync" in Limbs gives ' +
      'every model a limb entry; set root, parents and joints there. ' +
      'The orange ball is the joint anchor — drag it into the socket; the ' +
      'blue line is the swing axis.'),
    row('move', 'Move [X] drags a WHOLE limb: grab any limb and slide it, on ' +
      'the plane facing the camera, snapped to whole voxels. Its joint anchor ' +
      'moves with it, so the joint stays where it sits in the mesh — that is ' +
      'the difference from painting voxels around, which leaves the anchor ' +
      'behind. Children do NOT follow their parent: raising a torso means ' +
      'moving the head and arms too. The move itself is not on the undo ' +
      'stack — drag it back rather than Ctrl+Z — but your paint history ' +
      'survives it.'),
    row('walk', 'K toggles the gait preview (it walks the exported data, not ' +
      'an editor imitation). Legs need a two-bone IK chain (+ chain with the ' +
      'LOWER bone selected) and a gait block; leg groups define the gait ' +
      'state machine.'),
    row('clips', 'Clips are keyframed poses (attacks, hurt...). + clip, pick ' +
      'a part, drag the rotation rings, I writes a key at the cursor (no ' +
      'drag = holds the sampled pose), P plays, auto-key writes on ring ' +
      'release. Ease belongs to the outgoing key.'),
    row('flipbook', 'Frame-swap animation. On a rig a tag animates ONE part: ' +
      'duplicate that part\'s model (⧉), edit it, + appends it as a frame — ' +
      'every frame must carry the part or the engine drops it. On a plain ' +
      'file every model is a frame. space play · [ ] step · D duplicate · ' +
      'Del delete · O onion (red = prev, blue = next).'),
    row('micro', 'Two ways to go finer than one world voxel. A lone 2³/4³/8³ ' +
      'model is a micro BRICK: one world cell subdivided (green wireframe = ' +
      'the cell) — save under microvox/ and point a material\'s "micro" block ' +
      'at it; that is also how terrain and prefabs get micro detail. A mob ' +
      'gets micro detail from "skinScale" in the rig panel: the whole file ' +
      'is authored in skin units, skinScale of them per world voxel, so a ' +
      '136³ model at skinScale 8 is a 17-voxel-tall creature with 8× detail. ' +
      'The collider is derived coarser by the engine. The 2× ' +
      'button (Models panel) upscales an EXISTING mob in place: voxels, ' +
      'anchors and keys double and scale bumps — same size, finer grain.'));
  p.style.display = 'none';
  return p;
}

/* ==========================================================================
   10. file I/O
   ========================================================================== */

async function refreshFileList() {
  if (!ui.fileSel) return;
  try {
    const j = await (await fetch('/api/models', { cache: 'no-store' })).json();
    ui.fileSel.innerHTML = '';
    ui.fileSel.append(el('option', { value: '' }, '— open a model —'));
    for (const f of (j.files || [])) {
      if (!f.path.endsWith('.vox')) continue;         // .json are sidecars
      ui.fileSel.append(el('option', { value: f.path }, f.path));
    }
    if (docPath) ui.fileSel.value = docPath;
  } catch {
    ui.fileSel.innerHTML = '';
    ui.fileSel.append(el('option', { value: '' }, '(server not available)'));
  }
}

async function openPath(path) {
  if (!path) return;
  if (docDirty && !confirm('Discard unsaved model changes?')) {
    ui.fileSel.value = docPath || '';
    return;
  }
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent(path), { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const parsed = readVox(await r.arrayBuffer());
    for (const w of parsed.warnings) hooks.toast(path + ': ' + w, true);
    if (!parsed.prefab) throw new Error('no non-empty models');

    const over = parsed.prefab.models.find(m =>
      m.dim.x > MAX_EDIT_DIM || m.dim.y > MAX_EDIT_DIM || m.dim.z > MAX_EDIT_DIM);
    if (over)
      hooks.toast(`${path}: model "${over.name}" exceeds ${MAX_EDIT_DIM}³ — ` +
        'edits may be slow', true);

    doc = parsed.prefab;
    activeModel = 0;
    grid = doc.models[0].grid;
    docPath = path;
    docName = path.split('/').pop().replace(/\.vox$/i, '');
    clearUndo();             // also resets any open structural transaction
    setSelection(null);
    // Recover the document's art colours from its own RGBA chunk. This has to
    // happen before anything renders: the grids came back holding art INDICES,
    // and an index means nothing without the palette that issued it.
    adoptArtPalette(parsed.palette);

    // Sidecar is optional; a missing one is normal for a fresh model. It is
    // kept as the PARSED OBJECT and written back whole, so fields this editor
    // knows nothing about survive a load/save cycle untouched.
    sidecar = null;
    sidecarPath = path.replace(/\.vox$/i, '.json');
    try {
      const sr = await fetch('/api/model?path=' + encodeURIComponent(sidecarPath),
        { cache: 'no-store' });
      if (sr.ok) sidecar = JSON.parse(await sr.text());
    } catch (e) {
      hooks.toast('sidecar ' + sidecarPath + ' did not parse — treating as absent', true);
      sidecar = null;
    }

    clearDirty();
    if (initialised) { frameCamera(); updateMirrorPlane(); needsRebuild = true; }
    hooks.onModelsChanged?.();
    hooks.onSelectionChanged?.(null);
    hooks.onSidecarChanged?.();
    hooks.toast(`opened ${path} (${doc.models.length} model` +
      (doc.models.length === 1 ? '' : 's') + ')');
  } catch (e) {
    hooks.toast('open failed: ' + (e.message || e), true);
  }
}

function applyPreset(v) {
  if (!v) return;
  if (docDirty && !confirm('Discard unsaved model changes?')) return;
  const micro = /^micro(\d+)$/.exec(v);
  if (micro) {
    const n = +micro[1];
    newModel(n, n, n, 'micro' + n);
    hooks.toast(`new ${n}³ micro brick — this is exactly one world cell`);
    return;
  }
  if (v === 'custom') {
    const s = prompt(`dimensions, "x y z" or a single number (max ${MAX_EDIT_DIM})`, '16 16 16');
    if (!s) return;
    const p = s.trim().split(/[\s,x×]+/).map(Number).filter(n => n > 0);
    if (!p.length) { hooks.toast('could not parse dimensions', true); return; }
    const [dx, dy, dz] = p.length === 1 ? [p[0], p[0], p[0]] : [p[0], p[1] ?? p[0], p[2] ?? p[0]];
    if ([dx, dy, dz].some(n => n > MAX_EDIT_DIM)) {
      hooks.toast('max is ' + MAX_EDIT_DIM + ' per axis', true); return;
    }
    newModel(dx, dy, dz);
    hooks.toast(`new ${dx}×${dy}×${dz} model`);
    return;
  }
  const n = +v;
  newModel(n, n, n);
  hooks.toast(`new ${n}³ model`);
}

async function save(saveAs) {
  if (!grid) return;
  let path = docPath;
  if (saveAs || !path) {
    const suggest = path || 'models/' + docName + '.vox';
    const p = prompt('save as (relative to assets/; models/, mobs/ or microvox/)', suggest);
    if (!p) return;
    path = p.trim();
    if (!/\.vox$/i.test(path)) path += '.vox';
  }

  // Drop models that ended up empty; writeVox rejects a zero-voxel model and
  // an empty limb would break the mob loader anyway.
  const live = doc.models.filter(m => m.grid.data.some(v => v !== 0));
  if (!live.length) { hooks.toast('nothing to save — every model is empty', true); return; }
  if (live.length !== doc.models.length) {
    hooks.toast(`skipping ${doc.models.length - live.length} empty model(s)`, true);
    // Dropping a model renumbers everything after it in the .vox, and the
    // sidecar references models by INDEX (flipbook frames' "model" field).
    if (sidecar && sidecar.flipbooks && Object.keys(sidecar.flipbooks).length)
      hooks.toast('dropped models shift .vox model indices — re-check each ' +
        'flipbook frame\'s "model" number after this save', true);
  }

  // Multi-model files need the scene graph or the limbs lose their names and
  // pile up at the origin. A single model gets a scene graph too when it has a
  // meaningful name, which costs ~120 bytes and keeps MagicaVoxel showing it.
  const usingScene = live.length > 1 || !!(live[0].name && live[0].name !== 'model');

  // Save the ENGINE's canonical form: models cropped tight, prefab rebased to
  // the voxel min corner (voxload.cpp does this at load anyway; writing the
  // slack editing boxes both fails the round-trip below and lets anchors
  // drift by the slack margin in-game). `shift` is how far the content floated
  // off the editor's origin — prefab-space sidecar data must move with it.
  const { prefab, shift } = tightenPrefab({ size: doc.size, models: live });
  const shifted = !!(shift.x || shift.y || shift.z);

  // The RGBA chunk carries BOTH halves of the palette: material colours (from
  // refreshMaterials) and the art colours this document painted with. Colours
  // allocated since the last refresh only exist in artPalette, so fold them in
  // here — a .vox whose art indices point at black entries loads as a black
  // creature, and the round-trip below would not catch it (it compares
  // indices, not the colours they resolve to).
  artPalette.writeInto(palette);

  // Round-trip assertion, every save. The prefab-level test is the one that
  // matters here: the per-model test would pass even if every limb landed on
  // top of its neighbour.
  const rt = usingScene
    ? prefabRoundTripTest(prefab, palette)
    : roundTripTest([gridToModel(live[0].grid, docName)], palette);
  if (!rt.ok) {
    hooks.toast('SAVE ABORTED — .vox round-trip failed: ' + rt.error, true);
    return;
  }

  try {
    const bytes = usingScene
      ? writeVox(prefabToVoxModels(prefab), palette, { scene: true })
      : writeVox([gridToModel(live[0].grid, docName)], palette);
    const r = await fetch('/api/model?path=' + encodeURIComponent(path), {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: bytes,
    });
    const j = await r.json().catch(() => ({ ok: false, error: 'bad response' }));
    if (!j.ok) { hooks.toast('save failed: ' + (j.error || r.status), true); return; }

    // Sidecar. `sidecar` is the object parsed from disk with only the fields
    // the rig UI edits mutated in place, so unknown keys (clips, chains,
    // anything a later wave adds) are written back verbatim. When there is no
    // sidecar yet we create a stub so the pairing exists from the first save.
    const spath = path.replace(/\.vox$/i, '.json');
    let sidecarNote = '';
    // Anchors are prefab-space absolutes; if the tight rebase moved the
    // content, the WRITTEN sidecar moves with it (the in-memory one stays in
    // editor coordinates — the viewport has not changed).
    let sideOut = sidecar;
    if (shifted && sidecar && Array.isArray(sidecar.limbs) &&
        sidecar.limbs.some(l => Array.isArray(l.anchor))) {
      sideOut = JSON.parse(JSON.stringify(sidecar));
      for (const l of sideOut.limbs)
        if (Array.isArray(l.anchor) && l.anchor.length === 3)
          l.anchor = [l.anchor[0] - shift.x, l.anchor[1] - shift.y,
                      l.anchor[2] - shift.z];
      hooks.toast(`content floats ${shift.x},${shift.y},${shift.z} off the ` +
        'origin — saved anchors were rebased to match the engine\'s crop', true);
    }
    try {
      if (sideOut && Object.keys(sideOut).length) {
        const sr = await fetch('/api/model?path=' + encodeURIComponent(spath), {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(sideOut, null, 2) + '\n',
        });
        const sj = await sr.json().catch(() => ({ ok: false }));
        sidecarNote = sj.ok ? ', sidecar saved' : ', SIDECAR SAVE FAILED';
      } else {
        const probe = await fetch('/api/model?path=' + encodeURIComponent(spath),
          { cache: 'no-store' });
        if (!probe.ok) {
          await fetch('/api/model?path=' + encodeURIComponent(spath), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: '{}\n',
          });
          sidecarNote = ', sidecar created';
        }
      }
    } catch { sidecarNote = ', sidecar write errored'; }

    docPath = path;
    sidecarPath = spath;
    docName = path.split('/').pop().replace(/\.vox$/i, '');
    clearDirty();
    await refreshFileList();
    await hooks.onSave?.();
    hooks.toast(`saved ${path} (${rt.bytes} bytes, round-trip ok${sidecarNote})`);
  } catch (e) {
    hooks.toast('save failed: ' + (e.message || e), true);
  }
}

/* ==========================================================================
   11. public entry points
   ========================================================================== */

let sectionEl = null;
const isActive = () => !!sectionEl && sectionEl.classList.contains('active');

function showFailure(msg) {
  initFailed = true;
  if (!sectionEl) return;
  sectionEl.innerHTML = '';
  sectionEl.append(
    el('div', { class: 'banner' }, '⚠',
      el('div', {}, msg)));
}

/**
 * Called by tuner.html once, at load. Only wires things up — no WebGL context
 * is created until the tab is first shown.
 */
export function attach(opts) {
  el = opts.el;
  hooks = { ...hooks, ...opts };
  sectionEl = opts.section;
  buildUI(sectionEl);
  newModel(16, 16, 16);
  document.addEventListener('keydown', onKeyDown);
  return true;
}

/**
 * Merge in the rig/timeline hooks. Kept separate from attach() so editor.js has
 * no import-time dependency on rig.js: if rig.js fails to load, the voxel
 * editor still works and simply has no rig panel.
 */
export function attachRig(rigHooks) {
  hooks = { ...hooks, ...rigHooks };
  needsRebuild = true;
}

/**
 * Called every time the Models tab is activated. Creates the renderer on the
 * first call and refreshes anything that may have changed under us (materials
 * are edited on another tab).
 */
export function activate() {
  if (initFailed) return;
  refreshMaterials();
  renderPalette();

  if (!initialised) {
    if (!hasWebGL()) {
      showFailure('WebGL is not available in this browser, so the 3D model editor ' +
        'cannot run. Everything else in the tuner works normally.');
      return;
    }
    try {
      buildScene();
    } catch (e) {
      showFailure('the 3D editor failed to start: ' + (e.message || e));
      return;
    }
    initialised = true;
    canvas.addEventListener('pointerdown', onPointerDown);
    canvas.addEventListener('pointermove', onPointerMove);
    canvas.addEventListener('pointerup', onPointerUp);
    canvas.addEventListener('pointerleave', () => {
      pointer.inside = false;
      if (!drag) ghost.visible = false;
    });
    canvas.addEventListener('contextmenu', e => e.preventDefault());
    // Middle-drag is PAN (see controls.mouseButtons). OrbitControls is
    // pointer-events only and sets touchAction='none', which covers touch
    // and the wheel — but NOT middle-click autoscroll, which the browser
    // triggers off the legacy `mousedown`. Nothing else in the stack listens
    // for that, so panning also kicked the tuner page into scroll mode.
    // Same shape as the contextmenu guard above: the button is ours.
    canvas.addEventListener('mousedown', e => {
      if (e.button === 1) e.preventDefault();
    });
    window.addEventListener('resize', resize);
    // The section is display:none until now, so clientWidth was 0 during
    // buildScene(); size it after the tab is visible.
    animate();
    refreshFileList();

    // Sanity check of the axis mapping — cheap, and a silent failure here
    // would corrupt every asset the editor writes.
    const a = axisSelfTest();
    if (!a.ok) hooks.toast('vox.js axis self-test FAILED: ' + a.error, true);
  }

  // Runs on every tab activation, not just the first: this is the earliest
  // point at which the section is visible and the flex row has a real width.
  restoreSideW();
  resize();
  // Directories on disk may have changed while another tab was up — or in
  // another session entirely, which is the normal case for the limb library.
  hooks.onActivate?.();
  updateMirrorPlane();
  updateMicroGhost();
  updateGizmo();
  updateResizeHandles();
  renderToolbar();
  // Models may have changed under us (nothing else edits them today, but the
  // skeleton is cheap to rebuild). Do NOT fire onSidecarChanged here: that
  // hook means "a different sidecar object was loaded" and resets the rig
  // panel's selection — firing it on every tab switch threw away the user's
  // selected part / clip / tag each time they peeked at another tab.
  hooks.onModelsChanged?.();
  needsRebuild = true;
  updateStatus();
}

/** True when the model has unsaved edits — feeds the tuner's dirty pill. */
export const isDirty = () => docDirty;

/** Save from the page's Ctrl+S handler. */
export function saveFromHost() { return save(false); }

/* ---- API consumed by rig.js -------------------------------------------- */

export const getDoc = () => doc;
export const getActiveModel = () => activeModel;
export const getModels = () => (doc ? doc.models : []);
/** File stem of the open document — a saved limb records where it came from. */
export const getDocName = () => docName;
/** Server-relative path of the open document ("mobs/asha.vox"), '' if unsaved.
 *  The creature shelf lists every OTHER creature, and identity is the path:
 *  two files can share a stem across models/ and mobs/. */
export const getDocPath = () => docPath || '';
export const getSelection = () => selection;
export const getSidecar = () => sidecar;
export const getMaterials = () => materials;
export const matColorOf = matColor;

export function setSidecar(s) { sidecar = s; }

let _sidecarBefore = null;
function snapshotSidecar() {
  return sidecar ? JSON.stringify(sidecar) : null;
}
export function touchSidecar() {
  if (!_sidecarBefore) _sidecarBefore = snapshotSidecar();
  markDirty();
}
export function commitSidecarUndo() {
  if (!_sidecarBefore) return;
  const after = snapshotSidecar();
  if (after === _sidecarBefore) { _sidecarBefore = null; return; }
  undoStack.push({ type: 'sidecar', data: { before: _sidecarBefore, after } });
  redoStack.length = 0;
  _sidecarBefore = null;
}
export function discardSidecarUndo() { _sidecarBefore = null; }

let _moveUndoBefore = null;
function pushMoveUndo(indices) {
  _moveUndoBefore = {
    offsets: indices.map(j => ({ mi: j, offset: { ...doc.models[j].offset } })),
    anchors: (sidecar && Array.isArray(sidecar.limbs))
      ? sidecar.limbs.filter(l => Array.isArray(l.anchor) && l.anchor.length === 3)
                      .map(l => [l.name, l.anchor.slice()])
      : null,
  };
}
function finishMoveUndo(indices) {
  if (!_moveUndoBefore) return;
  const after = indices.map(j => ({ mi: j, offset: { ...doc.models[j].offset } }));
  const anchorsAfter = (sidecar && Array.isArray(sidecar.limbs))
    ? sidecar.limbs.filter(l => Array.isArray(l.anchor) && l.anchor.length === 3)
                    .map(l => [l.name, l.anchor.slice()])
    : null;
  const b = _moveUndoBefore;
  _moveUndoBefore = null;
  const changed = b.offsets.some((e, k) => {
    const a = after[k]; return a.offset.x !== e.offset.x ||
      a.offset.y !== e.offset.y || a.offset.z !== e.offset.z;
  });
  if (!changed) return;
  commitSidecarUndo();
  undoStack.push({ type: 'move', data: {
    before: b.offsets, after,
    anchors: (b.anchors || anchorsAfter) ? {
      before: b.anchors || [], after: anchorsAfter || [],
    } : null,
  }});
  redoStack.length = 0;
}

export { setActiveModel, addModel, duplicateModel, removeModel, renameModel,
         splitSelectionToModel, upscaleDoc, downscaleDoc, markDirty, moveModel,
         growModel, setSelection, graftModels, pushMoveUndo, finishMoveUndo,
         // rig.js owns edits that are HALF structural and half sidecar (a limb
         // swap, a 2× resample). Both halves have to land in one undo step, so
         // it opens the transaction rather than each half opening its own.
         beginStructural, endStructural };

/** The document's art palette, so a limb saved out of it can carry its
 *  colours (grid.color holds INDICES into this and nothing else). */
export const getArtPalette = () => artPalette;

/** Force a full instance rebuild (gait preview / onion skin drive this). */
export function invalidate() { needsRebuild = true; }

/** Render one model's grid into a small canvas, for timeline thumbnails. */
export function thumbnail(modelIndex, size = 40) {
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const g2 = c.getContext('2d');
  const m = doc?.models[modelIndex];
  if (!m) return c;
  // Cheap orthographic front view (+Z toward the viewer): paint columns back
  // to front so the nearest voxel wins. No lighting — this is a 40px chip.
  const sc = size / Math.max(m.dim.x, m.dim.y);
  const ox = (size - m.dim.x * sc) / 2, oy = (size - m.dim.y * sc) / 2;
  for (let z = 0; z < m.dim.z; z++)
    for (let y = 0; y < m.dim.y; y++)
      for (let x = 0; x < m.dim.x; x++) {
        const i = x + y * m.dim.x + z * m.dim.x * m.dim.y;
        const v = m.grid.data[i];
        if (!v) continue;
        g2.fillStyle = shownColor(v, m.grid.color ? m.grid.color[i] : 0);
        // engine +Y is up, canvas +Y is down
        g2.fillRect(ox + x * sc, oy + (m.dim.y - 1 - y) * sc,
                    Math.ceil(sc), Math.ceil(sc));
      }
  return c;
}
