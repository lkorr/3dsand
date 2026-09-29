/* housegen.js — the house generator: parameters + seed in, a building out.
 *
 * WHAT IT IS FOR. docs/PLAN_world_editor.md §2.2 / P2. A generator makes the
 * SCAFFOLD of a building — a clean, regular, believable rural-medieval house —
 * and a person then hand-edits the deliberate choices on top (a manger, an
 * anvil, the book on the shelf). So this optimises for STRUCTURE, not noise:
 * every post sits on a bay line, every window is framed, every opening has a
 * lintel, and nothing is roughened "for realism" that a hand edit would then
 * have to scrub off. The seed varies the small choices (brace pattern, door
 * offset, which end the beds go) and never the silhouette.
 *
 * WHAT IT IS NOT. It does not place the building in the world (P4: a
 * `structure` ref -> the worldgen stamp path), and it does not know Harrowby.
 *
 * PURE MODULE — same discipline as treegen.js. No DOM, no fetch, no WebGL, no
 * Math.random(). The tuner's Environment -> Structures page and
 * `scripts/bake_structure.mjs` / `scripts/test_housegen.mjs` all call the SAME
 * functions, so the preview is byte-for-byte the asset. Randomness is the
 * counter hash `frand(seed, key...)`: same params + same seed -> identical
 * bytes on any machine, and nudging one slider does not reshuffle unrelated
 * choices (each draw is keyed by what it decides, not by call order).
 *
 * COORDINATES are the engine's: Y up, X/Z horizontal, one cell = one voxel,
 * heading 0 faces +Z and heading 90 faces +X (CLAUDE.md conventions). Every
 * size parameter is authored in METRES and converted with `opts.vpm` (voxels
 * per metre, default 10 = the engine's 0.1 m voxel; bake_structure.mjs reads
 * it from src/sim/world.h the way bake_trees.mjs does).
 *
 * THE FRAME OF A HOUSE (local, after the crop to occupied bounds — the same
 * box src/sim/voxload.cpp rebases a .vox to, so slot coordinates survive the
 * round trip):
 *   - the FRONT wall faces +Z (heading 0), the back -Z, the LEFT gable -X,
 *     the RIGHT gable +X; the ridge of a gable roof runs along X.
 *   - `origin` = [centre of the wall footprint in X, GRADE row, centre in Z].
 *     GRADE is the first row above the floor slab: where your feet are on the
 *     ground floor and where the outside ground surface should sit. A placed
 *     structure's ref y IS this row (PLAN §5 P4).
 *   - Below grade there is a shallow footing under the walls, so a house
 *     stamped onto a not-quite-level pad does not float.
 *
 * SLOTS are refs-in-waiting (PLAN §2.2): { name, kind, pos, yaw, props }.
 * `pos` is a local voxel cell; for anything you stand at it is the FEET row.
 * Kinds use the ref kinds of PLAN §2.1 (door, bed, container, marker,
 * waynode); a hearth and a work spot are `marker`s with a `tags` prop.
 */

'use strict';

// =============================================================================
// constants
// =============================================================================

export const DEFAULT_VOX_PER_M = 10;
/** One .vox model is at most 256 cells on an axis (the XYZI byte coords). */
export const MAX_DIM = 256;
/** Bumped when the SAME params + seed would produce different voxels. Recorded
 *  in every struct.json's generator block so a re-bake can say why it moved. */
export const GENERATOR_VERSION = 1;

/** The local palette. A cell holds an index into this list (1-based, 0 =
 *  air), and the file writers resolve NAMES against materials.json at write
 *  time — never an id baked into this module. Append only. */
export const PALETTE = ['timber', 'plank', 'daub', 'cobble', 'flagstone',
                        'roof_tile', 'thatch', 'door_wood', 'straw_bed', 'glass'];
const TI = 1, PL = 2, DB = 3, CB = 4, FS = 5, RT = 6, TH = 7, DW = 8, SB = 9, GL = 10;
const MAT_OF = {timber: TI, plank: PL, daub: DB, cobble: CB, flagstone: FS,
                roof_tile: RT, thatch: TH, door_wood: DW, straw_bed: SB, glass: GL};

// =============================================================================
// the counter hash (same construction as treegen.js; only has to be STABLE)
// =============================================================================
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x >>> 0;
}
export function hashN(...vals) {
  let h = 0x9e3779b9;
  for (let i = 0; i < vals.length; i++) {
    h = mix32((h ^ (vals[i] | 0)) >>> 0);
    h = (h + 0x9e3779b9) >>> 0;
  }
  return mix32(h);
}
/** Uniform [0,1), keyed by the seed and by WHAT is being decided. */
function frand(seed, ...key) { return hashN(seed, ...key) / 4294967296; }
// Decision keys: one per choice, so they never collide or shift.
const K_BRACE = 1, K_DOOR = 2, K_BED = 3, K_PART = 4, K_CHIM = 5, K_DOORB = 6;

// =============================================================================
// THE SCHEMA — every knob there is, and nothing else is editable
//
// Same row vocabulary as trees.js ({k, n, d, min, max, step, int}); `kind`
// adds 'enum' (options) and 'bool'. The Structures page builds its panel from
// this table and normalizeParams clamps against it, so "what can I change about
// a house" has one answer and adding a knob means adding a row here.
// =============================================================================
export const SCHEMA = [
  {sec: 'Footprint', note: 'Outer wall dimensions in metres. The ridge of a gable roof runs along the WIDTH; the front (door) wall faces +Z.'},
  {k: 'width', n: 'width (m)', min: 4, max: 14, step: 0.1,
   d: 'Along the ridge, wall face to wall face. A cottage is 6-8 m, a longhouse 10-14 m.'},
  {k: 'depth', n: 'depth (m)', min: 3.5, max: 9, step: 0.1,
   d: 'Front wall to back wall. Beds stand perpendicular to the back wall, so under ~4 m there is no room for one on the ground floor.'},
  {k: 'storeys', n: 'storeys', min: 1, max: 2, step: 1, int: true,
   d: 'Walled storeys under the eaves. Two storeys get a plank upper floor on visible joists and a steep stair.'},
  {k: 'storeyHeight', n: 'storey height (m)', min: 2.2, max: 3.2, step: 0.1,
   d: 'Floor to the underside of the next floor (or the wall plate). 2.6 m clears a 1.7 m person under the tie beams.'},

  {sec: 'Walls', note: 'The frame is the look: posts on bay lines, a sill beam, a mid rail at window-sill height, a wall plate, corner braces.'},
  {k: 'wallStyle', n: 'wall style', kind: 'enum', options: ['timber', 'plank_daub', 'plank', 'cobble'],
   d: 'timber = box frame with daub panels set back one voxel (the frame stands proud). plank_daub = weatherboards below the mid rail, daub above. plank = weatherboarded all over. cobble = rubble stone, 0.4 m thick, dressed quoins, timber lintels.'},
  {k: 'plinth', n: 'plinth (m)', min: 0, max: 0.8, step: 0.1,
   d: 'A cobble course the frame sits on, one voxel proud of the wall face. Keeps the sill beam out of the wet; a cobble wall gets a one-row footing course instead.'},
  {k: 'panel', n: 'max panel (m)', min: 0.8, max: 2, step: 0.1,
   d: 'Widest daub panel before another post is added. Smaller reads as close studding (richer); larger as square panelling (plainer).'},
  {k: 'braces', n: 'corner braces', kind: 'bool',
   d: 'Diagonal braces in the corner panels of framed walls. Which half of the storey they sit in is a seeded choice.'},

  {sec: 'Roof', note: 'Pitch is snapped to a rise of 2..8 voxels per 4 so the slope steps in a regular rhythm — the stats line says the pitch you actually got.'},
  {k: 'roofShape', n: 'roof shape', kind: 'enum', options: ['gable', 'hip'],
   d: 'gable = two slopes and triangular end walls with bargeboards. hip = four slopes, no gable walls (a thatched hip is the classic cottage).'},
  {k: 'roofMaterial', n: 'roof covering', kind: 'enum', options: ['thatch', 'tile'],
   d: 'thatch = ~0.4 m of straw with a raised, scalloped ridge roll (flammable). tile = fired clay, thin, with course lines, ridge tiles, fascia and bargeboards.'},
  {k: 'pitch', n: 'pitch (deg)', min: 27, max: 63, step: 1,
   d: 'Thatch wants 45+ to shed rain; tile is happy at 30-45.'},
  {k: 'overhang', n: 'overhang (m)', min: 0, max: 0.8, step: 0.1,
   d: 'How far the eaves reach past the wall face. The gable verge gets three quarters of it.'},
  {k: 'chimney', n: 'chimney', kind: 'enum', options: ['none', 'left', 'right'],
   d: 'A cobble stack on the left (-X) or right (+X) gable with an inside fireplace, a timber bressumer and a flagstone hearth. none = an open central hearth in the main room (a longhouse).'},

  {sec: 'Openings', note: 'Doors are 0.9 x 2.0 m with a timber frame and lintel; the leaf is door_wood, hung on the inside face, opening inward.'},
  {k: 'doorsFront', n: 'front doors', min: 0, max: 2, step: 1, int: true, d: 'Doors in the front (+Z) wall.'},
  {k: 'doorsBack', n: 'back doors', min: 0, max: 1, step: 1, int: true,
   d: 'A back door lines up with the first front door: a cross passage.'},
  {k: 'doorsSide', n: 'gable door', min: 0, max: 1, step: 1, int: true,
   d: 'A door in the gable without the chimney (a byre door on a longhouse).'},
  {k: 'windows', n: 'windows / storey', min: 0, max: 6, step: 1, int: true,
   d: 'Per storey, shared between the front and back walls (front gets the odd one). A window that will not fit clear of doors, partitions and corners is skipped and the stats line says so.'},
  {k: 'sideWindows', n: 'gable windows', min: 0, max: 2, step: 1, int: true,
   d: 'Per storey, in each gable wall that has no chimney.'},
  {k: 'windowStyle', n: 'window style', kind: 'enum', options: ['shutters', 'glass'],
   d: 'shutters = an open hole with a projecting sill and two plank shutters folded back against the wall (glass was dear in a hamlet). glass = a pane mid-wall behind a timber cross mullion.'},

  {sec: 'Interior', note: 'Furniture is scaffold for the hand edit: a bed is a timber frame with a straw_bed mattress, a chest a plank box with timber corners.'},
  {k: 'floor', n: 'ground floor', kind: 'enum', options: ['flagstone', 'plank', 'cobble'],
   d: 'The ground-floor slab. Upper floors and lofts are always plank.'},
  {k: 'partitions', n: 'partitions', min: 0, max: 2, step: 1, int: true,
   d: 'Plank cross walls on the ground floor, each with an open framed doorway. The room at the chimney end is kept the larger (it is the hall).'},
  {k: 'loft', n: 'sleeping loft', kind: 'bool',
   d: 'A plank loft on the tie beams at the end away from the chimney, reached by a steep stair. One-storey houses only.'},
  {k: 'loftLength', n: 'loft length', min: 0.3, max: 1, step: 0.05,
   d: 'Fraction of the inside length the loft covers.'},
  {k: 'beds', n: 'beds', min: 0, max: 4, step: 1, int: true,
   d: 'Placed upstairs / in the loft if there is one, otherwise in the ground-floor rooms away from the hearth. A bed that finds no spot with 1 m of headroom is skipped and the stats line says so.'},
  {k: 'chests', n: 'chests', min: 0, max: 3, step: 1, int: true,
   d: 'At the foot of a bed first, then along the ground-floor walls.'}
];

