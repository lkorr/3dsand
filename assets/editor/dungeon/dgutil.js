/* dgutil.js — the shared vocabulary of the dungeon generators (PLAN_dungeons
 * §3.3): counter hash, quarter-turn transforms, the schema row vocabulary,
 * the UNIVERSAL sliders every piece gets (§4), and the piece Builder the kits
 * draw with.
 *
 * PURE MODULE — no DOM, no fetch, no Math.random(). Same discipline as
 * housegen.js / treegen.js: every choice is `frand(seed, key...)` keyed by WHAT
 * it decides (strings are hashed), so moving one slider never reshuffles an
 * unrelated choice, and the same params + seed give the same bytes anywhere.
 *
 * COORDINATES. Engine convention: Y up, one unit = one voxel (0.1 m), heading 0
 * faces +Z and heading 90 faces +X. Prim geometry is CONTINUOUS voxel space:
 * a cell (x,y,z) is inside a shape when its CENTRE (x+.5, y+.5, z+.5) is. A box
 * [min,max) therefore covers cells min..max-1 exactly when min/max are integers.
 *
 * THE PIECE FRAME. Every piece type is generated with its ORIGIN socket at
 * (0,0,0). Horizontal origin sockets face -Z (the piece body extends to +Z);
 * a socket's `pos` is the CENTRE OF THE FLOOR LINE of its opening, `w` its
 * width and `h` its clear height. A vertical socket (dir ±Y) has `pos` at the
 * centre of the opening plane and `w` x `h` = its horizontal extent (x by z).
 */

'use strict';

// =============================================================================
// counter hash (the treegen/housegen construction; only has to be STABLE)
// =============================================================================
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x >>> 0;
}
/** FNV-1a of a string, so readable keys ('pillar', 'set') can key draws. */
export function strHash(s) {
  let h = 0x811c9dc5;
  s = String(s);
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i) & 0xFFFF; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}
export function hashN(...vals) {
  let h = 0x9e3779b9;
  for (let i = 0; i < vals.length; i++) {
    const v = typeof vals[i] === 'string' ? strHash(vals[i]) : (vals[i] | 0);
    h = mix32((h ^ v) >>> 0);
    h = (h + 0x9e3779b9) >>> 0;
  }
  return mix32(h);
}
/** Uniform [0,1), keyed by the seed and by WHAT is being decided. */
export function frand(seed, ...key) { return hashN(seed, ...key) / 4294967296; }

/** FNV-1a over a byte string / text, as 8 hex digits (file content hashes). */
export function fnvHex(text) {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    h ^= c & 0xFF; h = Math.imul(h, 0x01000193) >>> 0;
    if (c > 0xFF) { h ^= c >>> 8; h = Math.imul(h, 0x01000193) >>> 0; }
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

// =============================================================================
// quarter-turn transforms
// =============================================================================
/** Yaw in degrees (multiple of 90) -> quarter turns 0..3. */
export function quarter(yaw) { return ((Math.round((yaw || 0) / 90) % 4) + 4) % 4; }
/** Rotate a vector by q quarter turns about +Y, heading convention (+Z -> +X). */
export function rotV(q, v) {
  const [x, y, z] = v;
  switch (q & 3) {
    case 0: return [x, y, z];
    case 1: return [z, y, -x];
    case 2: return [-x, y, -z];
    default: return [-z, y, x];
  }
}
/** Rotate then translate. -0 is normalised to 0 so JSON never prints "-0". */
export function xformP(q, t, p) {
  const r = rotV(q, p);
  return [r[0] + t[0] + 0, r[1] + t[1] + 0, r[2] + t[2] + 0].map(z0);
}
/** Rotate+translate a CELL index (PLAN 3.4.1 Rot: q1 takes (x, z) to
 *  (z, -1 - x)): the cell's centre is rotated, then floored. */
export function xformCell(q, t, c) {
  const r = rotV(q, [c[0] + 0.5, c[1], c[2] + 0.5]);
  return [Math.floor(r[0]) + t[0], c[1] + t[1], Math.floor(r[2]) + t[2]].map(z0);
}
/** Inverse of xformP. */
export function invXformP(q, t, p) {
  return rotV((4 - (q & 3)) & 3, [p[0] - t[0], p[1] - t[1], p[2] - t[2]]).map(z0);
}
function z0(v) { return v === 0 ? 0 : v; }
export function rotAxis(q, axis) {          // 'x'|'y'|'z'
  if (axis === 'y' || !(q & 1)) return axis;
  return axis === 'x' ? 'z' : 'x';
}
const DIR_OF = {'+x': [1, 0, 0], '-x': [-1, 0, 0], '+y': [0, 1, 0], '-y': [0, -1, 0], '+z': [0, 0, 1], '-z': [0, 0, -1]};
export function dirVec(d) { return DIR_OF[d].slice(); }
export function dirName(v) {
  for (const k in DIR_OF) { const w = DIR_OF[k]; if (w[0] === v[0] && w[1] === v[1] && w[2] === v[2]) return k; }
  return null;
}
export function rotDir(q, d) { return dirName(rotV(q, dirVec(d)).map(z0)); }
/** Rotate+translate an AABB {min,max} (rotation of corners, then re-sort). */
export function xformBox(q, t, min, max) {
  const a = xformP(q, t, min), b = xformP(q, t, max);
  return {min: [Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.min(a[2], b[2])],
          max: [Math.max(a[0], b[0]), Math.max(a[1], b[1]), Math.max(a[2], b[2])]};
}

/** Transform one prim (a fresh object; the input is not touched). */
export function xformPrim(q, t, p) {
  const o = JSON.parse(JSON.stringify(p));
  if (o.min && o.max) { const b = xformBox(q, t, o.min, o.max); o.min = b.min; o.max = b.max; }
  // Capsule ends and tube points name CELLS (PLAN 3.4.1).
  if (o.a) o.a = xformCell(q, t, o.a);
  if (o.b) o.b = xformCell(q, t, o.b);
  if (o.pts) o.pts = o.pts.map(pt => xformCell(q, t, pt));
  // A liquid carve's `level` is a local Y: it moves with the piece (quarter
  // turns never touch Y). Without this every pool in a piece placed off y = 0
  // was born at its own piece-local height -- a sump 22 m down filled to the brim.
  if (o.level !== undefined) o.level += t[1];
  if (o.axis) o.axis = rotAxis(q, o.axis);
  if (o.dir) o.dir = rotDir(q, o.dir);
  return o;
}

// =============================================================================
// shapes: containment + bounds (the JS mirror used by validation and tests;
// the GPU's WGSL is the truth for VOXELS, noise included — PLAN §2 point 3)
// =============================================================================
function segDist(p, a, b) {
  const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const ap = [p[0] - a[0], p[1] - a[1], p[2] - a[2]];
  const L2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  let t = L2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / L2 : 0;
  t = Math.max(0, Math.min(1, t));
  const d = [ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t];
  return {d: Math.hypot(d[0], d[1], d[2]), t};
}
const AX = {x: 0, y: 1, z: 2};
const C5 = (c) => [c[0] + 0.5, c[1] + 0.5, c[2] + 0.5];   // a cell's centre

/** Ramp (PLAN 3.4.1): the walking-surface PROFILE starts at min.y on the
 *  edge opposite `dir` and rises stepH every stepD cells along `dir` (no
 *  cap: a generator sizes the footprint so the last step is the top). */
export function rampProfile(p, q) {
  const ax = AX[p.dir[1]], sgn = p.dir[0] === '+' ? 1 : -1;
  const u = sgn > 0 ? q[ax] - p.min[ax] : p.max[ax] - q[ax];
  return p.min[1] + Math.floor(Math.max(0, u) / Math.max(1, p.stepD)) * p.stepH;
}

/** Is the point q (continuous) inside the prim's UN-NOISED shape? */
export function primContains(p, q) {
  const inBox = (m0, m1) => q[0] >= m0[0] && q[0] < m1[0] && q[1] >= m0[1] && q[1] < m1[1] && q[2] >= m0[2] && q[2] < m1[2];
  switch (p.shape) {
    case 'box': return inBox(p.min, p.max);
    case 'rbox': {
      const r = p.r || 0, d = [0, 0, 0];
      for (let i = 0; i < 3; i++) {
        const lo = p.min[i] + r, hi = p.max[i] - r;
        d[i] = q[i] < lo ? lo - q[i] : q[i] > hi ? q[i] - hi : 0;
      }
      return inBox(p.min, p.max) && Math.hypot(d[0], d[1], d[2]) <= r;
    }
    case 'ellipsoid': {
      let s = 0;
      for (let i = 0; i < 3; i++) {
        const c = (p.min[i] + p.max[i]) / 2, h = (p.max[i] - p.min[i]) / 2;
        if (h <= 0) return false;
        s += ((q[i] - c) / h) ** 2;
      }
      return s <= 1;
    }
    case 'cylinder': {
      if (!inBox(p.min, p.max)) return false;
      const a = AX[p.axis];
      let s = 0;
      for (let i = 0; i < 3; i++) {
        if (i === a) continue;
        const c = (p.min[i] + p.max[i]) / 2, h = (p.max[i] - p.min[i]) / 2;
        s += ((q[i] - c) / h) ** 2;
      }
      return s <= 1;
    }
    case 'capsule': return segDist(q, C5(p.a), C5(p.b)).d <= p.r;
    case 'spline_tube': {
      for (let i = 0; i + 1 < p.pts.length; i++) {
        const {d, t} = segDist(q, C5(p.pts[i]), C5(p.pts[i + 1]));
        if (d <= p.radii[i] + (p.radii[i + 1] - p.radii[i]) * t) return true;
      }
      return false;
    }
    case 'ramp': {
      // footprint in XZ; the solid band profile-below <= y < profile+height, y < max.y
      if (q[0] < p.min[0] || q[0] >= p.max[0] || q[2] < p.min[2] || q[2] >= p.max[2] || q[1] >= p.max[1]) return false;
      const f = rampProfile(p, q);
      return q[1] >= f - (p.below || 0) && q[1] < f + (p.height || 0);
    }
    case 'vault': {
      if (!inBox(p.min, p.max)) return false;
      const across = p.axis === 'x' ? 2 : 0;
      const ys = p.max[1] - p.rise;
      if (q[1] < ys) return true;
      const c = (p.min[across] + p.max[across]) / 2, half = (p.max[across] - p.min[across]) / 2;
      return ((q[across] - c) / half) ** 2 + ((q[1] - ys) / p.rise) ** 2 <= 1;
    }
    default: return false;
  }
}

/** The raw AABB of a prim's shape (no noise, blend or skin). */
export function shapeBounds(p) {
  if (p.shape === 'ramp') return {min: [p.min[0], p.min[1] - (p.below || 0), p.min[2]], max: p.max.slice()};
  if (p.min && p.max) return {min: p.min.slice(), max: p.max.slice()};
  if (p.shape === 'capsule') {
    return {min: [0, 1, 2].map(i => Math.min(p.a[i], p.b[i]) - p.r), max: [0, 1, 2].map(i => Math.max(p.a[i], p.b[i]) + p.r)};
  }
  if (p.shape === 'spline_tube') {
    const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
    p.pts.forEach((pt, i) => { for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], pt[k] - p.radii[i]); mx[k] = Math.max(mx[k], pt[k] + p.radii[i]); } });
    return {min: mn, max: mx};
  }
  throw new Error('prim without bounds: ' + p.shape);
}
/** The AABB the prim can touch: shape + noise amplitude + blend + skin depth.
 *  This is what the engine's sparse chunk index must cover (PLAN §3.4). */