const DEFAULTS = {
  width: 8, depth: 5.5, storeys: 1, storeyHeight: 2.6,
  wallStyle: 'timber', plinth: 0.4, panel: 1.2, braces: true,
  roofShape: 'gable', roofMaterial: 'thatch', pitch: 50, overhang: 0.4, chimney: 'right',
  doorsFront: 1, doorsBack: 0, doorsSide: 0, windows: 2, sideWindows: 0, windowStyle: 'shutters',
  floor: 'flagstone', partitions: 0, loft: false, loftLength: 0.5, beds: 1, chests: 1
};

export function defaultParams() { return JSON.parse(JSON.stringify(DEFAULTS)); }

/** Fill, clamp and snap. Unknown keys are dropped (the schema is the truth);
 *  a house with no door at all gets one front door. */
export function normalizeParams(src) {
  const p = defaultParams();
  const s = src && typeof src === 'object' ? src : {};
  for (const r of SCHEMA) {
    if (!r.k) continue;
    const v = s[r.k];
    if (r.kind === 'enum') { if (r.options.includes(v)) p[r.k] = v; }
    else if (r.kind === 'bool') { if (typeof v === 'boolean') p[r.k] = v; }
    else if (typeof v === 'number' && isFinite(v)) {
      let x = Math.min(r.max, Math.max(r.min, v));
      if (r.int) x = Math.round(x);
      else x = Math.round(x / r.step) * r.step;
      // Kill float dust (0.30000000000000004) so the params block in a
      // struct.json diffs as the number a person typed.
      p[r.k] = Number(x.toFixed(4));
    }
  }
  if (p.doorsFront + p.doorsBack + p.doorsSide === 0) p.doorsFront = 1;
  if (p.storeys > 1) p.loft = false;
  return p;
}

// =============================================================================
// presets — the starting points the Structures page offers under "New"
// =============================================================================
export const PRESETS = {
  cottage: {seed: 3, params: {width: 7, depth: 5, storeys: 1, wallStyle: 'timber', roofShape: 'hip',
    roofMaterial: 'thatch', pitch: 50, overhang: 0.5, chimney: 'right', doorsFront: 1, windows: 2,
    partitions: 0, beds: 1, chests: 1, floor: 'flagstone', plinth: 0.4}},
  longhouse: {seed: 11, params: {width: 13, depth: 6, storeys: 1, storeyHeight: 2.6, wallStyle: 'timber',
    plinth: 0.4, panel: 1.3, roofShape: 'gable', roofMaterial: 'thatch', pitch: 52, overhang: 0.5,
    chimney: 'none', doorsFront: 1, doorsBack: 1, doorsSide: 1, windows: 2, sideWindows: 0,
    floor: 'cobble', partitions: 1, loft: true, loftLength: 0.45, beds: 2, chests: 1}},
  smithy: {seed: 5, params: {width: 9, depth: 6.5, storeys: 1, storeyHeight: 2.8, wallStyle: 'cobble',
    plinth: 0, roofShape: 'gable', roofMaterial: 'tile', pitch: 38, overhang: 0.4, chimney: 'left',
    doorsFront: 1, doorsBack: 1, windows: 2, sideWindows: 1, floor: 'flagstone', partitions: 1,
    loft: false, beds: 1, chests: 1}},
  alehouse: {seed: 7, params: {width: 9.5, depth: 6, storeys: 1, storeyHeight: 2.6, wallStyle: 'plank_daub',
    plinth: 0.3, roofShape: 'hip', roofMaterial: 'thatch', pitch: 50, overhang: 0.6, chimney: 'right',
    doorsFront: 1, doorsBack: 0, windows: 3, sideWindows: 1, floor: 'flagstone', partitions: 1,
    loft: false, beds: 2, chests: 1}},
  townhouse: {seed: 2, params: {width: 7.5, depth: 6, storeys: 2, storeyHeight: 2.5, wallStyle: 'timber',
    plinth: 0.5, panel: 1.0, roofShape: 'gable', roofMaterial: 'tile', pitch: 45, overhang: 0.4,
    chimney: 'left', doorsFront: 1, windows: 2, sideWindows: 1, windowStyle: 'glass', floor: 'plank',
    partitions: 1, beds: 2, chests: 2}}
};
export const PRESET_ORDER = ['cottage', 'longhouse', 'smithy', 'alehouse', 'townhouse'];

/** The three committed fixtures in assets/structures/samples/ (P4 and the gate
 *  use them). Each is a preset at its own seed; `bake_structure.mjs --samples`
 *  regenerates exactly these. */
export const SAMPLES = {
  'samples/longhouse': 'longhouse',
  'samples/smithy': 'smithy',
  'samples/alehouse': 'alehouse'
};

// =============================================================================
// the generator
// =============================================================================

/**
 * Generate a house.
 * @param {object} params  schema params (normalised here)
 * @param {number} seed
 * @param {{vpm?:number}} opts
 * @returns {{dim:{x,y,z}, cells:Uint8Array, names:string[], origin:number[],
 *            slots:object[], meta:object, params:object, seed:number}}
 *   cells[(z*ny + y)*nx + x] = 1-based index into `names` (PALETTE), 0 = air.
 */
export function generateHouse(params, seed, opts) {
  const P = normalizeParams(params);
  seed = (seed | 0) >>> 0;
  const vpm = (opts && opts.vpm) || DEFAULT_VOX_PER_M;
  const mr = (v) => Math.round(v * vpm);
  const m = (v) => Math.max(1, mr(v));
  const warnings = [];

  // ---- dimensions (voxels) --------------------------------------------------
  const W = m(P.width), D = m(P.depth);
  const cobbleWall = P.wallStyle === 'cobble';
  const t = cobbleWall ? m(0.4) : m(0.3);          // wall thickness
  const pw = m(0.2);                                // post / rail size
  const H = m(P.storeyHeight);
  const nS = P.storeys;
  const O = mr(P.overhang);                         // eaves overhang
  const Ov = Math.round(O * 0.75);                  // verge overhang
  const plinthH = cobbleWall ? 1 : mr(P.plinth);
  const chimProt = P.chimney === 'none' ? 0 : m(0.6);
  const F = Math.max(1, mr(0.2));                   // footing depth below grade
  const g = F;                                      // grade row
  const doorW = m(0.9), doorH = m(2.0);
  const winW = m(0.8), winH = m(0.8), sillH = m(0.9);
  const shutW = Math.ceil(winW / 2);

  // Roof: the slope is a rise of q voxels per 4 of run, so every pitch steps in
  // a regular, repeating rhythm instead of the ragged stair a raw tan() gives.
  const q = Math.min(8, Math.max(2, Math.round(Math.tan(P.pitch * Math.PI / 180) * 4)));
  const secant = Math.sqrt(1 + (q / 4) * (q / 4));
  const Tv = P.roofMaterial === 'thatch' ? Math.max(3, Math.round(m(0.4) * secant))
                                         : Math.max(2, Math.round(m(0.15) * secant));

  const padX = Math.max(O, Ov) + chimProt + shutW + 4;
  const padZ = O + shutW + 4;
  const x0 = padX, x1 = padX + W, z0 = padZ, z1 = padZ + D;
  const floorY = [g];
  for (let k = 1; k < nS; k++) floorY.push(floorY[k - 1] + H + 1);   // slab row between
  const wallTopY = floorY[nS - 1] + H - 1;
  const halfIn = Math.floor((D - 1) / 2);            // deepest inward index (gable)
  const ridgeTopEst = wallTopY + 1 + Math.floor(((2 * Math.max(halfIn, Math.floor((Math.min(W, D) - 1) / 2)) + 1) * q) / 8) + Tv + 4;
  const NX = x1 + padX + 2, NZ = z1 + padZ + 2;
  const NY = ridgeTopEst + m(1.2) + 4;

  const cells = new Uint8Array(NX * NY * NZ);
  const idx = (x, y, z) => (z * NY + y) * NX + x;
  const inb = (x, y, z) => x >= 0 && y >= 0 && z >= 0 && x < NX && y < NY && z < NZ;
  const set = (x, y, z, v) => { if (inb(x, y, z)) cells[idx(x, y, z)] = v; };
  const get = (x, y, z) => (inb(x, y, z) ? cells[idx(x, y, z)] : 0);
  const box = (xa, ya, za, xb, yb, zb, v) => {      // half-open
    for (let z = za; z < zb; z++) for (let y = ya; y < yb; y++) for (let x = xa; x < xb; x++) set(x, y, z, v);
  };

  // Interior (inside the wall faces).
  const ix0 = x0 + t, ix1 = x1 - t, iz0 = z0 + t, iz1 = z1 - t;
  const floorMat = MAT_OF[P.floor];

  // ---- walls as (u along, w inward) frames ------------------------------------
  // `u` runs along the OUTER face (0..L-1), `w` from the outer face inward
  // (0..t-1; -1 is one voxel proud, where sills and shutters sit). Every wall
  // spans its full outer length, so the four corner blocks are written by two
  // walls — both call them corner post (or quoin), so they agree.
  const WALLS = {
    front: {cx: x0, cz: z1 - 1, du: [1, 0], dw: [0, -1], L: W, heading: 0},
    back:  {cx: x0, cz: z0,     du: [1, 0], dw: [0, 1],  L: W, heading: 180},
    left:  {cx: x0, cz: z0,     du: [0, 1], dw: [1, 0],  L: D, heading: 270},
    right: {cx: x1 - 1, cz: z0, du: [0, 1], dw: [-1, 0], L: D, heading: 90}
  };
  for (const [name, wl] of Object.entries(WALLS)) { wl.name = name; wl.open = []; }
  const wx = (wl, u, w) => wl.cx + wl.du[0] * u + wl.dw[0] * w;
  const wz = (wl, u, w) => wl.cz + wl.du[1] * u + wl.dw[1] * w;
  const wset = (wl, u, w, y, v) => set(wx(wl, u, w), y, wz(wl, u, w), v);
  // u of a world x (long walls) or z (gables).
  const uOfX = (x) => x - x0, uOfZ = (z) => z - z0;

  // ---- plan: partitions (ground floor), which room holds the hearth ------------
  // The HALL is the room with the fire, and it is the larger: partitions are
  // measured from the chimney end (58% with one partition, 45% + 30% with
  // two), from the left with no chimney — so the hall is always the first room
  // counted from that end, and the far end is where the stair and loft go.
  const partW = m(0.2);
  const partitions = [];   // {x (first cell), dz0 (doorway)}
  const farLeft = P.chimney === 'right';
  {
    const n = P.partitions;
    const span = ix1 - ix0;
    const fr = n === 1 ? [0.58] : n === 2 ? [0.45, 0.75] : [];
    for (let k = 0; k < n; k++) {
      const f = fr[k] + (frand(seed, K_PART, k) - 0.5) * 0.06;
      let px = farLeft ? ix1 - Math.round(span * f) - partW : ix0 + Math.round(span * f);
      px = Math.max(ix0 + m(1.4), Math.min(ix1 - m(1.4) - partW, px));
      partitions.push({x: px});
    }
    partitions.sort((a, b) => a.x - b.x);
  }

  // ---- plan: stair (two storeys or a loft) --------------------------------------
  // A steep stair of plank treads on a timber stringer, 0.2 m rise per 0.2 m
  // run, against the back wall at the end AWAY from the chimney. It is solid
  // voxels a body can walk up, which a vertical ladder would not be.
  const rise = m(0.2), run = m(0.2), stairW = m(0.8);
  let stair = null;
  let loft = null;
  if (nS > 1 || P.loft) {
    const yT = nS > 1 ? floorY[1] : wallTopY + 2;        // surface the stair arrives at
    const nSteps = Math.ceil((yT - g) / rise);
    const len = nSteps * run;
    if (P.loft) {
      const Lin = ix1 - ix0;
      const ll = Math.max(m(1.6), Math.round(Lin * P.loftLength));
      loft = farLeft ? {a: ix0, b: Math.min(ix1, ix0 + ll)} : {a: Math.max(ix0, ix1 - ll), b: ix1};
    }
    // Stair runs toward the loft (arriving at its edge) or from the far gable.
    let dir, xs;                                          // dir: +1 rises toward +X
    if (loft) {
      if (farLeft) { dir = -1; xs = loft.b + len - 1; }   // first tread at the high-x end
      else { dir = 1; xs = loft.a - len; }
    } else if (farLeft) { dir = -1; xs = ix0 + m(0.3) + len - 1; }   // foot toward the middle,
    else { dir = 1; xs = ix1 - m(0.3) - len; }                         // top at the far gable
    const xa = dir > 0 ? xs : xs - len + 1, xb = xa + len;
    // Against the back wall, unless the only door asked for is a back door.
    const front = P.doorsBack > 0 && P.doorsFront === 0;
    stair = front ? {dir, xs, xa, xb, yT, nSteps, zside: 'front', za: iz1 - stairW, zb: iz1, zOpen: iz1 - stairW - 1}
                  : {dir, xs, xa, xb, yT, nSteps, zside: 'back', za: iz0, zb: iz0 + stairW, zOpen: iz0 + stairW};
    if (xa < ix0 || xb > ix1) { warnings.push('the stair does not fit: ' + (loft ? 'shorten the loft' : 'widen the house')); stair = null; if (loft) loft = null; }
  }
  // A partition may not cut the stair: move it clear (past the stair's far
  // end, or before its foot), and drop it if neither leaves a usable room.
  if (stair) {
    const clr = m(0.9);
    for (let k = partitions.length - 1; k >= 0; k--) {
      const pt = partitions[k];
      if (!(pt.x < stair.xb + clr && stair.xa - clr < pt.x + partW)) continue;
      const after = stair.xb + clr, before = stair.xa - clr - partW;
      if (ix1 - (after + partW) >= m(1.2)) pt.x = after;
      else if (before - ix0 >= m(1.2)) pt.x = before;
      else { partitions.splice(k, 1); warnings.push('a partition was dropped: it would cut the stair'); }
    }
    partitions.sort((a, b) => a.x - b.x);
  }
  // Drop a partition that would leave a room under 1.2 m.
  for (let k = partitions.length - 1; k >= 0; k--) {
    const lo = k ? partitions[k - 1].x + partW : ix0;
    const hi = k + 1 < partitions.length ? partitions[k + 1].x : ix1;
    if (partitions[k].x - lo < m(1.2) || hi - (partitions[k].x + partW) < m(1.2)) {
      partitions.splice(k, 1); warnings.push('a partition was dropped: its room would be under 1.2 m');
    }
  }

  // ---- plan: doors ------------------------------------------------------------
  // Doors sit on free stretches of wall: clear of the corners, of partitions
  // (whose ends meet the long walls) and of the stair (which blocks the inside).
  const doors = [];     // {wall, u0, key}
  const partBlock = (u, wl) => {
    if (wl !== WALLS.front && wl !== WALLS.back) return false;
    for (const pt of partitions) if (u + doorW + pw + 2 > pt.x - x0 && u - pw - 2 < pt.x - x0 + partW) return true;
    return false;
  };
  const stairBlock = (u, wl) => stair && ((wl === WALLS.back && stair.zside === 'back') || (wl === WALLS.front && stair.zside === 'front')) &&
      u + doorW + m(0.5) > stair.xa - x0 && u - m(0.5) < stair.xb - x0;
  const fitDoor = (wl, centre) => {
    const lo = t + pw + 2, hi = wl.L - t - pw - 2 - doorW;
    const ok = (u) => u >= lo && u <= hi && !partBlock(u, wl) && !stairBlock(u, wl) &&
        !doors.some(d => d.wall === wl && u < d.u0 + doorW + 2 * pw + 4 && d.u0 < u + doorW + 2 * pw + 4);
    const c0 = Math.round(centre - doorW / 2);
    for (let s = 0; s < wl.L; s++) for (const u of [c0 + s, c0 - s]) if (ok(u)) return u;
    return -1;
  };
  const sideGable = P.chimney === 'left' ? WALLS.right : WALLS.left;
  const planDoors = (warn) => {
    doors.length = 0;
    for (let k = 0; k < P.doorsFront; k++) {
      const wl = WALLS.front;
      const jit = (frand(seed, K_DOOR, k) - 0.5) * wl.L * 0.12;
      const u = fitDoor(wl, wl.L * (k + 1) / (P.doorsFront + 1) + jit);
      if (u < 0) warn('front door ' + k + ' does not fit'); else doors.push({wall: wl, u0: u, key: 'front_' + k});
    }
    if (P.doorsBack) {
      const wl = WALLS.back;
      // Cross passage: opposite the first front door.
      const f = doors.find(d => d.wall === WALLS.front);
      const u = fitDoor(wl, f ? f.u0 + doorW / 2 : wl.L * 0.5 + (frand(seed, K_DOORB) - 0.5) * wl.L * 0.2);
      if (u < 0) warn('back door does not fit'); else doors.push({wall: wl, u0: u, key: 'back_0'});
    }
    if (P.doorsSide) {
      const u = fitDoor(sideGable, sideGable.L / 2);
      if (u < 0) warn('gable door does not fit'); else doors.push({wall: sideGable, u0: u, key: sideGable.name + '_0'});
    }
  };
  {
    const w = [];
    planDoors((m1) => w.push(m1));
    // A house must have a way in. If the stair took the only wall a door
    // could use, the stair (and its loft) gives way, not the door.
    if (!doors.length && stair) {
      w.length = 0;
      warnings.push('the stair left no room for a door: no stair' + (loft ? ' and no loft' : '') + ' (widen the house)');
      stair = null; loft = null;
      planDoors((m1) => w.push(m1));
    }
    // ...and so do partitions, last first.
    while (!doors.length && partitions.length) {
      w.length = 0;
      partitions.pop();
      warnings.push('a partition was dropped to make room for a door');
      planDoors((m1) => w.push(m1));
    }
    warnings.push(...w);
  }
  if (!doors.length) warnings.push('NO DOOR fits this house');
  // Rooms on the ground floor, as x ranges [a, b).
  const rooms = [];
  {
    let a = ix0;
    for (const pt of partitions) { rooms.push({a, b: pt.x}); a = pt.x + partW; }
    rooms.push({a, b: ix1});
  }
  const roomOfX = (x) => { for (let k = 0; k < rooms.length; k++) if (x >= rooms[k].a && x < rooms[k].b) return k; return -1; };
  const hearthRoom = farLeft ? rooms.length - 1 : 0;

  for (const d of doors) d.wall.open.push({type: 'door', u0: d.u0, u1: d.u0 + doorW, y0: g, y1: g + doorH, storey: 0});

  // ---- plan: windows ----------------------------------------------------------
  const chimGable = P.chimney === 'left' ? WALLS.left : P.chimney === 'right' ? WALLS.right : null;
  const chimU0 = Math.round(D / 2 - m(1.2) / 2), chimU1 = chimU0 + m(1.2);
  const winClear = shutW + pw + 2;          // shutters need their width beside the frame
  const placeWindow = (wl, storey, centre) => {
    const fy = floorY[storey];
    const y0 = fy + sillH, y1 = y0 + winH;
    const lo = t + winClear, hi = wl.L - t - winClear - winW;
    const clash = (u) => wl.open.some(o => {
      // a window is only blocked by openings it overlaps vertically
      if (o.y1 + pw <= y0 - 2 || y1 + pw <= o.y0 - 2) return false;
      const gap = winClear + (o.type === 'door' ? pw + 1 : winClear);
      return u < o.u1 + gap && o.u0 < u + winW + gap;
    });
    const okU = (u) => u >= lo && u <= hi && !clash(u) && !(storey === 0 && partBlock(u, wl)) &&
        !(chimGable === wl && u + winW + winClear > chimU0 - 2 && u - winClear < chimU1 + 2);
    const c0 = Math.round(centre - winW / 2);
    for (let s = 0; s < wl.L / 2; s++) for (const u of [c0 + s, c0 - s]) if (okU(u)) {
      wl.open.push({type: 'window', u0: u, u1: u + winW, y0, y1, storey});
      return true;
    }
    return false;
  };
  let winSkipped = 0;
  for (let s = 0; s < nS; s++) {
    const nf = Math.ceil(P.windows / 2), nb = Math.floor(P.windows / 2);
    for (let k = 0; k < nf; k++) if (!placeWindow(WALLS.front, s, WALLS.front.L * (k + 0.5) / nf)) winSkipped++;
    for (let k = 0; k < nb; k++) if (!placeWindow(WALLS.back, s, WALLS.back.L * (k + 0.5) / nb)) winSkipped++;
    for (const gw of [WALLS.left, WALLS.right]) {
      if (gw === chimGable) continue;
      for (let k = 0; k < P.sideWindows; k++) if (!placeWindow(gw, s, gw.L * (k + 1) / (P.sideWindows + 1))) winSkipped++;
    }
  }
  if (winSkipped) warnings.push(winSkipped + ' window(s) did not fit clear of doors, partitions and corners');

  // ---- foundation + ground floor slab -----------------------------------------
  box(x0 - 1, 0, z0 - 1, x1 + 1, g, z1 + 1, CB);                    // footing, one proud
  box(ix0, g - 1, iz0, ix1, g, iz1, floorMat);                       // the slab

  // ---- walls ------------------------------------------------------------------
  const braceVariant = Math.floor(frand(seed, K_BRACE) * 3);          // see the brace pass
  for (const wl of Object.values(WALLS)) buildWall(wl);

  // The slab row between storeys belongs to the LOWER storey's plate band, so
  // the sill beam above it and the plate below it read as one floor band.
  function storeyOf(y) { for (let k = nS - 1; k >= 0; k--) if (y >= floorY[k]) return k; return 0; }

  function buildWall(wl) {
    const L = wl.L;
    const framed = !cobbleWall;
    // Posts per storey: corners, both sides of every opening in that storey,
    // then studs so no panel is wider than `panel`.
    const postsOf = [];
    for (let s = 0; s < nS; s++) {
      const posts = [[0, t], [L - t, L]];
      for (const o of wl.open) if (o.storey === s) { posts.push([o.u0 - pw, o.u0]); posts.push([o.u1, o.u1 + pw]); }
      posts.sort((a, b) => a[0] - b[0]);
      const merged = [];
      for (const p of posts) {
        if (merged.length && p[0] <= merged[merged.length - 1][1]) merged[merged.length - 1][1] = Math.max(merged[merged.length - 1][1], p[1]);
        else merged.push(p.slice());
      }
      const maxPanel = m(P.panel);
      const out = [];
      for (let i = 0; i < merged.length; i++) {
        out.push(merged[i]);
        if (i + 1 >= merged.length) break;
        const a = merged[i][1], b = merged[i + 1][0];
        const gap = b - a;
        if (gap > maxPanel) {
          const n = Math.ceil((gap + pw) / (maxPanel + pw)) - 1;
          for (let k = 1; k <= n; k++) {
            const c = Math.round(a + (gap * k) / (n + 1) - pw / 2);
            out.push([c, c + pw]);
          }
        }
      }
      out.sort((a, b) => a[0] - b[0]);
      postsOf.push(out);
    }
    const isPost = (s, u) => postsOf[s].some(p => u >= p[0] && u < p[1]);
    // The mid rail runs at window-sill height, unless the plinth and sill
    // beam leave less than 0.4 m of panel under it: a one-row strip of daub
    // between two beams reads as a mistake, so the lower panel runs full height.
    const hasMid = (s) => (floorY[s] + sillH - pw) - ((s === 0 ? g + plinthH : floorY[s]) + pw) >= m(0.4);

    for (let y = g; y <= wallTopY; y++) {
      const s = storeyOf(y);
      const fy = floorY[s];
      const topS = s === nS - 1 ? wallTopY : floorY[s + 1] - 1;   // the slab row belongs to the lower band
      const baseS = s === 0 ? g + plinthH : fy;
      for (let u = 0; u < L; u++) {
        const corner = u < t || u >= L - t;
        for (let w = 0; w < t; w++) {
          let v;
          if (y < g + plinthH && !cobbleWall) v = CB;
          else if (cobbleWall) {
            // Dressed quoins: alternating long and short blocks at each corner.
            const blk = Math.floor((y - g) / 3);
            const ql = blk % 2 === 0 ? t + m(0.3) : t + m(0.1);
            v = (u < ql || u >= L - ql) ? FS : CB;
            if (y >= wallTopY - pw + 1) v = TI;                   // wall plate
            if (nS > 1 && y >= floorY[1] - 2 && y < floorY[1] && !corner) v = TI;   // floor band
          } else {
            const rail = (y >= baseS && y < baseS + pw) ||            // sill beam
                         (y > topS - pw && y <= topS) ||              // plate / floor band
                         (hasMid(s) && y >= fy + sillH - pw && y < fy + sillH);   // mid rail
            if (corner || isPost(s, u) || rail) v = TI;
            else {
              const belowMid = y < fy + sillH - pw;
              const boards = P.wallStyle === 'plank' || (P.wallStyle === 'plank_daub' && belowMid);
              if (boards) v = (w === 0 && (y - fy) % 3 === 2) ? 0 : PL;   // weatherboard shadow lines
              else v = w === 0 ? 0 : DB;                                   // daub set back one voxel
            }
          }
          wset(wl, u, w, y, v);
        }
      }
    }
    // plinth course one proud of the face (framed walls), footing for cobble
    if (plinthH > 0) for (let u = -1; u <= L; u++) for (let y = g; y < g + plinthH; y++) wset(wl, u, -1, y, CB);

    // braces in the corner panels of each storey
    if (framed && P.braces && P.wallStyle !== 'plank') {
      for (let s = 0; s < nS; s++) {
        const fy = floorY[s];
        const topS = s === nS - 1 ? wallTopY : floorY[s + 1] - 1;
        const baseS = s === 0 ? g + plinthH : fy;
        const midHi = fy + sillH;
        const posts = postsOf[s];
        const panels = [];
        for (let i = 0; i + 1 < posts.length; i++) panels.push([posts[i][1], posts[i + 1][0]]);
        if (!panels.length) continue;
        const ends = [[panels[0], 0], [panels[panels.length - 1], 1]];
        for (const [pn, side] of ends) {
          const [a, b] = pn;
          if (b - a < m(0.5)) continue;
          if (wl.open.some(o => o.storey === s && o.u0 < b + pw && a - pw < o.u1)) continue;
          // Variant 0: a down-brace (corner high -> inner low) above the mid
          // rail; 1: the same, full storey height; 2: an up-brace above the
          // mid rail. Without a mid rail every variant runs full height.
          let yLo = hasMid(s) && braceVariant !== 1 ? midHi : baseS + pw;
          const yHi = topS - pw + 1;
          if (P.wallStyle === 'plank_daub') yLo = midHi;
          if (yHi - yLo < 4) continue;
          const up = braceVariant === 2;
          const uHi = (side === 0) !== up ? a : b - 1, uLo = (side === 0) !== up ? b - 1 : a;
          const n = Math.max(Math.abs(uLo - uHi), yHi - 1 - yLo);
          for (let i = 0; i <= n; i++) {
            const uu = Math.round(uHi + (uLo - uHi) * i / n);
            const yy = Math.round(yHi - 1 - (yHi - 1 - yLo) * i / n);
            for (let du = 0; du < pw; du++) {
              const u2 = uLo > uHi ? uu + du : uu - du;
              if (u2 < a || u2 >= b) continue;
              for (let w = 0; w < t; w++) wset(wl, u2, w, yy, TI);
            }
          }
        }
      }
    }

    // openings: holes, leaves, frames, lintels, sills, shutters
    for (const o of wl.open) {
      const jamb = cobbleWall ? 1 : pw;
      if (o.type === 'door') {
        for (let y = o.y0; y < o.y1; y++) for (let u = o.u0; u < o.u1; u++) for (let w = 0; w < t; w++)
          wset(wl, u, w, y, w === t - 1 ? DW : 0);
        for (let y = o.y0; y < o.y1 + pw; y++) for (let w = 0; w < t; w++) {
          for (let j = 1; j <= jamb; j++) { wset(wl, o.u0 - j, w, y, TI); wset(wl, o.u1 - 1 + j, w, y, TI); }
        }
        for (let y = o.y1; y < o.y1 + pw; y++) for (let u = o.u0 - jamb - (cobbleWall ? 1 : 0); u < o.u1 + jamb + (cobbleWall ? 1 : 0); u++)
          for (let w = 0; w < t; w++) wset(wl, u, w, y, TI);
        // threshold board, and the plinth course stops at the door
        for (let u = o.u0; u < o.u1; u++) { for (let w = 0; w < t; w++) wset(wl, u, w, g - 1, TI); for (let y = g; y < g + plinthH; y++) wset(wl, u, -1, y, 0); }
      } else {
        const glass = P.windowStyle === 'glass';
        const midW = Math.floor(t / 2);
        for (let y = o.y0; y < o.y1; y++) for (let u = o.u0; u < o.u1; u++) for (let w = 0; w < t; w++)
          wset(wl, u, w, y, glass && w === midW ? GL : 0);
        if (glass) {                                  // cross mullion
          const uc = o.u0 + Math.floor(winW / 2), yc = o.y0 + Math.floor(winH / 2);
          for (let y = o.y0; y < o.y1; y++) wset(wl, uc, midW, y, TI);
          for (let u = o.u0; u < o.u1; u++) wset(wl, u, midW, yc, TI);
        }
        // sill (projecting one) and lintel, one wider than the hole each side
        for (let u = o.u0 - 1; u <= o.u1; u++) {
          for (let w = -1; w < t; w++) wset(wl, u, w, o.y0 - 1, TI);
          for (let w = 0; w < t; w++) wset(wl, u, w, o.y1, TI);
          if (cobbleWall) for (let w = 0; w < t; w++) wset(wl, u, w, o.y1 + 1, TI);
        }
        if (cobbleWall) for (let y = o.y0; y < o.y1; y++) for (let w = 0; w < t; w++) { wset(wl, o.u0 - 1, w, y, TI); wset(wl, o.u1, w, y, TI); }
        if (!glass) {                                 // shutters folded back flat, beside the frame
          const off = cobbleWall ? 1 : pw;
          for (let y = o.y0; y < o.y1; y++) for (let i = 0; i < shutW; i++) {
            wset(wl, o.u0 - 1 - off - i, -1, y, PL);
            wset(wl, o.u1 + off + i, -1, y, PL);
          }
        }
      }
    }
  }

  // ---- floors: upper storey slab on joists, loft --------------------------------
  const joistStep = m(0.5);
  for (let s = 1; s < nS; s++) {
    const sy = floorY[s] - 1;
    box(ix0, sy, iz0, ix1, sy + 1, iz1, PL);
    for (let x = ix0 + 2; x < ix1 - 1; x += joistStep) box(x, sy - 1, iz0, x + 1, sy, iz1, TI);
  }
  if (loft) {
    box(loft.a, wallTopY + 1, iz0, loft.b, wallTopY + 2, iz1, PL);
    for (let x = loft.a + 2; x < loft.b - 1; x += joistStep) box(x, wallTopY, iz0, x + 1, wallTopY + 1, iz1, TI);
    // an edge beam under the open side of the loft
    const ex = loft.a === ix0 ? loft.b - pw : loft.a;
    box(ex, wallTopY - 1, iz0, ex + pw, wallTopY + 1, iz1, TI);
  }

  // ---- partitions (ground floor): plank boarding, framed open doorway ----------
  for (let k = 0; k < partitions.length; k++) {
    const pt = partitions[k];
    const top = nS > 1 ? floorY[1] - 2 : wallTopY;
    // doorway near the front or the back, seeded; never behind the stair
    let atFront = frand(seed, K_PART, 10 + k) < 0.5;
    if (stair && pt.x + partW > stair.xa - m(0.9) && pt.x < stair.xb + m(0.9)) atFront = stair.zside === 'back';
    const dz0 = atFront ? iz1 - m(0.3) - doorW : iz0 + m(0.3);
    pt.dz0 = dz0;
    for (let y = g; y <= top; y++) for (let z = iz0; z < iz1; z++) for (let x = pt.x; x < pt.x + partW; x++) {
      const inDoor = z >= dz0 && z < dz0 + doorW && y < g + doorH;
      const frame = (z >= dz0 - 1 && z < dz0 + doorW + 1 && y < g + doorH + pw) || z === iz0 || z === iz1 - 1;
      set(x, y, z, inDoor ? 0 : frame ? TI : ((z - iz0) % 3 === 2 && x === pt.x ? 0 : PL));
    }
    for (let z = dz0; z < dz0 + doorW; z++) for (let x = pt.x; x < pt.x + partW; x++) set(x, g - 1, z, TI);
  }

  // ---- roof -------------------------------------------------------------------
  // Underside height at inward index i (0 = the column just inside the outer
  // face; negative = overhang): wall plate + floor((i + 0.5) * q / 4). Integer
  // arithmetic, so the rhythm is exact.
  const yU = (i) => wallTopY + 1 + Math.floor(((2 * i + 1) * q) / 8);
  const inwardZ = (z) => Math.min(z - z0, z1 - 1 - z);
  const inwardX = (x) => Math.min(x - x0, x1 - 1 - x);
  const hip = P.roofShape === 'hip';
  const inwardAt = (x, z) => hip ? Math.min(inwardZ(z), inwardX(x)) : inwardZ(z);
  const iMaxZ = Math.floor((D - 1) / 2);
  const rx0 = hip ? x0 - O : x0 - Ov, rx1 = hip ? x1 + O : x1 + Ov;
  const rz0 = z0 - O, rz1 = z1 + O;
  const thatch = P.roofMaterial === 'thatch';
  const roofMat = thatch ? TH : RT;
  const ridgeW = m(0.3);

  // gable infill first (under the roof), per style
  if (!hip) {
    for (const wl of [WALLS.left, WALLS.right]) {
      for (let u = 0; u < wl.L; u++) {
        const z = wz(wl, u, 0);
        const top = yU(inwardZ(z));
        const uc = Math.floor(wl.L / 2);
        const stud = cobbleWall ? false : (Math.abs(u - uc) % m(0.6) < pw);
        for (let y = wallTopY + 1; y < top; y++) for (let w = 0; w < t; w++) {
          let v;
          if (cobbleWall) v = CB;
          else if (P.wallStyle === 'plank') v = (w === 0 && u % 3 === 2) ? 0 : PL;
          else v = stud ? TI : (w === 0 ? 0 : DB);
          wset(wl, u, w, y, v);
        }
      }
    }
  }

  // trusses: tie beam at the plate, principal rafters under the covering,
  // a king post on the ridge line. Visible from inside (and from a loft).
  {
    const span = ix1 - ix0;
    const nb = Math.max(1, Math.round(span / m(2.4)));
    for (let k = 1; k < nb; k++) {
      const xb = ix0 + Math.round(span * k / nb) - Math.floor(pw / 2);
      if (partitions.some(pt => xb < pt.x + partW + 1 && pt.x < xb + pw + 1)) continue;
      for (let x = xb; x < xb + pw; x++) {
        for (let z = iz0; z < iz1; z++) {
          set(x, wallTopY, z, TI);
          if (!hip || inwardX(x) > inwardZ(z)) for (let dy = 1; dy <= pw; dy++) set(x, yU(inwardZ(z)) - dy, z, TI);
        }
        if (!hip || inwardX(x) >= iMaxZ) {
          const zc = z0 + Math.floor(D / 2) - Math.floor(pw / 2);
          for (let z = zc; z < zc + pw; z++) for (let y = wallTopY + 1; y < yU(iMaxZ) - pw; y++) set(x, y, z, TI);
        }
      }
    }
  }

  // the covering
  for (let z = rz0; z < rz1; z++) for (let x = rx0; x < rx1; x++) {
    const i = inwardAt(x, z);
    const iz = inwardZ(z);
    const yb = yU(i);
    let yt = yb + Tv;
    // thatch ridge roll: raised band along the ridge with a scalloped skirt
    if (thatch) {
      const onRidgeLine = !hip || inwardX(x) > iz || iz >= iMaxZ - ridgeW;
      if (onRidgeLine && i >= iMaxZ - ridgeW) yt += 2;
      else if (onRidgeLine && i >= iMaxZ - ridgeW - 2 && (((x - x0) % 6) + 6) % 6 < 3) yt += 1;
    } else {
      if (i >= iMaxZ) yt += 1;                                  // ridge tiles
      else if (((i % 3) + 3) % 3 === 2) yt += 1;                 // tile course lip
    }
    for (let y = yb; y < yt; y++) set(x, y, z, roofMat);
    // fascia along the eaves (tile: on the edge; thatch: tucked under, one in)
    const eaveEdge = thatch ? (z === rz0 + 1 || z === rz1 - 2) : (z === rz0 || z === rz1 - 1);
    const hipEdge = hip && (thatch ? (x === rx0 + 1 || x === rx1 - 2) : (x === rx0 || x === rx1 - 1));
    if (O > 0 && (eaveEdge || hipEdge) && i < 0) { set(x, yb - 1, z, TI); if (!thatch) set(x, yb, z, TI); }
    // bargeboards on the gable verges
    if (!hip && (x === rx0 || x === rx1 - 1)) { set(x, yb, z, TI); set(x, yb - 1, z, TI); }
  }

  // ---- chimney + fireplace, or an open central hearth ----------------------------
  let hearthSlot = null;
  const hearthReserve = [];     // [xa, za, xb, zb] floor areas kept clear
  if (chimGable) {
    const sgn = P.chimney === 'left' ? -1 : 1;
    const zc0 = z0 + chimU0, zc1 = z0 + chimU1;
    const xa = P.chimney === 'left' ? x0 - chimProt : x1 - t;
    const xb = P.chimney === 'left' ? x0 + t : x1 + chimProt;
    const apex = yU(iMaxZ) + Tv + (thatch ? 2 : 1);
    const top = apex + m(0.8);
    box(xa, 0, zc0, xb, top, zc1, CB);
    box(xa - 1, top, zc0 - 1, xb + 1, top + 2, zc1 + 1, FS);   // cap
    // flue: open to the sky, down to the fireplace
    const fx0 = P.chimney === 'left' ? x0 - chimProt + 2 : x1 + 1, fx1 = P.chimney === 'left' ? x0 - 1 : x1 + chimProt - 2;
    const fireH = m(0.9), fireW = m(1.0);
    const fz0 = z0 + Math.round(D / 2 - fireW / 2), fz1 = fz0 + fireW;
    box(fx0, g + fireH, fz0 + 2, fx1, top + 2, fz1 - 2, 0);
    // fireplace opening through the wall into the stack
    const fa = P.chimney === 'left' ? x0 - chimProt + 2 : x1 - t, fb = P.chimney === 'left' ? x0 + t : x1 + chimProt - 2;
    box(fa, g, fz0, fb, g + fireH, fz1, 0);
    box(fa, g - 1, fz0, fb, g, fz1, FS);                          // fire bed
    // bressumer: a timber beam over the opening, on the inside face
    const ba = P.chimney === 'left' ? x0 + t - 2 : x1 - t, bb = ba + 2;
    box(ba, g + fireH, fz0 - 2, bb, g + fireH + pw, fz1 + 2, TI);
    // flagstone hearth apron in front
    const ha = P.chimney === 'left' ? ix0 : ix1 - m(0.6), hb = ha + m(0.6);
    box(ha, g - 1, fz0 - 1, hb, g, fz1 + 1, FS);
    const hx = P.chimney === 'left' ? ix0 + m(0.8) : ix1 - m(0.8) - 1;
    hearthSlot = {pos: [hx, g, z0 + Math.floor(D / 2)], yaw: P.chimney === 'left' ? 270 : 90};
    hearthReserve.push([Math.min(hx, sgn < 0 ? ix0 : hx) - 2, fz0 - 3, Math.max(hx + 3, sgn > 0 ? ix1 : hx + 3), fz1 + 3]);
  } else {
    const r = rooms[hearthRoom];
    const hs = m(1.2);
    const hx0 = Math.round((r.a + r.b) / 2 - hs / 2), hz0 = Math.round((iz0 + iz1) / 2 - hs / 2);
    box(hx0, g - 1, hz0, hx0 + hs, g, hz0 + hs, FS);
    for (let x = hx0; x < hx0 + hs; x++) for (let z = hz0; z < hz0 + hs; z++)
      if (x === hx0 || x === hx0 + hs - 1 || z === hz0 || z === hz0 + hs - 1) set(x, g, z, CB);
    hearthSlot = {pos: [hx0 + Math.floor(hs / 2), g, hz0 + hs + m(0.4)], yaw: 180};
    hearthReserve.push([hx0 - m(0.6), hz0 - m(0.6), hx0 + hs + m(0.6), hz0 + hs + m(0.6)]);
  }

  // ---- stair ------------------------------------------------------------------
  if (stair) {
    const {dir, xs, yT, nSteps, za, zb, zOpen} = stair;
    // Clear the stairwell: slab, joists and tie beams over the footprint (and
    // one run beyond each end), so nothing hangs at head height over a tread.
    for (let x = stair.xa - run; x < stair.xb + run; x++) for (let z = za; z < zb; z++)
      for (let y = g; y < yT; y++) {
        if (x < ix0 || x >= ix1) continue;
        const v = get(x, y, z);
        const onFar = (x < stair.xa || x >= stair.xb);
        if ((v === PL || v === TI) && !(onFar && y >= yT - 1)) set(x, y, z, 0);
      }
    for (let k = 0; k < nSteps; k++) {
      const topRow = yT - 1 - rise * (nSteps - 1 - k);
      for (let r = 0; r < run; r++) {
        const x = xs + dir * (k * run + r);
        for (let z = za; z < zb; z++) set(x, topRow, z, PL);
        // stringer on the open side, two rows deep
        set(x, topRow - 1, zOpen, TI); set(x, topRow, zOpen, TI);
        if (topRow - 2 >= g) set(x, topRow - 2, zOpen, TI);
      }
    }
    // newel post at the foot
    const nx = xs - dir;
    for (let y = g; y < g + m(1.0); y++) set(nx, y, zOpen, TI);
  }

  // ---- furniture: beds, chests ---------------------------------------------------
  // A 2D reservation map per floor level keeps furniture out of door swings,
  // the stair and its landing, the hearth, and partition doorways.
  const levels = [];      // {fy, area:[xa,za,xb,zb][], rooms, name}
  if (nS > 1) levels.push({fy: floorY[1], name: 'upper', areas: [[ix0, iz0, ix1, iz1]]});
  if (loft) levels.push({fy: wallTopY + 2, name: 'loft', areas: [[loft.a, iz0, loft.b, iz1]]});
  const ground = {fy: g, name: 'ground', areas: []};
  // ground rooms, hearth room last so beds prefer the other rooms
  const order = rooms.map((r, k) => k).sort((a, b) => (a === hearthRoom) - (b === hearthRoom) || a - b);
  for (const k of order) ground.areas.push([rooms[k].a, iz0, rooms[k].b, iz1, k]);
  levels.push(ground);

  const reserved = new Map();   // fy -> Uint8Array(NX*NZ)
  const res = (fy) => { if (!reserved.has(fy)) reserved.set(fy, new Uint8Array(NX * NZ)); return reserved.get(fy); };
  const mark = (fy, xa, za, xb, zb) => {
    const r = res(fy);
    for (let z = Math.max(0, za); z < Math.min(NZ, zb); z++) for (let x = Math.max(0, xa); x < Math.min(NX, xb); x++) r[z * NX + x] = 1;
  };
  // door swings (quarter disc of the leaf width, inside) and a passage beyond
  for (const d of doors) {
    const wl = d.wall;
    for (let u = d.u0 - m(0.3); u < d.u0 + doorW + m(0.3); u++) for (let w = t; w < t + doorW + m(0.5); w++) {
      const x = wx(wl, u, w), z = wz(wl, u, w);
      mark(g, x, z, x + 1, z + 1);
    }
  }
  if (stair) {
    mark(g, stair.xa - m(0.8), stair.za - m(0.8), stair.xb + m(0.8), stair.zb + m(0.8));
    mark(stair.yT, stair.xa - m(0.8), stair.za - m(0.8), stair.xb + m(0.8), stair.zb + m(0.8));
  }
  for (const r of hearthReserve) mark(g, r[0], r[1], r[2], r[3]);
  for (const pt of partitions) mark(g, pt.x - m(0.7), pt.dz0 - 2, pt.x + partW + m(0.7), pt.dz0 + doorW + 2);

  const bedL = m(1.9), bedW = m(0.9), headroom = m(1.0);
  const bedTopOff = 4;                         // mattress top = fy + 4 rows
  const beds = [];
  const chests = [];
  const freeFoot = (fy, xa, za, xb, zb, area, hMin, hMax) => {
    if (xa < area[0] || za < area[1] || xb > area[2] || zb > area[3]) return false;
    const r = res(fy);
    for (let z = za; z < zb; z++) for (let x = xa; x < xb; x++) {
      if (r[z * NX + x]) return false;
      if (get(x, fy - 1, z) === 0) return false;                    // must stand on something
      for (let y = fy + hMin; y < fy + hMax; y++) if (get(x, y, z)) return false;
    }
    return true;
  };
  const scanDir = frand(seed, K_BED) < 0.5 ? 1 : -1;
  function placeBeds(count) {
    for (const lv of levels) {
      for (const area of lv.areas) {
        // candidate orientations: head to back wall, head to front wall, along the ridge
        const cands = [];
        const xsR = [];
        for (let x = area[0] + 1; x + bedW <= area[2] - 1; x++) xsR.push(x);
        if (scanDir < 0) xsR.reverse();
        for (const x of xsR) cands.push({xa: x, za: area[1], xb: x + bedW, zb: area[1] + bedL, yaw: 180});
        for (const x of xsR) cands.push({xa: x, za: area[3] - bedL, xb: x + bedW, zb: area[3], yaw: 0});
        const zc = Math.round((iz0 + iz1) / 2 - bedW / 2);
        for (let x = area[0] + 1; x + bedL <= area[2] - 1; x++) cands.push({xa: x, za: zc, xb: x + bedL, zb: zc + bedW, yaw: 270});
        for (const c of cands) {
          if (beds.length >= count) return;
          if (!freeFoot(lv.fy, c.xa, c.za, c.xb, c.zb, area, 0, bedTopOff + headroom)) continue;
          beds.push({...c, fy: lv.fy, level: lv.name});
          // 0.4 m of walking room along the long sides; one cell at head and
          // foot, so a chest can stand at the foot.
          if (c.yaw === 270) mark(lv.fy, c.xa - 1, c.za - m(0.4), c.xb + 1, c.zb + m(0.4));
          else mark(lv.fy, c.xa - m(0.4), c.za - 1, c.xb + m(0.4), c.zb + 1);
        }
      }
    }
  }
  placeBeds(P.beds);
  if (beds.length < P.beds) warnings.push('only ' + beds.length + ' of ' + P.beds + ' beds found a spot with 1 m of headroom');
  for (const b of beds) buildBed(b);

  function buildBed(b) {
    const {xa, za, xb, zb, fy, yaw} = b;
    // head end: yaw is the heading from foot to head
    const headAt = (x, z) => yaw === 180 ? z === za : yaw === 0 ? z === zb - 1 : x === xa;
    for (let z = za; z < zb; z++) for (let x = xa; x < xb; x++) {
      const edge = x === xa || x === xb - 1 || z === za || z === zb - 1;
      const cornerP = (x === xa || x === xb - 1) && (z === za || z === zb - 1);
      if (cornerP) for (let y = fy; y < fy + 3; y++) set(x, y, z, TI);
      if (edge) set(x, fy + 2, z, TI);
      else { set(x, fy + 2, z, SB); }
      set(x, fy + 3, z, SB);
      if (headAt(x, z)) for (let y = fy + 1; y < fy + 6; y++) set(x, y, z, (x === xa || x === xb - 1 || z === za || z === zb - 1) && cornerP ? TI : PL);
    }
  }

  // chests: at the foot of each bed first, then along ground-floor walls
  const chW = m(0.8), chD = m(0.5), chH = m(0.5);
  function placeChests(count) {
    for (const b of beds) {
      if (chests.length >= count) return;
      const area = [ix0, iz0, ix1, iz1];
      let c;
      if (b.yaw === 180) { const x = Math.round((b.xa + b.xb) / 2 - chW / 2); c = {xa: x, za: b.zb + 1, xb: x + chW, zb: b.zb + 1 + chD, yaw: 0}; }
      else if (b.yaw === 0) { const x = Math.round((b.xa + b.xb) / 2 - chW / 2); c = {xa: x, za: b.za - 1 - chD, xb: x + chW, zb: b.za - 1, yaw: 180}; }
      else { const z = Math.round((b.za + b.zb) / 2 - chW / 2); c = {xa: b.xb + 1, za: z, xb: b.xb + 1 + chD, zb: z + chW, yaw: 90}; }
      if (freeFoot(b.fy, c.xa, c.za, c.xb, c.zb, area, 0, chH + m(0.5))) { chests.push({...c, fy: b.fy}); mark(b.fy, c.xa - 2, c.za - 2, c.xb + 2, c.zb + m(0.6)); }
    }
    for (const area of ground.areas) {
      for (const back of [true, false]) {
        const xsR = [];
        for (let x = area[0] + 1; x + chW <= area[2] - 1; x++) xsR.push(x);
        if (scanDir > 0) xsR.reverse();
        for (const x of xsR) {
          if (chests.length >= count) return;
          const c = back ? {xa: x, za: area[1], xb: x + chW, zb: area[1] + chD, yaw: 0}
                         : {xa: x, za: area[3] - chD, xb: x + chW, zb: area[3], yaw: 180};
          if (!freeFoot(g, c.xa, c.za, c.xb, c.zb, area, 0, chH + m(0.5))) continue;
          chests.push({...c, fy: g});
          mark(g, c.xa - m(0.3), c.za - m(0.3), c.xb + m(0.3), c.zb + m(0.6));
        }
      }
    }
  }
  placeChests(P.chests);
  if (chests.length < P.chests) warnings.push('only ' + chests.length + ' of ' + P.chests + ' chests found a spot');
  for (const c of chests) {
    for (let z = c.za; z < c.zb; z++) for (let x = c.xa; x < c.xb; x++) for (let y = c.fy; y < c.fy + chH; y++) {
      const ex = (x === c.xa || x === c.xb - 1), ez = (z === c.za || z === c.zb - 1), top = y === c.fy + chH - 1;
      set(x, y, z, (ex && ez) || (top && (ex || ez)) ? TI : PL);
    }
  }

  // ---- slots --------------------------------------------------------------------
  const slots = [];
  const add = (name, kind, pos, yaw, props) => slots.push({name, kind, pos: pos.map(v => v | 0), yaw: ((yaw % 360) + 360) % 360, props: props || {}});
  const way = new Map();     // name -> Set(links)
  const link = (a, b) => { if (!way.has(a)) way.set(a, new Set()); if (!way.has(b)) way.set(b, new Set()); way.get(a).add(b); way.get(b).add(a); };
  const wayPos = new Map();

  // rooms first, so door links can name them
  rooms.forEach((r, k) => {
    const n = 'waynode_room_' + k;
    wayPos.set(n, [Math.floor((r.a + r.b) / 2), g, Math.floor((iz0 + iz1) / 2)]);
  });

  for (const d of doors) {
    const wl = d.wall;
    const name = 'door_' + d.key;
    const uMid = d.u0 + Math.floor(doorW / 2);
    // leaf box (inclusive), in the wall's inner layer
    const lx0 = wx(wl, d.u0, t - 1), lz0 = wz(wl, d.u0, t - 1);
    const lx1 = wx(wl, d.u0 + doorW - 1, t - 1), lz1 = wz(wl, d.u0 + doorW - 1, t - 1);
    const leafMin = [Math.min(lx0, lx1), g, Math.min(lz0, lz1)];
    const leafMax = [Math.max(lx0, lx1), g + doorH - 1, Math.max(lz0, lz1)];
    // Hinge on the side nearer a corner, so the open leaf lies back against
    // the wall rather than across the room.
    const hingeLow = d.u0 < wl.L - (d.u0 + doorW);
    const hu = hingeLow ? d.u0 : d.u0 + doorW;        // cell BOUNDARY along u
    // The hinge line is the vertical edge on the INNER face of the leaf.
    const hx = wl.cx + wl.du[0] * hu + wl.dw[0] * t + (wl.dw[0] < 0 ? 1 : 0) + (wl.du[0] < 0 ? 1 : 0);
    const hz = wl.cz + wl.du[1] * hu + wl.dw[1] * t + (wl.dw[1] < 0 ? 1 : 0) + (wl.du[1] < 0 ? 1 : 0);
    // left / right AS SEEN FROM OUTSIDE: the viewer faces heading+180, and
    // their right hand points to heading+180+90 = heading-90.
    const rh = ((wl.heading - 90) % 360 + 360) % 360;
    const rightV = [Math.round(Math.sin(rh * Math.PI / 180)), Math.round(Math.cos(rh * Math.PI / 180))];
    const alongLow = [-wl.du[0], -wl.du[1]];          // direction toward the low-u end
    const lowIsRight = alongLow[0] === rightV[0] && alongLow[1] === rightV[1];
    const hinge = (hingeLow === lowIsRight) ? 'right' : 'left';
    const pos = [wx(wl, uMid, t - 1), g, wz(wl, uMid, t - 1)];
    add(name, 'door', pos, wl.heading, {
      leaf: {min: leafMin, max: leafMax}, hinge, hingeLine: [hx, hz],
      opens: 'in', openAngle: 95, wall: wl.name
    });
    const outN = 'waynode_' + d.key + '_out', inN = 'waynode_' + d.key + '_in';
    wayPos.set(outN, [wx(wl, uMid, -m(0.8)), g, wz(wl, uMid, -m(0.8))]);
    wayPos.set(inN, [wx(wl, uMid, t + m(0.8)), g, wz(wl, uMid, t + m(0.8))]);
    link(outN, inN);
    const ip = wayPos.get(inN);
    const rk = roomOfX(ip[0]);
    if (rk >= 0) link(inN, 'waynode_room_' + rk);
  }
  partitions.forEach((pt, k) => {
    const n = 'waynode_partition_' + k;
    wayPos.set(n, [pt.x + Math.floor(partW / 2), g, pt.dz0 + Math.floor(doorW / 2)]);
    link(n, 'waynode_room_' + k); link(n, 'waynode_room_' + (k + 1));
  });
  if (stair) {
    const b = 'waynode_stair_bottom', tp = 'waynode_stair_top';
    const zc = Math.floor((stair.za + stair.zb) / 2);
    // One step off the open side, for a node that does not fit beyond an end.
    const zSide = stair.zside === 'back' ? stair.zb + m(0.5) : stair.za - m(0.5) - 1;
    const bx = stair.xs - stair.dir * m(0.6);
    wayPos.set(b, bx >= ix0 && bx < ix1 ? [bx, g, zc] : [stair.xs, g, zSide]);
    const lastX = stair.xs + stair.dir * (stair.nSteps * run - 1);
    // Off the top: onward onto the loft (the stair arrives at its edge), or
    // sideways off the open side onto the upper floor (the stair arrives at
    // the gable wall).
    wayPos.set(tp, loft ? [lastX + stair.dir * m(0.6), stair.yT, zc] : [lastX - stair.dir * m(0.3), stair.yT, zSide]);
    link(b, tp);
    const rk = roomOfX(stair.xs);
    if (rk >= 0) link(b, 'waynode_room_' + rk);
    const up = loft ? 'waynode_loft' : 'waynode_upper';
    const ua = loft ? [Math.floor((loft.a + loft.b) / 2), stair.yT, Math.floor((iz0 + iz1) / 2)]
                    : [Math.floor((ix0 + ix1) / 2), stair.yT, Math.floor((iz0 + iz1) / 2) + m(0.6)];
    wayPos.set(up, ua);
    link(tp, up);
  }
  // single room with no partition and no doors still needs its node listed
  rooms.forEach((r, k) => { if (!way.has('waynode_room_' + k)) way.set('waynode_room_' + k, new Set()); });

  if (hearthSlot) add('hearth', 'marker', hearthSlot.pos, hearthSlot.yaw, {tags: ['hearth']});
  rooms.forEach((r, k) => {
    if (k === hearthRoom && hearthSlot) return;
    add('work_' + k, 'marker', [Math.floor((r.a + r.b) / 2), g, iz1 - m(1.0)], 0, {tags: ['work']});
  });
  beds.forEach((b, k) => {
    const cx = Math.floor((b.xa + b.xb) / 2), cz = Math.floor((b.za + b.zb) / 2);
    add('bed_' + k, 'bed', [cx, b.fy + bedTopOff, cz], b.yaw, {
      box: {min: [b.xa, b.fy, b.za], max: [b.xb - 1, b.fy + bedTopOff - 1, b.zb - 1]}, level: b.level});
  });
  chests.forEach((c, k) => {
    add('chest_' + k, 'container', [Math.floor((c.xa + c.xb) / 2), c.fy, Math.floor((c.za + c.zb) / 2)], c.yaw, {
      box: {min: [c.xa, c.fy, c.za], max: [c.xb - 1, c.fy + chH - 1, c.zb - 1]}, items: []});
  });
  // waynodes last, in a stable (sorted) order
  for (const n of [...way.keys()].sort()) {
    add(n, 'waynode', wayPos.get(n), 0, {links: [...way.get(n)].sort()});
  }

  // ---- crop to the occupied box (what voxload.cpp will rebase to) -----------------
  let mnx = NX, mny = NY, mnz = NZ, mxx = -1, mxy = -1, mxz = -1;
  for (let z = 0; z < NZ; z++) for (let y = 0; y < NY; y++) {
    const row = (z * NY + y) * NX;
    for (let x = 0; x < NX; x++) if (cells[row + x]) {
      if (x < mnx) mnx = x; if (x > mxx) mxx = x;
      if (y < mny) mny = y; if (y > mxy) mxy = y;
      if (z < mnz) mnz = z; if (z > mxz) mxz = z;
    }
  }
  const nx = mxx - mnx + 1, ny = mxy - mny + 1, nz = mxz - mnz + 1;
  const out = new Uint8Array(nx * ny * nz);
  const counts = {};
  for (let z = 0; z < nz; z++) for (let y = 0; y < ny; y++) for (let x = 0; x < nx; x++) {
    const v = cells[idx(x + mnx, y + mny, z + mnz)];
    out[(z * ny + y) * nx + x] = v;
    if (v) counts[PALETTE[v - 1]] = (counts[PALETTE[v - 1]] || 0) + 1;
  }
  const sh = (p) => [p[0] - mnx, p[1] - mny, p[2] - mnz];
  for (const s of slots) {
    s.pos = sh(s.pos);
    if (s.props.leaf) s.props.leaf = {min: sh(s.props.leaf.min), max: sh(s.props.leaf.max)};
    if (s.props.box) s.props.box = {min: sh(s.props.box.min), max: sh(s.props.box.max)};
    if (s.props.hingeLine) s.props.hingeLine = [s.props.hingeLine[0] - mnx, s.props.hingeLine[1] - mnz];
  }
  const origin = [Math.floor((x0 + x1) / 2) - mnx, g - mny, Math.floor((z0 + z1) / 2) - mnz];
  const footprint = {min: [x0 - mnx, z0 - mnz], max: [x1 - 1 - mnx, z1 - 1 - mnz]};
  const clipped = nx > MAX_DIM || ny > MAX_DIM || nz > MAX_DIM;
  if (clipped) warnings.push('over ' + MAX_DIM + ' voxels on an axis (' + nx + 'x' + ny + 'x' + nz + '): a .vox cannot hold it');
  let total = 0;
  for (const k in counts) total += counts[k];

  return {
    dim: {x: nx, y: ny, z: nz}, cells: out, names: PALETTE.slice(), origin, slots,
    params: P, seed,
    meta: {
      vpm, voxels: total, counts, footprint, grade: g - mny, wallTop: wallTopY - mny,
      pitchDeg: Math.round(Math.atan(q / 4) * 180 / Math.PI), riseQ: q,
      rooms: rooms.length, doors: doors.length,
      windows: Object.values(WALLS).reduce((a, wl) => a + wl.open.filter(o => o.type === 'window').length, 0),
      beds: beds.length, chests: chests.length, stair: !!stair, loft: !!loft,
      clipped, warnings
    }
  };
}