export function primReach(p) {
  const b = shapeBounds(p);
  const pad = Math.ceil((p.noise ? p.noise.amp : 0) + (p.blend || 0) + (p.op === 'skin' ? (p.skin || 0) : 0));
  return {min: b.min.map(v => Math.floor(v - pad)), max: b.max.map(v => Math.ceil(v + pad))};
}
export function boxUnion(a, b) {
  if (!a) return b && {min: b.min.slice(), max: b.max.slice()};
  if (!b) return a;
  return {min: a.min.map((v, i) => Math.min(v, b.min[i])), max: a.max.map((v, i) => Math.max(v, b.max[i]))};
}
export function boxOverlap(a, b) {      // per-axis overlap extents (<=0 = disjoint)
  return [0, 1, 2].map(i => Math.min(a.max[i], b.max[i]) - Math.max(a.min[i], b.min[i]));
}

// =============================================================================
// the schema row vocabulary + the UNIVERSAL sliders (PLAN §4)
// =============================================================================
// Rows: {sec, note} | {k, n, d, min, max, step, int?, def} | {k, n, d, kind:'enum', options, def}
// | {k, n, d, kind:'bool', def}. `def` is the default (housegen keeps them in a
// separate DEFAULTS object; here the row carries it so a type's schema is one
// self-describing array the tuner builds its panel from).

/** Light types every kit can offer; a kit lists the subset it supports. */
export const LIGHT_TYPES = ['none', 'torch', 'lantern', 'candles', 'brazier', 'crystal', 'fungus'];

/**
 * The universal rows for one piece type. `size` gives the type's own label,
 * range and default for width / length / height / ceiling (metres); a key set
 * to null drops that row (a shaft has no "length", say). `u` overrides the
 * defaults of the other universal rows; `lights` is the kit's light list.
 */