// =============================================================================
// files: .vox + .struct.json
// =============================================================================

/** Map the result's local palette to engine material ids, by NAME, against the
 *  materials.json list (index i is material id i+1: air is implicit at 0).
 *  @returns {{ids:Object<string,number>, missing:string[]}} */
export function resolveMaterials(materials) {
  const idOf = new Map();
  (materials || []).forEach((m, i) => idOf.set(m.id, i + 1));
  const ids = {}, missing = [];
  for (const n of PALETTE) {
    const id = idOf.get(n);
    if (id === undefined) missing.push(n); else ids[n] = id;
  }
  return {ids, missing};
}

/** The result as an engine-space grid of MATERIAL IDS (vox.js makeGrid shape:
 *  data[x + y*dx + z*dx*dy]). Throws if a material is missing or an id does not
 *  fit the .vox byte. */
export function toIdGrid(res, materials) {
  const {ids, missing} = resolveMaterials(materials);
  const used = Object.keys(res.meta.counts);
  const lacking = used.filter(n => missing.includes(n));
  if (lacking.length) throw new Error('materials.json has no ' + lacking.join(', '));
  const {x: nx, y: ny, z: nz} = res.dim;
  const data = new Uint8Array(nx * ny * nz);
  for (let z = 0; z < nz; z++) for (let y = 0; y < ny; y++) for (let x = 0; x < nx; x++) {
    const v = res.cells[(z * ny + y) * nx + x];
    if (!v) continue;
    const id = ids[PALETTE[v - 1]];
    if (id > 255) throw new Error('material ' + PALETTE[v - 1] + ' is id ' + id + ': a .vox palette index stops at 255');
    data[x + y * nx + z * nx * ny] = id;
  }
  return {dim: {x: nx, y: ny, z: nz}, data, color: null};
}