export function universalSchema(size, u, lights, flags) {
  u = u || {};
  flags = flags || {};
  const S = (k, n, d, base) => {
    const o = size && size[k];
    if (o === null) return null;
    return Object.assign({k, n, d, min: base[0], max: base[1], step: 0.1, def: base[2]}, o || {});
  };
  const rows = [
    {sec: 'Size', note: 'Metres. 1 voxel = 0.1 m; a 3 m passage is 30 voxels wide.'},
    S('width', 'width (m)', 'Clear width of the space, wall to wall.', [1, 40, 3]),
    S('length', 'length (m)', 'Along the piece from its origin socket.', [2, 120, 10]),
    S('height', 'height (m)', 'Clear height of the walls, floor to the springing of the ceiling.', [1, 30, 3]),
    S('ceiling', 'ceiling rise (m)', 'Extra height of the ceiling above the walls: a vault crown, a dome, the rounded back of a drift.', [0, 20, 0.5]),
    {sec: 'Surface', note: 'Noise is in WORLD space, so walls stay continuous across piece joins.'},
    {k: 'floorUneven', n: 'floor unevenness', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Bumps in the floor slab (noise amplitude, scaled by the kit).'},
    {k: 'wallRough', n: 'wall roughness', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Noise amplitude on carved walls, scaled by the kit (caves rough, ruins dressed).'},
    {k: 'wallScale', n: 'roughness scale (m)', min: 0.3, max: 8, step: 0.1, def: 1.5, d: 'Feature size of the wall noise.'},
    // VARIETY (D8): what breaks the "connected blocks" look. Every row at its
    // default is the regular piece the kit drew before these rows existed.
    {sec: 'Variety', note: 'Break the kit look. Every row at 0 is the regular piece. The sockets stay where they are (wander is zero, with zero slope, at both ends), except slope, which lifts the far ones.'},
    flags.passage ? {k: 'wander', n: 'wander (m)', min: 0, max: 8, step: 0.1, def: 0, d: 'Sideways S-curve of the passage centreline: the passage bends and comes back. Zero at both ends, so it still snaps.'} : null,
    flags.passage ? {k: 'wanderWave', n: 'wander wavelength (m)', min: 4, max: 60, step: 0.5, def: 16, d: 'Length of one full bend of the wander (two harmonics, so it never repeats exactly).'} : null,
    flags.slope ? {k: 'slope', n: 'slope (m)', min: -8, max: 8, step: 0.1, def: 0, d: 'Height gained from the origin socket to the far end (negative = falls). The far sockets move with the floor.'} : null,
    flags.slope ? {k: 'slopeSteps', n: 'slope steps', min: 0, max: 8, step: 1, int: true, def: 0, d: '0 = a smooth grade; n = the slope is taken in n steps (terraces, a worn stair in a passage).'} : null,
    {k: 'widthVar', n: 'width variation', min: 0, max: 0.6, step: 0.05, def: 0, d: 'The space swells and narrows along its length (fraction of the width). The ends keep the socket size.'},
    {k: 'heightVar', n: 'height variation', min: 0, max: 0.6, step: 0.05, def: 0, d: 'The roof rises and dips along the length (fraction of the height).'},
    {k: 'jitter', n: 'irregularity', min: 0, max: 1, step: 0.05, def: 0, d: 'Uneven spacing of repeated things (timber sets, ribs, columns, niches) and per-element lean, sag, breaks and missing ones.'},
    flags.room ? {k: 'bays', n: 'side bays', min: 0, max: 6, step: 1, int: true, def: 0, d: 'Alcoves / side bays cut into the walls at irregular places: the room is no longer a box.'} : null,
    flags.room ? {k: 'bayDepth', n: 'bay depth (m)', min: 0.5, max: 8, step: 0.1, def: 2, d: 'How far a side bay goes back into the rock (each is 60-140% of it).'} : null,
    flags.room ? {k: 'lobe', n: 'offset lobe', min: 0, max: 1, step: 0.05, def: 0, d: 'One side of the room bulges out into a second, offset space (a side chamber, a transept, a worked-out pocket). 0 = none.'} : null,
    {sec: 'Age', note: 'Decay and moisture are applied by the core to every piece the same way, plus whatever the kit does with them.'},
    {k: 'decay', n: 'decay / age', min: 0, max: 1, step: 0.05, def: 0.25, d: 'Rubble on floors, broken trim and supports, collapsed corners.'},
    {k: 'moisture', n: 'moisture', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Moss on walls and ceilings, puddles in floor dips.'},
    {k: 'deco', n: 'deco density', min: 0, max: 1, step: 0.05, def: 0.5, d: 'How much of the suggested furniture and clutter is placed.'},
    {sec: 'Light', note: 'Underground is dark; a light is an emissive voxel. Light spreads through the irradiance grid in 0.4 m blocks: a glowing SURFACE a block or more across lights a room, a one-voxel flame barely lights its bracket. Count is clamped to the mount points the piece offers.'},
    {k: 'lightCount', n: 'lights', min: 0, max: 16, step: 1, int: true, def: 2, d: 'Number of lights placed at the piece\'s mount points.'},
    {k: 'lightType', n: 'light type', kind: 'enum', options: lights || LIGHT_TYPES, def: lights ? lights[0] : 'torch',
     d: 'torch = a bracket and a flame; lantern / candles / brazier = props; crystal = an emissive crystal lump; fungus = glowcaps.'},
    {k: 'warmth', n: 'light warmth', min: 0, max: 1, step: 0.05, def: 0.8, d: 'Chance each light uses the warm light material (else the cool one).'},
    {k: 'lightPower', n: 'light power', min: 0, max: 1, step: 0.05, def: 0, d: 'Size of each light: 0 = the small flame / lamp / lump; 1 = a torch fire 0.4 m across, a lamp with a big flame, a full fire bowl, a crystal lump a metre across. Bigger glowing surfaces carry further.'},
    {k: 'lightDensity', n: 'light density', min: 0, max: 1, step: 0.05, def: 0, d: 'Adds lights by floor area so a big room is lit like a small one (1 = about one light per 25 m2). 0 = exactly the light count.'},
    flags.room ? {sec: 'Key light', note: 'One strong light that makes a big space readable.'} : null,
    flags.room ? {k: 'keyLight', n: 'key light', kind: 'enum', options: ['none', 'shaft', 'fire', 'crystals'], def: 'none',
                  d: 'shaft = a light shaft up through the roof to the sky (set its height so it reaches the ground: the piece then opens at the surface); fire = a big fire in a ring of stones; crystals = a cluster of big glowing crystals.'} : null,
    flags.room ? {k: 'keySize', n: 'key light size', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Width of the shaft / the fire / the crystal cluster.'} : null,
    flags.room ? {k: 'keyHeight', n: 'shaft height (m)', min: 2, max: 80, step: 0.5, def: 12, d: 'shaft: how high above the floor the shaft is cut (tuned to the ground over this room: it is a slider the placer adjusts per map).'} : null,
    flags.room ? {k: 'keyAt', n: 'key light at', min: 0, max: 1, step: 0.05, def: 0.5, d: 'Where along the room (0 = the entrance end, 1 = the far end).'} : null,
    flags.room ? {k: 'keySide', n: 'key light across', min: -1, max: 1, step: 0.05, def: 0, d: 'Across the room (-1 = the left wall, +1 = the right).'} : null,
    {sec: 'Join', note: 'How this piece meets its neighbours.'},
    {k: 'blend', n: 'blend radius (vox)', min: 0, max: 16, step: 1, int: true, def: 4, d: 'Smooth-min radius of this piece\'s carves against neighbouring carves.'},
    {k: 'asym', n: 'symmetry break', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Seeded offsets on positions and sizes that would otherwise be mirror-exact.'}
  ].filter(Boolean);
  for (const r of rows) if (r.k && u[r.k] !== undefined) r.def = u[r.k];
  return rows;
}

export function defaultsOf(schema) {
  const p = {};
  for (const r of schema) if (r.k) p[r.k] = Array.isArray(r.def) ? r.def.slice() : r.def;
  return p;
}

/** Fill, clamp and snap against a schema (housegen.normalizeParams rules).
 *  Unknown keys are dropped: the schema is the truth. */
export function normalizeParams(schema, src) {
  const p = defaultsOf(schema);
  const s = src && typeof src === 'object' ? src : {};
  for (const r of schema) {
    if (!r.k) continue;
    const v = s[r.k];
    if (r.kind === 'enum') { if (r.options.includes(v)) p[r.k] = v; }
    else if (r.kind === 'bool') { if (typeof v === 'boolean') p[r.k] = v; }
    else if (typeof v === 'number' && isFinite(v)) {
      let x = Math.min(r.max, Math.max(r.min, v));
      x = r.int ? Math.round(x) : Math.round(x / r.step) * r.step;
      p[r.k] = Number(x.toFixed(4));
    }
  }
  return p;
}

// =============================================================================
// the Builder: what a piece generator draws with
// =============================================================================
/**
 * One Builder per generate() call. The kit's generator calls carve / fill /
 * skin / keep / socket / prop / ref / way / lightSpot in LOCAL coordinates and
 * returns `b.finish(extra)`. finish() applies the universal sliders the same
 * way for every kit: wall noise and blend on carves, decay rubble and moisture
 * scatter on the rooms, the light choice among the mount points.
 *
 * kitInfo: {kit, roughMax (vox of wall noise at wallRough 1), floorMax (vox of
 * floor bumps at floorUneven 1), lights: {type -> {prop?|prim?}}}
 */
/** Grow a shape by d voxels on every side (in place). */
export function inflate(p, d) {
  if (p.shape === 'ramp') { p.min = [p.min[0] - d, p.min[1], p.min[2] - d]; p.max = [p.max[0] + d, p.max[1] + d, p.max[2] + d]; p.height = (p.height || 0) + d; p.below = (p.below || 0) + d; return p; }
  if (p.min && p.max) { p.min = p.min.map(v => v - d); p.max = p.max.map(v => v + d); }
  if (p.shape === 'vault') p.rise = (p.rise || 0) + d;
  if (p.shape === 'rbox') p.r = (p.r || 0) + d;
  if (p.shape === 'capsule') p.r = (p.r || 0) + d;
  if (p.shape === 'spline_tube') p.radii = p.radii.map(r => r + d);
  return p;
}

export class Builder {
  constructor(kitInfo, type, P, seed, opts) {
    this.kit = kitInfo; this.type = type; this.P = P; this.seed = seed >>> 0;
    this.vpm = (opts && opts.vpm) || 10;
    this.prims = []; this.scatter = []; this.sockets = []; this.props = []; this.refs = [];
    this.waynodes = []; this.lightSpots = []; this.notes = [];
    this.surface = false; this.flooded = false;
  }
  /** metres -> voxels (rounded; at least `lo`). */
  m(x, lo) { const v = Math.round(x * this.vpm); return lo !== undefined ? Math.max(lo, v) : v; }
  r(...k) { return frand(this.seed, ...k); }
  ri(lo, hi, ...k) { return lo + Math.floor(this.r(...k) * (hi - lo + 1)); }
  pick(arr, ...k) { return arr[Math.min(arr.length - 1, Math.floor(this.r(...k) * arr.length))]; }
  chance(p, ...k) { return this.r(...k) < p; }
  /** A symmetry-break offset in [-amount, amount] voxels, scaled by `asym`. */
  jit(amount, ...k) { return Math.round((this.r('asym', ...k) * 2 - 1) * amount * this.P.asym); }
  /** Deco count: `base` at deco 0.5, 0 at deco 0, 2x at deco 1. */
  deco(base) { return Math.round(base * this.P.deco * 2); }

  // ---- VARIETY (D8) ------------------------------------------------------------
  // Every helper returns the regular piece's value when its slider is 0, so a
  // kit can call them unconditionally and an old dungeon generates as before.

  /** Sideways offset (voxels) of a passage centreline at z on [0, L]: zero
   *  WITH ZERO SLOPE at both ends (sin^2 envelope), so the sockets stay on the
   *  axis and the join stays square to them. Two harmonics, seeded phases. */
  wanderX(z, L, key) {
    const A = this.m(this.P.wander || 0);
    if (!A || L <= 0) return 0;
    const lam = Math.max(20, this.m(this.P.wanderWave || 16));
    const t = Math.min(1, Math.max(0, z / L));
    const env = Math.sin(Math.PI * t) ** 2;
    const p1 = this.r('wander', key || '', 'p1') * Math.PI * 2, p2 = this.r('wander', key || '', 'p2') * Math.PI * 2;
    const w = 0.8 * Math.sin(p1 + (2 * Math.PI * z) / lam) + 0.35 * Math.sin(p2 + (2 * Math.PI * z) / (lam * 0.41));
    return Math.round((A * env * w) / 1.15);
  }
  /** Floor height (voxels) at z on [0, L] from the slope rows: a smooth grade
   *  or `slopeSteps` terraces. 0 at z <= 0, the full slope at z >= L. */
  slopeY(z, L) {
    const R = this.m(this.P.slope || 0);
    if (!R || L <= 0) return 0;
    const t = Math.min(1, Math.max(0, z / L)), n = this.P.slopeSteps | 0;
    return n > 0 ? Math.round((R * Math.floor(t * n + 0.5)) / n) : Math.round(R * t);
  }
  /** The slope's full rise in voxels (where the far sockets go). */
  slopeR() { return this.m(this.P.slope || 0); }
  /** Smooth seeded value noise in [-1, 1] along z (cells of `cell` voxels). */
  vnoise(z, cell, ...key) {
    const c = Math.max(1, cell), i = Math.floor(z / c), f = z / c - i, s = f * f * (3 - 2 * f);
    const a = this.r('vn', ...key, i) * 2 - 1, b = this.r('vn', ...key, i + 1) * 2 - 1;
    return a + (b - a) * s;
  }
  /** Width / height multiplier at z for the widthVar / heightVar rows (1 at
   *  0). `pin` (0..1) fades the variation out toward the ends (the sockets). */
  varAt(row, z, L, key, pin) {
    const v = this.P[row] || 0;
    if (!v) return 1;
    let env = 1;
    if (pin !== undefined && L > 0) {
      const t = Math.min(1, Math.max(0, z / L));
      env = Math.min(1, Math.min(t, 1 - t) / Math.max(0.01, pin));
    }
    return 1 + v * env * this.vnoise(z, this.m(3.5), row, key || '');
  }
  /** Stations along [z0, z1]: nominal spacing S from z0 + first (default S/2),
   *  each gap stretched or shrunk by up to 45 % and each station nudged by
   *  `jitter` (keyed per index: raising jitter moves stations, it never
   *  reorders them). jitter 0 = the regular row the kits drew before. */
  stations(z0, z1, S, key, first) {
    const J = this.P.jitter || 0, out = [];
    let z = z0 + (first === undefined ? Math.round(S / 2) : first);
    for (let i = 0; z <= z1 && i < 4096; i++) {
      out.push(Math.round(z));
      z += S * (1 + 0.9 * J * (this.r('stn', key || '', i) - 0.5));
    }
    return out;
  }
  /** `chance` scaled by jitter: per-element breaks / missing / lean. */
  odd(chance, ...key) { return (this.P.jitter || 0) > 0 && this.r('odd', ...key) < chance * this.P.jitter; }

  /** Cut [z0, z1] into segments about segLen long (irregular with jitter).
   *  Each {z0, z1, zm, x: wander at zm, y: slope at zm, w, h: width / height
   *  multipliers}; the first and last keep w = h = 1 and x = 0 so the sockets
   *  keep their exact opening. Segments overlap by one cell. */
  segments(z0, z1, segLen, key) {
    const span = z1 - z0;
    const n = Math.max(1, Math.round(span / Math.max(4, segLen)));
    const cuts = [z0];
    for (let i = 1; i < n; i++) cuts.push(Math.round(z0 + (span * (i + 0.5 * (this.P.jitter || 0) * (this.r('seg', key || '', i) - 0.5))) / n));
    cuts.push(z1);
    const out = [];
    for (let i = 0; i < n; i++) {
      const a = cuts[i], b = cuts[i + 1], zm = Math.round((a + b) / 2);
      const end = i === 0 || i === n - 1;
      out.push({i, z0: i === 0 ? a : a - 1, z1: b + (i === n - 1 ? 0 : 1), zm, end,
                x: end ? 0 : this.wanderX(zm - z0, span, key), y: this.slopeY(zm - z0, span),
                y0: this.slopeY(a - z0, span), y1: this.slopeY(b - z0, span),
                w: end ? 1 : this.varAt('widthVar', zm - z0, span, key), h: end ? 1 : this.varAt('heightVar', zm - z0, span, key)});
    }
    return out;
  }

  /** A footing under a floor-standing prop / light on a rough floor: fills only
   *  AIR (openOnly) from `depth` below the prop's floor cell up to it, so the
   *  prop never floats over a noise dip; buried rock is untouched. */
  footing(pos, r, depth, mat, tag) {
    const d = Math.max(2, Math.round(depth === undefined ? (this.floorNoise() ? this.floorNoise().amp : 0) + 3 : depth));
    this.fill('cylinder', {min: [pos[0] - r, pos[1] - d, pos[2] - r], max: [pos[0] + r + 1, pos[1], pos[2] + r + 1], axis: 'y'},
              {mat: mat || 'floor', noise: null, layer: 3, mask: 'air', tag: tag || 'footing'});
  }

  /** Room variety (bays + an offset lobe) for a room whose interior is the box
   *  f = {x0, x1, z0, z1, y (floor), H (clear height), kind: 'cave'|'mine'|'built',
   *  floorAt?(z)}; the kit's own walls are left alone. Returns the carves. */
  roomVariety(f) {
    const P = this.P, out = [];
    const kind = f.kind || 'cave';
    const L = f.z1 - f.z0, W = f.x1 - f.x0;
    const yAt = (z) => (f.floorAt ? f.floorAt(z) : f.y);
    const wall = (side, z0, z1, depth, h, key, room) => {
      // a volume out of the side wall `side` (-1 left = x0, +1 right = x1)
      const xw = side < 0 ? f.x0 : f.x1, y = yAt((z0 + z1) / 2);
      const xa = side < 0 ? xw - depth : xw - 2, xb = side < 0 ? xw + 2 : xw + depth;
      let i;
      if (kind === 'cave') {
        i = this.carve('ellipsoid', {min: [side < 0 ? xa : xw - Math.round(depth * 0.5), y - 4, z0], max: [side < 0 ? xw + Math.round(depth * 0.5) : xb, y + h, z1]},
                       {tag: room ? 'room' : 'niche', noise: 1.1, blend: Math.max(P.blend, 6)});
        this.fill('box', {min: [Math.min(xa, xb) - 2, y - 6, z0 - 2], max: [Math.max(xa, xb) + 2, y, z1 + 2]}, {mat: 'floor', noise: 'floor', layer: 2, mask: 'air', tag: 'bayfloor'});
      } else if (kind === 'mine') {
        i = this.carve('box', {min: [xa, y, z0], max: [xb, y + h, z1]}, {tag: room ? 'room' : 'niche', noise: 0.9, blend: 3});
        this.floor(xa, z0, xb, z1, y);
      } else {
        const rise = Math.max(2, Math.min(Math.round((z1 - z0) / 2), Math.round(h * 0.35)));
        i = this.carve('vault', {min: [xa, y, z0], max: [xb, y + h, z1], axis: 'x', rise}, {tag: room ? 'room' : 'niche', noise: null, blend: 0, layer: 3});
        this.skin('vault', {min: [xa, y, z0], max: [xb, y + h, z1], axis: 'x', rise}, {skin: 2, mat: 'wall', layer: 1});
        this.floor(xa, z0, xb, z1, y, {layer: 3});
      }
      out.push(i);
      return i;
    };
    // A volume may not cut a side doorway (f.doors: [{side, z, w}]): it moves
    // to the other wall, else it is left out.
    const doors = f.doors || [];
    const clear = (side, za, zb) => !doors.some(dr => dr.side === side && za < dr.z + dr.w / 2 + this.m(1.2) && zb > dr.z - dr.w / 2 - this.m(1.2));
    const pickSide = (side, za, zb) => clear(side, za, zb) ? side : clear(-side, za, zb) ? -side : 0;
    // Side bays: irregular places along the long walls.
    const nb = P.bays | 0;
    for (let k = 0; k < nb; k++) {
      const bw = Math.round(this.m(1.4) + this.r('bay', k, 'w') * Math.min(this.m(3.6), L * 0.35));
      const zc = Math.round(f.z0 + bw / 2 + 2 + this.r('bay', k, 'z') * Math.max(1, L - bw - 4));
      const side = pickSide(this.r('bay', k, 'side') < 0.5 ? -1 : 1, zc - bw / 2, zc + bw / 2);
      if (!side) continue;
      const depth = Math.max(4, Math.round(this.m(P.bayDepth || 2) * (0.6 + 0.8 * this.r('bay', k, 'd'))));
      const h = Math.max(this.m(1.4), Math.round(f.H * (0.55 + 0.45 * this.r('bay', k, 'h'))));
      wall(side, zc - Math.round(bw / 2), zc + Math.round(bw / 2), depth, h, 'bay' + k, false);
    }
    // The lobe: one side bulges into a second space, offset along the room.
    if ((P.lobe || 0) > 0) {
      const lw = Math.max(this.m(2), Math.round(L * (0.3 + 0.35 * P.lobe)));
      const zc = Math.round(f.z0 + lw / 2 + this.r('lobe', 'z') * Math.max(1, L - lw));
      const side = pickSide(this.r('lobe', 'side') < 0.5 ? -1 : 1, zc - lw / 2, zc + lw / 2);
      const depth = Math.max(this.m(1.5), Math.round(W * (0.25 + 0.55 * P.lobe)));
      const h = Math.max(this.m(1.6), Math.round(f.H * (0.7 + 0.3 * this.r('lobe', 'h'))));
      if (side) wall(side, zc - Math.round(lw / 2), zc + Math.round(lw / 2), depth, h, 'lobe', true);
    }
    return out;
  }

  /** Extra floor light mounts on a ~5 m grid over a room (only with
   *  lightDensity > 0, so a piece without it keeps its mount list -- and with
   *  it, its seeded light picks -- exactly). f as roomVariety; skip(x, z) ->
   *  true drops a spot (a pillar, a pit, the way through). */
  gridSpots(f, skip) {
    if (!((this.P.lightDensity || 0) > 0)) return;
    const step = this.m(5), ins = this.m(1.2);
    for (let z = f.z0 + ins; z <= f.z1 - ins; z += step) for (let x = f.x0 + ins; x <= f.x1 - ins; x += step) {
      const xx = Math.round(x + this.jit(this.m(1), 'gs', x, z, 'x')), zz = Math.round(z + this.jit(this.m(1), 'gs', x, z, 'z'));
      if (skip && skip(xx, zz)) continue;
      const yy = f.floorAt ? f.floorAt(zz) : f.y;
      // only where the room really is (a bowl narrows toward its floor)
      if (!this.prims.some(p => p.op === 'carve' && p.tag === 'room' && !p.mat && primContains(p, [xx + 0.5, yy + 1.5, zz + 0.5]))) continue;
      this.lightSpot([xx, yy, zz], 'floor', '+y');
    }
  }

  /** The KEY LIGHT of a room (keyLight / keySize / keyHeight / keyAt / keySide):
   *  f = {x0, x1, z0, z1, y (floor), roof (the roof's local y over the room),
   *  kind, floorAt?(z)}. shaft = a carve up through the roof to keyHeight (the
   *  piece becomes a surface piece: the shaft is MEANT to break the ground);
   *  fire = a ring of stones round a bed of flame with tongues; crystals = big
   *  glowing prisms rooted in the floor. */
  keyLight(f) {
    const P = this.P;
    if (!P.keyLight || P.keyLight === 'none') return;
    const S = P.keySize === undefined ? 0.5 : P.keySize;
    const x = Math.round((f.x0 + f.x1) / 2 + (P.keySide || 0) * (f.x1 - f.x0) * 0.38);
    const z = Math.round(f.z0 + (f.z1 - f.z0) * (0.12 + 0.76 * (P.keyAt === undefined ? 0.5 : P.keyAt)));
    const y = f.floorAt ? f.floorAt(z) : f.y;
    if (P.keyLight === 'shaft') {
      const r = Math.max(6, Math.round(this.m(0.8 + 2.2 * S)));
      const top = y + Math.max(this.m(P.keyHeight || 12), (f.roof || y) - y + r + 4);
      const kind = f.kind || 'cave';
      if (kind === 'built') {
        // an oculus: a round well straight up from the vault crown
        this.carve('cylinder', {min: [x - r, Math.round((f.roof || y) - 4), z - r], max: [x + r, top, z + r], axis: 'y'}, {tag: 'apron', noise: null, blend: 0, layer: 3});
        this.skin('cylinder', {min: [x - r, Math.round((f.roof || y) - 4), z - r], max: [x + r, top, z + r], axis: 'y'}, {skin: 2, mat: 'wall', layer: 1});
      } else if (kind === 'mine') {
        this.carve('box', {min: [x - r, Math.round((f.roof || y) - 4), z - r], max: [x + r, top, z + r]}, {tag: 'apron', noise: 0.6, blend: 3});
      } else {
        const dx = this.jit(r, 'keyshaft', 'x'), dz = this.jit(r, 'keyshaft', 'z');
        const pts = [[x, Math.round((f.roof || y) - r), z], [x + dx, Math.round(((f.roof || y) + top) / 2), z + dz], [x - Math.round(dx / 2), top, z - Math.round(dz / 2)]];
        this.carve('spline_tube', {pts, radii: [r + 4, r, Math.round(r * 1.3)]}, {tag: 'apron', noise: 1.1, blend: Math.max(P.blend, 8)});
      }
      // The pool of daylight on the floor: moss / ferns where it lands.
      this.surface = true;
      this.notes.push('key light shaft: cut to ' + ((top - y) / this.vpm).toFixed(1) + ' m above the floor; the piece is a surface piece');
      this.ref('light', [x, y, z], {name: 'key_shaft', props: {type: 'shaft'}});
      return;
    }
    if (P.keyLight === 'fire') {
      const r = Math.max(3, Math.round(this.m(0.35 + 0.9 * S)));
      // stones round the fire, then the bed of flame, then tongues
      this.fill('cylinder', {min: [x - r - 3, y - 3, z - r - 3], max: [x + r + 4, y + 3, z + r + 4], axis: 'y'}, {mat: 'loose', noise: null, layer: 4, tag: 'keylight'});
      this.fill('cylinder', {min: [x - r, y - 3, z - r], max: [x + r + 1, y + 3, z + r + 1], axis: 'y'}, {mat: 'candle_flame', noise: null, layer: 4, tag: 'keylight'});
      const nt = 3 + Math.round(S * 4);
      for (let i = 0; i < nt; i++) {
        const a = this.r('tongue', i, 'a') * Math.PI * 2, d = Math.sqrt(this.r('tongue', i, 'd')) * (r - 1);
        const tx = x + Math.round(Math.cos(a) * d), tz = z + Math.round(Math.sin(a) * d);
        const th = Math.round(this.m(0.3) + this.r('tongue', i, 'h') * this.m(0.5 + S));
        const tw = 1 + Math.round(this.r('tongue', i, 'w') * 2);
        this.fill('box', {min: [tx - tw, y + 3, tz - tw], max: [tx + tw, y + 3 + th, tz + tw]}, {mat: 'candle_flame', noise: null, layer: 4, tag: 'keylight'});
      }
      this.ref('light', [x, y, z], {name: 'key_fire', props: {type: 'fire', warm: true}});
      return;
    }
    // crystals: big prisms leaning out of a common quartz root
    const n = 3 + Math.round(S * 4), Lm = this.m(1.2 + 2.6 * S);
    this.fill('ellipsoid', {min: [x - 8, y - 10, z - 8], max: [x + 9, y + 4, z + 9]}, {mat: 'quartzite', noise: 0.3, layer: 4, tag: 'keylight'});
    for (let i = 0; i < n; i++) {
      const a = this.r('kc', i, 'a') * Math.PI * 2, lean = 0.15 + 0.55 * this.r('kc', i, 'lean');
      const len = Math.round(Lm * (0.45 + 0.55 * this.r('kc', i, 'len')));
      const rr = Math.max(2, Math.round(len / 7));
      const tip = [x + Math.round(Math.cos(a) * lean * len), y + Math.round(len * (1 - lean * 0.5)), z + Math.round(Math.sin(a) * lean * len)];
      this.fill('spline_tube', {pts: [[x, y - 6, z], [Math.round((x + tip[0]) / 2), Math.round((y + tip[1]) / 2), Math.round((z + tip[2]) / 2)], tip], radii: [rr + 1, rr, 1]},
                {mat: i % 3 === 2 ? 'gem_arcane' : 'crystal', noise: null, layer: 4, tag: 'keylight'});
    }
    this.ref('light', [x, y, z], {name: 'key_crystals', props: {type: 'crystal', warm: false}});
  }

  wallNoise(mult) {
    const amp = Number((this.P.wallRough * this.kit.roughMax * (mult === undefined ? 1 : mult)).toFixed(2));
    if (amp <= 0) return null;
    return {amp, scale: Math.max(2, this.m(this.P.wallScale)), octaves: this.kit.octaves || 2, seedKey: 'wall'};
  }
  floorNoise() {
    const amp = Number((this.P.floorUneven * this.kit.floorMax).toFixed(2));
    return amp > 0 ? {amp, scale: Math.max(2, this.m(1.2)), octaves: 2, seedKey: 'floor'} : null;
  }

  /** Add a prim. o: {mat, noise: 'wall'|'floor'|null|{...}|number (wall mult),
   *  blend, layer, tag, skin, mask, pattern}. Returns its local index. */
  prim(op, shape, geom, o) {
    o = o || {};
    const p = Object.assign({op, shape}, geom);
    if (o.mat) p.mat = o.mat;
    else if (op === 'fill') p.mat = 'wall';
    else if (op === 'skin') p.mat = 'wall';
    let nz = o.noise;
    if (nz === undefined) nz = op === 'carve' ? 'wall' : null;
    if (nz === 'wall') nz = this.wallNoise();
    else if (nz === 'floor') nz = this.floorNoise();
    else if (typeof nz === 'number') nz = this.wallNoise(nz);
    if (nz) p.noise = nz;
    p.blend = o.blend !== undefined ? o.blend : (op === 'carve' ? this.P.blend : 0);
    if (op === 'skin') {
      // PLAN 3.4.1: a skin paints solid cells INSIDE its own shape within
      // `skin` of the carve, so its shape is the carve's inflated by skin.
      p.skin = o.skin || 2;
      inflate(p, p.skin);
    }
    if (op === 'carve' && o.liquid) { p.mat = o.liquid; p.level = Math.round(o.level); }
    if (o.layer !== undefined) p.layer = o.layer;
    if (o.mask) p.mask = o.mask;
    if (o.pattern) p.pattern = o.pattern;
    if (o.tag) p.tag = o.tag;
    for (const k of ['min', 'max', 'a', 'b']) if (p[k]) p[k] = p[k].map(v => Math.round(v));
    if (p.pts) p.pts = p.pts.map(pt => pt.map(v => Math.round(v)));
    if (p.radii) p.radii = p.radii.map(v => Math.max(1, Math.round(v)));
    if (p.r !== undefined) p.r = Math.max(0, Math.round(p.r));
    if (p.min && p.max) {
      const a = p.min, b = p.max;
      p.min = [Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.min(a[2], b[2])];
      p.max = [Math.max(a[0], b[0]), Math.max(a[1], b[1]), Math.max(a[2], b[2])];
      for (let i = 0; i < 3; i++) if (p.max[i] === p.min[i]) p.max[i] = p.min[i] + 1;
    }
    this.prims.push(p);
    return this.prims.length - 1;
  }
  carve(shape, geom, o) { return this.prim('carve', shape, geom, o); }
  fill(shape, geom, o) { return this.prim('fill', shape, geom, o); }
  skin(shape, geom, o) { return this.prim('skin', shape, geom, o); }
  keep(shape, geom, o) { return this.prim('keep', shape, geom, Object.assign({noise: null}, o)); }

  // ---- shorthands ------------------------------------------------------------
  box(op, min, max, o) { return this.prim(op, 'box', {min, max}, o); }
  /** A room / passage volume: carve tagged 'room' so the core scatters on it. */
  room(shape, geom, o) { return this.carve(shape, geom, Object.assign({tag: 'room'}, o)); }
  /** A floor slab [y-th, y) over the box footprint, floor noise. */
  floor(x0, z0, x1, z1, y, o) {
    o = o || {};
    const th = o.th || 2;
    return this.fill('box', {min: [x0, y - th, z0], max: [x1, y, z1]}, Object.assign({mat: 'floor', noise: 'floor', layer: 2}, o));
  }
  /** A straight horizontal passage along +Z from z0 to z1, floor at y,
   *  profile rect|arch|round|rough. Returns the room prim index. */
  passage(z0, z1, cx, y, w, h, profile, o) {
    o = o || {};
    const x0 = cx - w / 2, x1 = cx + w / 2;
    let i;
    if (profile === 'arch') {
      const rise = Math.max(2, Math.min(Math.round(w / 2), o.rise !== undefined ? o.rise : Math.round(w / 2)));
      i = this.room('vault', {min: [x0, y, z0], max: [x1, y + h + rise, z1], axis: 'z', rise}, o);
    } else if (profile === 'round') {
      i = this.room('cylinder', {min: [x0, y - Math.round(h * 0.15), z0], max: [x1, y + h, z1], axis: 'z'}, o);
    } else if (profile === 'rough') {
      i = this.room('rbox', {min: [x0, y, z0], max: [x1, y + h, z1], r: Math.round(Math.min(w, h) / 3)}, o);
    } else {
      i = this.room('box', {min: [x0, y, z0], max: [x1, y + h, z1]}, o);
    }
    return i;
  }

  /** A doorway / approach passage (not a room: no scatter on it). */
  doorway(z0, z1, cx, y, w, h, profile, o) {
    return this.passage(z0, z1, cx, y, w, h, profile, Object.assign({tag: 'door'}, o));
  }
  /** A heap of loose matter on the floor y0: footprint half-sizes rx, rz,
   *  height hgt, clamped so its steepest slope (3*hgt/r for this ellipsoid
   *  cap) stays under the kit's angle of repose — worldgen births powder AT
   *  REST (PLAN §2.2). mask 'air': it only fills open space. */
  mound(cx, y0, cz, rx, rz, hgt, o) {
    const rep = this.kit.repose || 0.9;
    const H = Math.min(hgt, rep * Math.min(rx, rz) / 3);
    if (H < 1) return -1;
    const hx = rx / 0.866, hz = rz / 0.866, hy = 2 * H, cy = y0 + H - hy;
    return this.fill('ellipsoid', {min: [cx - hx, cy - hy, cz - hz], max: [cx + hx, cy + hy, cz + hz]},
                     Object.assign({mat: 'rubble', mask: 'air', layer: 4, noise: null}, o));
  }
  /** A straight flight of steps (PLAN 3.4.1 ramp). Travel along `dir`
   *  ('+x'|'-x'|'+z'|'-z' = the direction of ASCENT) over the along-range
   *  [s0, s1) (absolute, s0 < s1) and across-range [a0, a1); y0 = the walking
   *  surface at the low end; rise R split into steps of stepH. The ramp
   *  footprint is cut to (n+1) * stepD so the profile tops out exactly at the
   *  high edge; any remainder is a flat box at the top. band: {height, below}.
   *  Returns the rise actually built (a whole number of steps). */
  flight(op, dir, a0, a1, s0, s1, y0, R, stepH, band, o) {
    const h = Math.max(1, Math.round(stepH)), n = Math.max(1, Math.round(R / h)), RR = n * h;
    const Lt = s1 - s0, d = Math.max(1, Math.floor(Lt / (n + 1)));
    const onZ = dir[1] === 'z', up = dir[0] === '+';
    const height = band.height || 0, below = band.below || 0;
    const box3 = (u0, u1, ylo, yhi) => onZ ? {min: [a0, ylo, u0], max: [a1, yhi, u1]} : {min: [u0, ylo, a0], max: [u1, yhi, a1]};
    // FRACTIONAL SLOPES (D8): the n + 1 treads share the run exactly. A run
    // that is not a multiple of n + 1 used to end in a flat landing the size of
    // the remainder (a stair that stopped short of its socket and a lip at the
    // top); now `rem` of the treads are one cell deeper, so the run need not
    // divide. Two ramp prims (the deep treads first, from the low end), the
    // same profile the single ramp would have had otherwise.
    const rem = Lt >= (n + 1) ? Lt - (n + 1) * d : 0;
    const Lr = Math.min(Lt, (n + 1) * d + rem);
    const r0 = up ? s0 : s1 - Lr, r1 = up ? s0 + Lr : s1;
    let first;
    if (rem > 0) {
      // treads 0..rem-1 of depth d+1 from the LOW end, then n+1-rem of depth d.
      const LA = rem * (d + 1);
      const lo0 = up ? r0 : r1 - LA, lo1 = up ? r0 + LA : r1;
      const hi0 = up ? r0 + LA : r0, hi1 = up ? r1 : r1 - LA;
      first = this.prim(op, 'ramp', Object.assign(box3(lo0, lo1, y0, y0 + (rem - 1) * h + Math.max(1, height)), {dir, stepH: h, stepD: d + 1, height, below}), o);
      this.prim(op, 'ramp', Object.assign(box3(hi0, hi1, y0 + rem * h, y0 + RR + Math.max(1, height)), {dir, stepH: h, stepD: d, height, below}), o);
    } else {
      first = this.prim(op, 'ramp', Object.assign(box3(r0, r1, y0, y0 + RR + Math.max(1, height)), {dir, stepH: h, stepD: d, height, below}), o);
    }
    if (Lr < Lt) {
      const t0 = up ? s0 + Lr : s0, t1 = up ? s1 : s1 - Lr;
      this.prim(op, 'box', box3(t0, t1, y0 + RR - below, y0 + RR + height), Object.assign({}, o, {tag: (o && o.tag) || 'flat'}));
    }
    return {rise: RR, prim: first, stepD: d, rem, profile: (u) => {
      // walking-surface height (above y0) at an along-coordinate u (absolute)
      const v = up ? u - r0 : r1 - 1 - u;
      if (v < 0) return 0;
      const LA = rem * (d + 1);
      const k = v < LA ? Math.floor(v / (d + 1)) : rem + Math.floor((v - LA) / d);
      return Math.min(n, k) * h;
    }};
  }

  /** A chain of waynodes prefix0..N-1 through pts; the first also links
   *  `first`, the last `last` (names or 'socket:<name>'). Returns the names. */
  wayChain(prefix, pts, first, last, extra) {
    const names = pts.map((_, i) => prefix + i);
    pts.forEach((pt, i) => {
      const links = [];
      if (i > 0) links.push(names[i - 1]);
      if (i + 1 < pts.length) links.push(names[i + 1]);
      if (i === 0 && first) links.push(...first);
      if (i === pts.length - 1 && last) links.push(...last);
      this.way(names[i], pt, links, extra);
    });
    return names;
  }

  /** A ladder prop of any length, split into stacked segments of at most
   *  20 m (a prop template is at most 256 cells on an axis, PLAN 3.4.1).
   *  pos = the foot; only the top segment gets the rope anchor. */
  ladder(propId, totalM, pos, yaw, params) {
    const seg = 20;
    const n = Math.max(1, Math.ceil(totalM / seg - 1e-6));
    for (let k = 0; k < n; k++) {
      const len = Math.min(seg, +(totalM - k * seg).toFixed(2));
      const pp = Object.assign({}, params || {}, {length: len});
      if (propId === 'caves/rope_ladder') pp.anchor = k === n - 1;
      this.prop(propId, pp, [pos[0], pos[1] + k * seg * this.vpm, pos[2]], yaw);
    }
  }

  socket(name, pos, dir, w, h, profile, tags) {
    this.sockets.push({name, pos: pos.map(Math.round), dir: dirVec(dir), w: Math.round(w), h: Math.round(h),
                       profile: profile || 'rect', tags: tags || []});
  }
  prop(prop, params, pos, yaw, extra) {
    this.props.push(Object.assign({prop, params: params || {}, pos: pos.map(Math.round), yaw: ((yaw || 0) % 360 + 360) % 360}, extra || {}));
  }
  ref(kind, pos, fields) { this.refs.push(Object.assign({kind, name: kind + this.refs.length, pos: pos.map(Math.round), yaw: 0}, fields || {})); }
  way(name, pos, links, extra) { this.waynodes.push(Object.assign({name, pos: pos.map(Math.round), links: links || []}, extra || {})); }
  /** A light mount point. mount: wall|ceil|floor; `out` = direction the mount
   *  faces into the room (for wall mounts: the wall normal). */
  lightSpot(pos, mount, out) { this.lightSpots.push({pos: pos.map(Math.round), mount, out: out || '+y'}); }

  /** Apply the universal sliders and return the generator result (§3.3). */
  finish(extra) {
    const P = this.P;
    // Decay + moisture scatter on every room volume, the same for every kit.
    this.prims.forEach((p, i) => {
      if (p.tag !== 'room' || p.op !== 'carve') return;
      if (P.decay > 0) this.scatter.push({prim: i, kind: 'rubble', density: +(P.decay * 0.6).toFixed(3), size: [1, 2 + Math.round(P.decay * 4)], surface: 'floor', mat: 'rubble', seedKey: 'decay'});
      if (P.moisture > 0) {
        this.scatter.push({prim: i, kind: 'moss', density: +(P.moisture * 0.9).toFixed(3), size: [1, 3], surface: 'wall', mat: 'moss', seedKey: 'moss'});
        this.scatter.push({prim: i, kind: 'puddle', density: +(P.moisture * 0.15).toFixed(3), size: [3, 6 + Math.round(P.moisture * 10)], surface: 'floor', mat: 'liquid', seedKey: 'puddle'});
      }
    });
    this.placeLights();
    const res = {
      prims: this.prims, scatter: this.scatter, sockets: this.sockets, props: this.props,
      refs: this.refs, waynodes: this.waynodes, lightSpots: this.lightSpots,
      surface: this.surface, flooded: this.flooded, notes: this.notes
    };
    Object.assign(res, extra || {});
    let bb = null;
    for (const p of this.prims) bb = boxUnion(bb, primReach(p));
    for (const s of this.sockets) bb = boxUnion(bb, {min: s.pos, max: s.pos.map(v => v + 1)});
    res.bounds = bb || {min: [0, 0, 0], max: [1, 1, 1]};
    return res;
  }

  /** Choose P.lightCount of the mount points. The ORDER of the candidates is a
   *  seeded shuffle keyed per candidate, so raising the count adds lights and
   *  never moves the ones already there. */
  placeLights() {
    const P = this.P;
    if (P.lightType === 'none' || !this.lightSpots.length) return;
    // How many: the count, or more by floor area (lightDensity). The area is the
    // XZ extent of the room carves; one light per ~25 m2 at density 1.
    let want = P.lightCount;
    if ((P.lightDensity || 0) > 0) {
      let bb = null;
      for (const p of this.prims) if (p.op === 'carve' && p.tag === 'room') bb = boxUnion(bb, shapeBounds(p));
      const area = bb ? ((bb.max[0] - bb.min[0]) * (bb.max[2] - bb.min[2])) / (this.vpm * this.vpm) : 0;
      want = Math.max(want, Math.round((P.lightDensity * area) / 25));
    }
    if (want <= 0) return;
    const pw = P.lightPower || 0;
    // Rough walls / floors: a light's root must reach the rock past the noise.
    const wallAmp = (this.wallNoise() || {amp: 0}).amp, floorAmp = (this.floorNoise() || {amp: 0}).amp;
    const order = this.lightSpots.map((s, i) => ({s, i, k: this.r('lightpick', i)})).sort((a, b) => a.k - b.k || a.i - b.i);
    const n = Math.min(want, order.length);
    for (let j = 0; j < n; j++) {
      const {s, i} = order[j];
      const warm = this.r('lightwarm', i) < P.warmth;
      const slot = warm ? 'light' : 'lightCool';
      const L = (this.kit.lights && this.kit.lights[P.lightType]) || {};
      const up = dirVec(s.out);
      const p = s.pos;
      const into = s.mount === 'floor' ? floorAmp + 2 : wallAmp + 2;      // how far a root must reach behind the mount
      if (P.lightType === 'torch') {
        // A bracket (support) out of the wall and a flame on it; the bracket's
        // root runs back into the wall past its noise. lightPower widens the
        // flame to a fire basket (up to 4 x 6 x 4 voxels).
        const fw = 1 + Math.round(pw * 3), fh = 2 + Math.round(pw * 4);
        const back = pw > 0 ? Math.round(into) : 0;
        const reach = 1 + Math.round(pw * 2);
        const base = [p[0] + up[0] * reach, p[1], p[2] + up[2] * reach];
        const root = [p[0] - up[0] * back, p[1], p[2] - up[2] * back];
        this.fill('box', {min: [Math.min(root[0], base[0]), p[1] - 1, Math.min(root[2], base[2])], max: [Math.max(root[0], base[0]) + 1, p[1], Math.max(root[2], base[2]) + 1]}, {mat: 'support', layer: 3, noise: null, tag: 'light'});
        const h0 = Math.floor((fw - 1) / 2);
        const fx0 = base[0] - (up[0] ? (up[0] > 0 ? 0 : fw - 1) : h0), fz0 = base[2] - (up[2] ? (up[2] > 0 ? 0 : fw - 1) : h0);
        if (pw > 0) this.fill('box', {min: [fx0, p[1] - 1, fz0], max: [fx0 + fw, p[1], fz0 + fw]}, {mat: 'iron', layer: 3, noise: null, tag: 'light'});
        this.fill('box', {min: [pw > 0 ? fx0 : base[0], p[1], pw > 0 ? fz0 : base[2]], max: [(pw > 0 ? fx0 : base[0]) + fw, p[1] + fh, (pw > 0 ? fz0 : base[2]) + fw]}, {mat: slot, layer: 3, noise: null, tag: 'light'});
      } else if (P.lightType === 'crystal') {
        const r = Math.round((2 + Math.floor(this.r('lightsz', i) * 3)) * (1 + 1.6 * pw));
        const mat = pw > 0 ? (warm ? 'crystal' : 'lightCool') : (warm ? 'crystal_spire' : 'lightCool');
        if (pw > 0 || into > 3) {
          // ANCHORED: a capsule from inside the rock behind the mount out to
          // the lump, so a rough wall or floor never leaves it an island.
          const a = [p[0] - up[0] * Math.round(into + r * 0.5), p[1] - up[1] * Math.round(into + r * 0.5), p[2] - up[2] * Math.round(into + r * 0.5)];
          this.fill('capsule', {a, b: [p[0] + up[0] * Math.round(r * 0.3), p[1] + up[1] * Math.round(r * 0.3), p[2] + up[2] * Math.round(r * 0.3)], r}, {mat, layer: 3, noise: null, tag: 'light'});
        } else this.fill('ellipsoid', {min: [p[0] - r, p[1] - r, p[2] - r], max: [p[0] + r, p[1] + r + 1, p[2] + r]}, {mat, layer: 3, noise: null, tag: 'light'});
      } else if (L.prop) {
        const yaw = s.out === '+x' ? 90 : s.out === '-x' ? 270 : s.out === '-z' ? 180 : 0;
        const pp = Object.assign({}, L.params || {}, {lit: slot, mount: s.mount});
        if (pw > 0) Object.assign(pp, L.power ? L.power(pw) : {});
        this.prop(L.prop, pp, s.pos, yaw, {light: true});
        if (s.mount === 'floor' && floorAmp >= 1) this.footing(s.pos, 3, floorAmp + 3, null, 'light');
      }
      this.ref('light', p, {props: {type: P.lightType, warm}});
    }
  }
}

/**
 * Register piece types on a kit. Each def: {type, label, version, surface?,
 * size, u, rows, PRESETS, build(b, P), migrate?(params, fromVersion)}.
 * Adds to each def: SCHEMA (universal rows + the type's own), defaults(),
 * normalize(src), generate(params, seed, ctx) -> the §3.3 result.
 */
export function defineKit(kit, defs) {
  kit.TYPES = kit.TYPES || {};
  kit.TYPE_ORDER = kit.TYPE_ORDER || [];
  for (const t of defs) {
    t.kit = kit.name;
    t.SCHEMA = universalSchema(t.size, t.u, kit.lightTypes, {passage: !!t.passage, room: !!t.room, slope: !!t.slope}).concat(t.rows || []);
    t.defaults = () => defaultsOf(t.SCHEMA);
    t.normalize = (src) => normalizeParams(t.SCHEMA, src);
    t.generate = (params, seed, ctx) => {
      const P = t.normalize(params);
      const b = new Builder(kit, t.type, P, (seed >>> 0), ctx);
      b.surface = !!t.surface;
      t.build(b, P);
      const res = b.finish();
      res.kit = kit.name; res.type = t.type; res.version = t.version; res.params = P; res.seed = seed >>> 0;
      return res;
    };
    kit.TYPES[t.type] = t;
    kit.TYPE_ORDER.push(t.type);
  }
  return kit;
}

/** Clamp helper. */
export const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