/** struct.json as text: stable key order, one slot per line, params one per
 *  line — a file a person can read and diff (PLAN §1.3). */
export function structJsonText(res, name, materials, extra) {
  const {ids} = resolveMaterials(materials);
  const used = {};
  for (const n of PALETTE) if (res.meta.counts[n]) used[n] = ids[n];
  const J = (v) => JSON.stringify(v);
  const L = [];
  L.push('{');
  L.push('  "name": ' + J(name) + ',');
  L.push('  "origin": ' + J(res.origin) + ',');
  L.push('  "size": ' + J([res.dim.x, res.dim.y, res.dim.z]) + ',');
  L.push('  "voxelsPerMetre": ' + J(res.meta.vpm) + ',');
  L.push('  "materials": ' + J(used) + ',');
  L.push('  "slots": [');
  res.slots.forEach((s, i) => L.push('    ' + J({name: s.name, kind: s.kind, pos: s.pos, yaw: s.yaw, props: s.props}) + (i + 1 < res.slots.length ? ',' : '')));
  L.push('  ],');
  L.push('  "generator": {');
  L.push('    "tool": "housegen",');
  L.push('    "version": ' + GENERATOR_VERSION + ',');
  L.push('    "seed": ' + J(res.seed) + ',');
  L.push('    "params": {');
  const keys = SCHEMA.filter(r => r.k).map(r => r.k);
  keys.forEach((k, i) => L.push('      ' + J(k) + ': ' + J(res.params[k]) + (i + 1 < keys.length ? ',' : '')));
  L.push('    }');
  L.push('  },');
  L.push('  "handEdited": ' + J(!!(extra && extra.handEdited)));
  L.push('}');
  return L.join('\n') + '\n';
}

/** Both files for one structure. `VOX` is assets/editor/vox.js (passed in so
 *  this module stays import-free for the test that loads it bare).
 *  @returns {{vox:Uint8Array, json:string}} */
export function buildStructureFiles(res, name, materials, VOX) {
  if (res.meta.clipped) throw new Error(res.meta.warnings.find(w => /\.vox cannot/.test(w)) || 'too big for a .vox');
  const g = toIdGrid(res, materials);
  const model = VOX.gridToModel(g, name.split('/').pop());
  const vox = VOX.writeVox([model], VOX.paletteFromMaterials(materials));
  return {vox, json: structJsonText(res, name, materials)};
}

/** Structure names: lowercase words, optionally one folder ("samples/smithy"). */
export function validName(n) { return typeof n === 'string' && /^[a-z0-9_]+(\/[a-z0-9_]+)?$/.test(n); }
