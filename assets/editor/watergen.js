/* watergen.js — the ONE body-of-water shaper.
 *
 * WHAT A "WATER BODY" IS HERE. A closed basin authored as a PRESET: a footprint
 * (how the shoreline is drawn), a bathymetry (how deep it is where), what fills
 * it, what the bed and bank are made of, and what grows in the depth bands the
 * shape produces. A tarn, a kettle, a marsh, a desert playa, a lava crater and
 * the authored spawn lake are all the same object with different numbers, the
 * way an oak and a spruce are the same object to treegen.js.
 *
 * WHY IT IS A SEPARATE MODULE FROM THE ENGINE'S POND. Today worldgen.wgsl grows
 * a pond as a parabolic disc on a tile hash (pondAt), and that formula lives
 * inside a CPU-mirrored block that check_invariants.py token-compares against
 * world.cpp. The authoring surface for ponds is therefore fourteen scalar
 * tuning rows and nothing else — one shape, world-wide. This module is the
 * authoring surface the engine does NOT yet have: a preset per KIND of water
 * body, previewable in the tuner byte-for-byte, and a biome row that says which
 * presets appear where and how often. Wiring the engine to read the preset
 * table is the follow-up (docs/PLAN_biomes.md §5 names the seams); until then
 * the tuner is the only consumer, which is the honest state of a scaffold.
 *
 * PURE MODULE. No DOM, no fetch, no WebGL. `scripts/test_watergen.mjs` imports it
 * under Node with nothing but `fs`, exactly like treegen.js.
 *
 * NO Math.random(), ANYWHERE. Every random number is `frand(...)` over a
 * counter-based hash keyed on the COLUMN and a salt, so nudging one slider does
 * not reshuffle the reed bed you were looking at, and the same params + seed
 * give byte-identical cells on any machine.
 *
 * THE SHAPE LANGUAGE (docs/PLAN_biomes.md §3 has the sources):
 *   footprint  = superellipse (Lamé curve, `squareness` is the exponent: 2 is
 *                an ellipse, 4 a squircle, 1.2 a diamond) with a domain-warped
 *                radius (fBm) for an organic shore, plus optional LOBES — extra
 *                ellipses smooth-min'd onto the main one — for bays that one
 *                warped disc can never make. Optional ISLANDS subtract.
 *   bathymetry = a depth PROFILE CURVE over the normalised radius u (0 at the
 *                deepest point, 1 at the shoreline): a parabola, a flat-floored
 *                kettle, a littoral shelf and a drop-off are all this one curve
 *                with different control points. Depth is then `rimDepth +
 *                (depth - rimDepth) * profile(u)`, plus a little floor noise.
 *   fill       = a material (water / lava / oil / none) at `surface + level`.
 *   berm       = the engine's structural containment: ground just outside the
 *                shore is FORCED above the waterline, then ramps back down.
 *   bands      = vegetation by DEPTH — emergent (reeds) in the shallow margin,
 *                floating (lilies) over the mid-depths, submerged (weeds) in the
 *                deep — and by DISTANCE PAST THE SHORE on land (mud, cattail,
 *                sedge, iris). Band WIDTHS are consequences of the shore slope,
 *                not knobs: a steep kettle has a one-voxel reed fringe and a
 *                marsh is all fringe.
 *
 * COORDINATES are the engine's: Y up, X/Z horizontal, one cell = one voxel.
 * Every length below is authored in METRES; `opts.vpm` converts (defaults to
 * DEFAULT_VOX_PER_M, and the tab passes the engine's real scale).
 */

'use strict';

// =============================================================================
// constants
// =============================================================================

export const DEFAULT_VOX_PER_M = 10;
/** Hard cap on a preview grid's edge, in cells. A 60 m lake with fringes at
 *  10 vpm is ~700 cells across; the cap keeps a runaway slider from asking the
 *  browser for a gigabyte. The generator CLIPS and says so in meta. */
export const MAX_DIM = 640;
export const MAX_HEIGHT = 160;
/** Liquid fullness nibble for a full cell (the engine's LIQ_FULL_STATE). */
export const LIQ_FULL = 8;

// =============================================================================
// hashing — the same lowbias32 mixer treegen.js uses
// =============================================================================

function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0; x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0; x = Math.imul(x, 0x846ca68b) >>> 0;
  return (x ^ (x >>> 16)) >>> 0;
}
export function hashN(...vals) {
  let h = 0x9e3779b9;
  for (const v of vals) h = mix32((h ^ ((v | 0) >>> 0)) >>> 0) ^ 0x85ebca6b;
  return mix32(h);
}
/** Uniform [0,1). */
function frand(...vals) { return hashN(...vals) / 4294967296; }
/** Uniform [-1,1). */
function frandS(...vals) { return frand(...vals) * 2 - 1; }

// =============================================================================
// value noise — smooth, lattice-hashed, deterministic
// =============================================================================

function smooth(t) { return t * t * (3 - 2 * t); }

function vnoise2(x, z, seed) {
  const xi = Math.floor(x), zi = Math.floor(z);
  const fx = smooth(x - xi), fz = smooth(z - zi);
  const a = frand(seed, xi, zi), b = frand(seed, xi + 1, zi);
  const c = frand(seed, xi, zi + 1), d = frand(seed, xi + 1, zi + 1);
  return (a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz;
}

/** fBm in [-1, 1], `oct` octaves, lacunarity 2, persistence 0.5. */
function fbm2(x, z, oct, seed) {
  let sum = 0, amp = 1, norm = 0;
  for (let o = 0; o < oct; o++) {
    sum += (vnoise2(x, z, seed + o * 131) * 2 - 1) * amp;
    norm += amp; amp *= 0.5; x = x * 2 + 17.3; z = z * 2 - 9.1;
  }
  return sum / norm;
}

// =============================================================================
// parameters
// =============================================================================

/**
 * Every authored value, with the default that makes a plain tarn. Lengths in
 * metres, chances as "1 in N" integers (0 = never), depths positive-down.
 */
export function defaultParams() {
  return {
    name: 'tarn',
    displayName: 'Tarn',
    kind: 'lake',                  // a label for the biome page: lake|pond|marsh|pool|dry
    footprint: {
      radius: 6.4,                 // metres, half the mean width
      radiusV: 3.2,                // per-instance jitter added on top (0..V)
      aspect: 1.0,                 // >1 stretches along the body's own X
      squareness: 2.0,             // superellipse exponent: 2 ellipse, 4 squircle
      rotation: 0,                 // degrees; the biome row may randomise
      rotationRandom: true,
      warpAmp: 0.14,               // fraction of radius the shore wanders
      warpFreq: 2.2,               // fBm cycles across the diameter
      warpOctaves: 3,
      lobes: 0,                    // extra ellipses smooth-min'd on
      lobeRadius: 0.55,            // x radius
      lobeSpread: 0.75,            // centre offset, x radius
      lobeWeld: 0.35,              // smooth-min radius, x radius
      islands: 0,
      islandRadius: 0.22,          // x radius
      islandHeight: 0.6            // metres above the waterline at the dome peak
    },
    bathymetry: {
      depth: 2.6,                  // metres at the deepest point
      rimDepth: 0.3,               // metres of drop right at the shoreline
      // profile(u): u 0 = deepest point, 1 = shoreline; value 1 = full depth,
      // 0 = rim depth. Monotone cubic through these points. This is the
      // parabola the engine draws today.
      profile: [[0, 1], [0.5, 0.75], [1, 0]],
      floorNoise: 0.15,            // metres of fBm on the bed, fading to 0 at the shore
      floorNoiseFreq: 3.0
    },
    fill: {
      material: 'water',           // '' or 'none' for a dry basin
      level: 0.0,                  // metres relative to the rim ground; negative = part-full
      surfaceMaterial: ''          // optional skin ON the surface (e.g. ice); '' = none
    },
    berm: {
      height: 0.5,                 // forced lift of the bank above the waterline
      width: 1.4,                  // metres over which it ramps back to natural ground
      coreFrac: 0.25               // inner fraction held flat at full height
    },
    bed: {
      shallow: 'sand',             // bed material where depth < shallowDepth
      deep: 'shore_mud',           // bed material in the deep
      shallowDepth: 0.8,
      thickness: 0.3,              // metres of bed material over the substrate
      substrate: 'stone'
    },
    ground: {
      skin: 'grass',               // the natural ground the body is cut into
      soil: 'dirt',
      soilDepth: 1.2,
      rock: 'stone',
      relief: 0.25,                // PREVIEW ONLY: metres of gentle ground noise
      reliefFreq: 1.2
    },
    shore: {
      band: 2.4,                   // metres past the shoreline the wet fringe reaches
      lift: 1.2,                   // metres above the waterline ground may stand and still be shore
      mudWidth: 1.0,               // inner ring that wears the mud skin
      mudMaterial: 'shore_mud',
      mossChance: 3,               // 1-in-N of mud-ring stone/soil wearing wet moss
      mossMaterial: 'wet_moss',
      // Distance-ordered from the water: each species has a REACH (metres past
      // the shore it will grow), a chance and a height in cells.
      plants: [
        {material: 'cattail', head: 'cattail_head', chance: 12, reach: 0.9, height: 2.0},
        {material: 'horsetail', head: '', chance: 10, reach: 1.6, height: 0.9},
        {material: 'water_iris', head: '', chance: 34, reach: 2.0, height: 0.5},
        {material: 'marsh_grass', head: '', chance: 4, reach: 2.4, height: 0.3}
      ]
    },
    aquatic: {
      // Depth bands, in metres of water over the bed.
      emergent: {material: 'reed', chance: 130, minDepth: 0.4, maxDepth: 1.4, height: 1.6},
      floating: {material: 'lilypad', flower: 'lily_flower', chance: 22, flowerChance: 5,
                 minDepth: 1.0, maxDepth: 3.0},
      submerged: {material: 'kelp', chance: 120, minDepth: 1.6, height: 1.0, clearance: 0.4}
    },
    placement: {
      // Defaults a biome row inherits (the row may override every one).
      tile: 44.8,                  // metres between candidate sites
      rarity: 4,                   // 1 in N tiles actually gets one
      maxSlope: 96,                // Q8 landform slope gate (256 = repose)
      minY: -1, maxY: -1           // ground band, -1 = unbounded
    },
    preview: {
      margin: 1.0                  // metres of plain ground past the outermost fringe
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

/** Deep-merge a (possibly partial) file onto the defaults and pin the bake scale. */
export function normalizeParams(src, vpm) {
  const p = merge(defaultParams(), src || {});
  // The profile has to start at the centre and end at the shore or the depth
  // function has a hole in it; sort, clamp, and pin the ends.
  p.bathymetry.profile = sanitizeProfile(p.bathymetry.profile);
  if (!Array.isArray(p.shore.plants)) p.shore.plants = [];
  p.vpm = vpm || (src && src.vpm) || DEFAULT_VOX_PER_M;
  return p;
}

export function sanitizeProfile(pts) {
  let out = (Array.isArray(pts) ? pts : [])
      .filter(q => Array.isArray(q) && q.length >= 2 && isFinite(q[0]) && isFinite(q[1]))
      .map(q => [Math.min(1, Math.max(0, +q[0])), Math.min(1, Math.max(0, +q[1]))])
      .sort((a, b) => a[0] - b[0]);
  if (!out.length) out = [[0, 1], [1, 0]];
  if (out[0][0] > 0) out.unshift([0, out[0][1]]);
  if (out[out.length - 1][0] < 1) out.push([1, out[out.length - 1][1]]);
  out[0][0] = 0; out[out.length - 1][0] = 1;
  // Drop exact duplicate x so the cubic has finite slopes.
  const dedup = [out[0]];
  for (let i = 1; i < out.length; i++)
    if (out[i][0] - dedup[dedup.length - 1][0] > 1e-4) dedup.push(out[i]);
    else dedup[dedup.length - 1] = out[i];
  return dedup;
}

// =============================================================================
// the depth profile — monotone cubic (Fritsch–Carlson) through the points
// =============================================================================

/**
 * Evaluate the profile at u in [0,1]. Returns the depth FRACTION (1 at the
 * deepest, 0 at the shore). Monotone cubic, so a shelf authored as two points
 * at the same depth stays flat instead of overshooting into a ridge.
 */
export function profileAt(pts, u) {
  const n = pts.length;
  if (n === 1) return pts[0][1];
  u = Math.min(1, Math.max(0, u));
  // tangents
  const d = new Array(n - 1), m = new Array(n);
  for (let i = 0; i < n - 1; i++) {
    const h = pts[i + 1][0] - pts[i][0];
    d[i] = h > 0 ? (pts[i + 1][1] - pts[i][1]) / h : 0;
  }
  m[0] = d[0]; m[n - 1] = d[n - 2];
  for (let i = 1; i < n - 1; i++) {
    if (d[i - 1] * d[i] <= 0) m[i] = 0;
    else {
      const w1 = 2 * (pts[i + 1][0] - pts[i][0]) + (pts[i][0] - pts[i - 1][0]);
      const w2 = (pts[i + 1][0] - pts[i][0]) + 2 * (pts[i][0] - pts[i - 1][0]);
      m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
    }
  }
  let i = 0;
  while (i < n - 2 && u > pts[i + 1][0]) i++;
  const h = pts[i + 1][0] - pts[i][0];
  if (h <= 0) return pts[i][1];
  const t = (u - pts[i][0]) / h;
  const t2 = t * t, t3 = t2 * t;
  const h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
  const h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
  const v = h00 * pts[i][1] + h10 * h * m[i] + h01 * pts[i + 1][1] + h11 * h * m[i + 1];
  return Math.min(1, Math.max(0, v));
}

// =============================================================================
// the footprint field
// =============================================================================

/** Polynomial smooth-min (iq). Binary and NOT associative: fold left, always. */
function smin(a, b, k) {
  if (k <= 0) return Math.min(a, b);
  const h = Math.min(1, Math.max(0, 0.5 + 0.5 * (b - a) / k));
  return b * (1 - h) + a * h - k * h * (1 - h);
}

/**
 * Build the per-instance footprint: radius roll, rotation roll, lobe and island
 * placements. Everything an INSTANCE decides once, so the per-column field
 * evaluation below is pure arithmetic.
 */
export function instanceOf(P, seed) {
  const F = P.footprint;
  const R = F.radius + F.radiusV * frand(seed, 0x51, 1);
  const rot = (F.rotationRandom ? frand(seed, 0x52, 2) * 360 : 0) + F.rotation;
  const c = Math.cos(rot * Math.PI / 180), s = Math.sin(rot * Math.PI / 180);
  const a = R * Math.sqrt(Math.max(0.05, F.aspect));
  const b = R / Math.sqrt(Math.max(0.05, F.aspect));
  const lobes = [];
  const nl = Math.max(0, Math.min(8, F.lobes | 0));
  for (let i = 0; i < nl; i++) {
    // Spread the lobes around the rim with a little jitter, never stacked.
    const ang = (i / nl) * Math.PI * 2 + frandS(seed, 0x53, i) * (Math.PI / nl) * 0.6;
    const dist = R * F.lobeSpread * (0.85 + 0.3 * frand(seed, 0x54, i));
    const lr = R * F.lobeRadius * (0.8 + 0.4 * frand(seed, 0x55, i));
    lobes.push({cx: Math.cos(ang) * dist, cz: Math.sin(ang) * dist,
                a: lr * (0.8 + 0.4 * frand(seed, 0x56, i)),
                b: lr * (0.8 + 0.4 * frand(seed, 0x57, i))});
  }
  const islands = [];
  const ni = Math.max(0, Math.min(6, F.islands | 0));
  for (let i = 0; i < ni; i++) {
    const ang = frand(seed, 0x58, i) * Math.PI * 2;
    const dist = R * (0.15 + 0.45 * frand(seed, 0x59, i));
    const ir = R * F.islandRadius * (0.7 + 0.6 * frand(seed, 0x5a, i));
    islands.push({cx: Math.cos(ang) * dist, cz: Math.sin(ang) * dist, r: ir});
  }
  // Outer reach in metres: the farthest any water can be from the centre.
  const lobeReach = nl ? R * (F.lobeSpread * 1.15 + F.lobeRadius * 1.2) : 0;
  const reach = Math.max(R * Math.sqrt(Math.max(F.aspect, 1 / F.aspect)), lobeReach) *
                (1 + Math.abs(F.warpAmp) * 1.3) + R * 0.05;
  return {R, rot, c, s, a, b, lobes, islands, reach, weld: R * F.lobeWeld};
}

/**
 * The footprint field at metres (x, z) from the centre. Returns
 *   { u, island }
 * where u < 1 is inside the water (0 at the deepest point, 1 exactly on the
 * shore), u > 1 is land with (u - 1) * R roughly the metres past the shore,
 * and `island` is the dome height fraction (0 = not on an island).
 */
export function fieldAt(P, I, x, z, seed) {
  const F = P.footprint;
  // Domain warp: displace the sample point by a vector fBm scaled to the
  // radius, so the shore wanders in metres proportional to the body.
  if (F.warpAmp !== 0) {
    const f = F.warpFreq / Math.max(0.5, 2 * I.R);
    const wx = fbm2(x * f + 3.1, z * f - 7.7, F.warpOctaves | 0 || 1, seed ^ 0x77);
    const wz = fbm2(x * f - 5.3, z * f + 2.9, F.warpOctaves | 0 || 1, seed ^ 0x78);
    x += wx * F.warpAmp * I.R; z += wz * F.warpAmp * I.R;
  }
  // Rotate into the body's frame.
  const lx = x * I.c + z * I.s, lz = -x * I.s + z * I.c;
  const n = Math.max(0.6, F.squareness);
  let s = Math.pow(Math.pow(Math.abs(lx / I.a), n) + Math.pow(Math.abs(lz / I.b), n), 1 / n);
  // Lobes are plain ellipses (n = 2) smooth-min'd onto the main field.
  for (const L of I.lobes) {
    const dx = (lx - L.cx) / L.a, dz = (lz - L.cz) / L.b;
    const sl = Math.sqrt(dx * dx + dz * dz);
    s = smin(s, sl, I.weld / Math.max(0.01, I.R));
  }
  let island = 0;
  for (const S of I.islands) {
    const dx = lx - S.cx, dz = lz - S.cz;
    const d = Math.sqrt(dx * dx + dz * dz) / S.r;
    if (d < 1) island = Math.max(island, 1 - d * d);
  }
  return {u: s, island};
}

// =============================================================================
// generation
// =============================================================================

/** Resolve every material NAME in a preset to a local palette index (1-based
 *  into `names`; 0 = air/none). Shared by the standalone voxeliser and by the
 *  biome swatch, which stamps several presets into ONE palette. */
export function paletteOf(P, names, idx) {
  names = names || []; idx = idx || new Map();
  const nameId = (nm) => {
    if (!nm || nm === 'none') return 0;
    let i = idx.get(nm);
    if (i === undefined) { names.push(nm); i = names.length; idx.set(nm, i); }
    return i;
  };
  const M = {
    fill: nameId(P.fill.material), surfSkin: nameId(P.fill.surfaceMaterial),
    bedShallow: nameId(P.bed.shallow), bedDeep: nameId(P.bed.deep),
    substrate: nameId(P.bed.substrate), skin: nameId(P.ground.skin),
    soil: nameId(P.ground.soil), rock: nameId(P.ground.rock),
    mud: nameId(P.shore.mudMaterial), moss: nameId(P.shore.mossMaterial),
    emergent: nameId(P.aquatic.emergent.material),
    floating: nameId(P.aquatic.floating.material),
    flower: nameId(P.aquatic.floating.flower),
    submerged: nameId(P.aquatic.submerged.material),
    plants: P.shore.plants.map(pl => ({...pl, id: nameId(pl.material), headId: nameId(pl.head)})),
    names, idx
  };
  M.wet = M.fill !== 0;
  return M;
}

export const ZONE = {LAND: 0, SHORE: 1, EMERGENT: 2, FLOATING: 3, OPEN: 4, BERM: 5, ISLAND: 6,
                     MUD: 7, DRYBED: 8};

/** The palette-jitter state nibble worldgen gives a solid cell. */
export function solidWord(id, seed, x, z) {
  return id ? (id | ((hashN(seed, x, z, 0x33) % 3) << 12)) : 0;
}

/**
 * Decide ONE column of a water body. This is the whole shape: the standalone
 * preview and the biome swatch both call it, so there is one answer to "what
 * is at (x, z) of a tarn".
 *
 *   P, I, M   preset, instance (instanceOf), palette (paletteOf)
 *   mx, mz    metres from the body's centre
 *   natural   the natural ground's topmost cell Y at this column
 *   surf      the rim ground Y the body was cut at (its waterline datum)
 *   x, z      integer column keys for the hash (any consistent lattice)
 *
 * Returns null when the column is untouched (past every fringe), else
 *   { top, skin, floor, bed, bedBottom, waterTop, surfSkin, zone, depthM,
 *     plants:[{id, name, y0, y1}], berm, u }
 * where `top` is the topmost GROUND cell (land columns; -1 in the basin),
 * `floor` the topmost BED cell (basin columns; -1 on land) and `waterTop` the
 * topmost water cell or -1.
 */
export function columnAt(P, I, M, mx, mz, natural, surf, x, z, seed) {
  const vpm = P.vpm, cellM = 1 / vpm;
  const m2v = (m) => Math.round(m * vpm);
  const f = fieldAt(P, I, mx, mz, seed);
  const u = f.u;
  const wet = M.wet;
  const waterY = surf + m2v(P.fill.level);
  const bedV = Math.max(1, m2v(P.bed.thickness));
  const emer = P.aquatic.emergent, flo = P.aquatic.floating, sub = P.aquatic.submerged;
  const out = {top: natural, skin: M.skin, floor: -1, bed: 0, bedBottom: -1, waterTop: -1,
               surfSkin: 0, zone: ZONE.LAND, depthM: 0, plants: [], berm: false, u};

  if (u < 1 && f.island <= 0) {
    // ---- inside the basin ---------------------------------------------------
    const frac = profileAt(P.bathymetry.profile, u);
    let dM = P.bathymetry.rimDepth + (P.bathymetry.depth - P.bathymetry.rimDepth) * frac;
    if (P.bathymetry.floorNoise > 0) {
      const fn = fbm2(mx * P.bathymetry.floorNoiseFreq / 4 - 8.8,
                      mz * P.bathymetry.floorNoiseFreq / 4 + 6.6, 3, seed ^ 0x5b);
      dM += fn * P.bathymetry.floorNoise * (1 - u * u);
    }
    const floor = surf - Math.max(1, m2v(dM));
    const waterDepthCells = wet ? waterY - floor : 0;
    const wdM = waterDepthCells * cellM;
    const shallow = wet && wdM < P.bed.shallowDepth;
    out.top = -1;
    out.floor = floor;
    out.bedBottom = floor - bedV + 1;
    out.bed = (shallow || !wet) ? M.bedShallow : M.bedDeep;
    out.depthM = wet ? wdM : -(surf - floor) * cellM;
    if (!wet || waterDepthCells <= 0) { out.zone = ZONE.DRYBED; return out; }
    out.waterTop = waterY;
    out.surfSkin = M.surfSkin;
    out.zone = ZONE.OPEN;
    if (M.emergent && emer.chance > 0 && wdM >= emer.minDepth && wdM <= emer.maxDepth) {
      out.zone = ZONE.EMERGENT;
      if (hashN(seed, x, z, 0x61) % emer.chance === 0)
        out.plants.push({id: M.emergent, name: emer.material, y0: floor + 1,
                         y1: floor + Math.max(1, m2v(emer.height))});
    } else if (M.floating && flo.chance > 0 && wdM >= flo.minDepth && wdM <= flo.maxDepth) {
      out.zone = ZONE.FLOATING;
      if (hashN(seed, x, z, 0x62) % flo.chance === 0) {
        out.plants.push({id: M.floating, name: flo.material, y0: waterY, y1: waterY});
        if (M.flower && flo.flowerChance > 0 && hashN(seed, x, z, 0x63) % flo.flowerChance === 0)
          out.plants.push({id: M.flower, name: flo.flower, y0: waterY + 1, y1: waterY + 1});
      }
    }
    if (M.submerged && sub.chance > 0 && wdM >= sub.minDepth &&
        hashN(seed, x, z, 0x64) % sub.chance === 0) {
      const cap = waterY - Math.max(1, m2v(sub.clearance));
      const top = Math.min(cap, floor + Math.max(1, m2v(sub.height)));
      if (top > floor) out.plants.push({id: M.submerged, name: sub.material, y0: floor + 1, y1: top});
    }
    return out;
  }

  if (f.island > 0 && u < 1) {
    // ---- an island: a dome of natural ground above the waterline -------------
    out.top = Math.max(natural, waterY + 1 + Math.round(f.island * P.footprint.islandHeight * vpm));
    out.depthM = -(out.top - surf) * cellM;
    out.zone = ZONE.ISLAND;
    return out;
  }

  // ---- outside the shoreline -------------------------------------------------
  const pastM = (u - 1) * I.R;
  const bermW = P.berm.width;
  if (pastM > Math.max(bermW, P.shore.band)) return null;   // untouched ground
  let top = natural;
  if (wet && P.berm.height > 0 && bermW > 0 && pastM < bermW) {
    const core = bermW * P.berm.coreFrac;
    const t = pastM <= core ? 1 : Math.max(0, 1 - (pastM - core) / Math.max(0.01, bermW - core));
    const forced = waterY + Math.max(1, Math.round(P.berm.height * vpm * t));
    // The engine FORCES the core above the waterline; the ramp blends back
    // into natural ground.
    top = pastM <= core ? Math.max(forced, natural) : Math.round(forced * t + natural * (1 - t));
    top = Math.max(top, natural - 1);
    out.berm = true;
    out.zone = ZONE.BERM;
  }
  const isShore = wet && pastM < P.shore.band && (top - waterY) * cellM <= P.shore.lift;
  if (isShore) {
    out.zone = ZONE.SHORE;
    if (pastM < P.shore.mudWidth && M.mud) {
      out.skin = M.mud; out.zone = ZONE.MUD;
      if (M.moss && P.shore.mossChance > 0 && hashN(seed, x, z, 0x71) % P.shore.mossChance === 0)
        out.skin = M.moss;
    }
    // Distance-ordered plants: the first species whose reach covers this
    // column and whose roll succeeds wins, so cattails crowd the water and
    // sedge takes the outer band.
    for (let k = 0; k < M.plants.length; k++) {
      const pl = M.plants[k];
      if (!pl.id || pl.chance <= 0 || pastM > pl.reach) continue;
      if (hashN(seed, x, z, 0x80 + k) % pl.chance !== 0) continue;
      const h = Math.max(1, m2v(pl.height));
      out.plants.push({id: pl.id, name: pl.material, y0: top + 1, y1: top + h});
      if (pl.headId) out.plants.push({id: pl.headId, name: pl.head, y0: top + h + 1, y1: top + h + 1});
      break;
    }
  }
  out.top = top;
  out.depthM = -(top - surf) * cellM;
  return out;
}

/** Voxelise one instance of a preset. Returns
 *  { dim:{x,y,z}, cells:Uint16Array, anchor:{x,y,z}, names:string[],
 *    meta:{...}, plan:{depth:Float32Array, zone:Uint8Array, nx, nz} }
 *  Cells are `localPaletteIndex | state << 12`, index 1-based into `names`
 *  (0 = air), the same convention as treegen.js so the viewer path is shared. */
export function generateWaterBody(params, seed, opts) {
  const P = normalizeParams(params, opts && opts.vpm);
  const vpm = P.vpm;
  const cellM = 1 / vpm;
  const m2v = (m) => Math.round(m * vpm);
  const I = instanceOf(P, seed);
  const M = paletteOf(P);
  const names = M.names;

  // ---- extents -------------------------------------------------------------
  const fringe = P.berm.width + P.shore.band + P.preview.margin;
  let half = Math.ceil((I.reach + fringe) * vpm) + 1;
  let clipped = false;
  if (2 * half + 1 > MAX_DIM) { half = (MAX_DIM - 1) >> 1; clipped = true; }
  const nx = 2 * half + 1, nz = nx;

  const depthV = m2v(P.bathymetry.depth + P.bathymetry.floorNoise);
  const soilV = m2v(P.ground.soilDepth), bedV = Math.max(1, m2v(P.bed.thickness));
  const rockV = 2;
  const surf = rockV + soilV + bedV + depthV + m2v(P.ground.relief) + 1;   // ground Y at the rim
  let plantTop = 0;
  for (const pl of M.plants) plantTop = Math.max(plantTop, m2v(pl.height) + (pl.headId ? 1 : 0));
  plantTop = Math.max(plantTop, m2v(P.aquatic.emergent.height), m2v(P.footprint.islandHeight) + 2);
  let ny = surf + m2v(P.berm.height) + plantTop + 3;
  if (ny > MAX_HEIGHT) { ny = MAX_HEIGHT; clipped = true; }

  const cells = new Uint16Array(nx * ny * nz);
  const at = (x, y, z) => (z * ny + y) * nx + x;
  const put = (x, y, z, w) => { if (y >= 0 && y < ny) cells[at(x, y, z)] = w; };

  const plan = {depth: new Float32Array(nx * nz), zone: new Uint8Array(nx * nz), nx, nz};
  const waterY = surf + m2v(P.fill.level);
  const meta = {waterCells: 0, areaM2: 0, volumeM3: 0, maxDepthM: 0, sumDepth: 0,
                shoreCells: 0, bermCells: 0, plants: {}, dim: {x: nx, y: ny, z: nz},
                clipped, radiusM: I.R, reachM: I.reach, rotation: I.rot,
                lobes: I.lobes.length, islands: I.islands.length,
                waterlineY: waterY, surfY: surf, wet: M.wet};
  const count = (nm) => { meta.plants[nm] = (meta.plants[nm] || 0) + 1; };

  for (let z = 0; z < nz; z++) {
    for (let x = 0; x < nx; x++) {
      const mx = (x - half) * cellM, mz = (z - half) * cellM;
      const relief = P.ground.relief > 0
          ? fbm2(mx * P.ground.reliefFreq / 4 + 11.1, mz * P.ground.reliefFreq / 4 - 3.3, 3, seed ^ 0x99)
            * P.ground.relief
          : 0;
      const natural = surf + Math.round(relief * vpm);
      const pi = z * nx + x;
      const c = columnAt(P, I, M, mx, mz, natural, surf, x, z, seed);
      // Rock + soil column to `top`, skinned.
      const column = (top, skinId) => {
        for (let y = 0; y < rockV && y <= top; y++) put(x, y, z, solidWord(M.rock, seed, x, z));
        for (let y = rockV; y < top; y++)
          put(x, y, z, solidWord(y >= top - soilV ? M.soil : M.rock, seed, x, z));
        if (top >= 0) put(x, top, z, solidWord(skinId, seed, x, z));
      };
      if (!c) { column(natural, M.skin); plan.depth[pi] = -(natural - surf) * cellM; continue; }
      plan.depth[pi] = c.depthM;
      plan.zone[pi] = c.zone;
      if (c.floor >= 0) {
        for (let y = 0; y < rockV; y++) put(x, y, z, solidWord(M.rock, seed, x, z));
        for (let y = rockV; y < c.bedBottom; y++) put(x, y, z, solidWord(M.substrate, seed, x, z));
        for (let y = Math.max(rockV, c.bedBottom); y <= c.floor; y++) put(x, y, z, solidWord(c.bed, seed, x, z));
        if (c.waterTop >= 0) {
          for (let y = c.floor + 1; y <= c.waterTop; y++) put(x, y, z, M.fill | (LIQ_FULL << 12));
          if (c.surfSkin) put(x, c.waterTop, z, solidWord(c.surfSkin, seed, x, z));
          const d = c.waterTop - c.floor;
          meta.waterCells += d;
          meta.areaM2 += cellM * cellM;
          meta.volumeM3 += d * cellM * cellM * cellM;
          meta.sumDepth += c.depthM;
          if (c.depthM > meta.maxDepthM) meta.maxDepthM = c.depthM;
        }
      } else {
        column(c.top, c.skin);
        if (c.berm) meta.bermCells++;
        if (c.zone === ZONE.SHORE || c.zone === ZONE.MUD) meta.shoreCells++;
      }
      for (const pl of c.plants) {
        for (let y = pl.y0; y <= pl.y1; y++) put(x, y, z, solidWord(pl.id, seed, x, z));
        if (pl.y1 >= pl.y0) count(pl.name);
      }
    }
  }

  meta.meanDepthM = meta.areaM2 > 0 ? meta.sumDepth / (meta.areaM2 * vpm * vpm) : 0;
  delete meta.sumDepth;
  // Volume development (limnology): 3 * mean / max — 1 is a cone, 1.5 a
  // parabola, 3 a bathtub. A one-number description of the profile.
  meta.volumeDevelopment = meta.maxDepthM > 0 ? 3 * meta.meanDepthM / meta.maxDepthM : 0;
  let voxels = 0;
  for (let i = 0; i < cells.length; i++) if (cells[i]) voxels++;
  meta.voxels = voxels;

  return {dim: {x: nx, y: ny, z: nz}, cells, anchor: {x: half, y: surf, z: half},
          names, meta, plan, zones: ZONE};
}

// =============================================================================
// a 1-D profile sampler for the cross-section widget
// =============================================================================

/** Depth in metres at normalised radius u for the preset (no floor noise). */
export function depthAtU(P, u) {
  const B = P.bathymetry;
  return B.rimDepth + (B.depth - B.rimDepth) * profileAt(sanitizeProfile(B.profile), u);
}

// =============================================================================
// presets — one per kind of standing water the world should have
// =============================================================================

export const PRESETS = {
  tarn: {
    name: 'tarn', displayName: 'Tarn', kind: 'lake',
    footprint: {radius: 6.4, radiusV: 3.2, warpAmp: 0.14},
    bathymetry: {depth: 2.6, rimDepth: 0.3, profile: [[0, 1], [0.5, 0.75], [1, 0]]},
    placement: {tile: 44.8, rarity: 4, maxSlope: 96}
  },
  kettle: {
    name: 'kettle', displayName: 'Kettle pond', kind: 'pond',
    footprint: {radius: 4.0, radiusV: 1.5, squareness: 2.4, warpAmp: 0.08, warpFreq: 1.6},
    bathymetry: {depth: 3.2, rimDepth: 0.6,
                 profile: [[0, 1], [0.55, 0.97], [0.8, 0.7], [1, 0]], floorNoise: 0.08},
    bed: {shallow: 'gravel', deep: 'shore_mud', shallowDepth: 0.6},
    shore: {band: 1.4, mudWidth: 0.5,
            plants: [{material: 'horsetail', head: '', chance: 8, reach: 1.0, height: 0.9},
                     {material: 'marsh_grass', head: '', chance: 5, reach: 1.4, height: 0.3}]},
    aquatic: {emergent: {chance: 0}, floating: {chance: 40, minDepth: 1.2, maxDepth: 2.6},
              submerged: {chance: 60, minDepth: 1.4, height: 1.2}},
    placement: {tile: 38.4, rarity: 6, maxSlope: 64}
  },
  marsh: {
    name: 'marsh', displayName: 'Marsh', kind: 'marsh',
    footprint: {radius: 9.0, radiusV: 4.0, aspect: 1.5, squareness: 1.6, warpAmp: 0.32,
                warpFreq: 3.4, warpOctaves: 4, lobes: 3, lobeRadius: 0.5, lobeSpread: 0.9,
                islands: 2, islandRadius: 0.18, islandHeight: 0.3},
    bathymetry: {depth: 0.9, rimDepth: 0.1,
                 profile: [[0, 1], [0.3, 0.95], [0.7, 0.7], [1, 0]], floorNoise: 0.2},
    berm: {height: 0.2, width: 0.8},
    bed: {shallow: 'shore_mud', deep: 'shore_mud', shallowDepth: 2.0, thickness: 0.5},
    shore: {band: 4.0, lift: 0.6, mudWidth: 2.2, mossChance: 2,
            plants: [{material: 'cattail', head: 'cattail_head', chance: 5, reach: 2.0, height: 2.0},
                     {material: 'reed', head: '', chance: 6, reach: 2.6, height: 1.4},
                     {material: 'water_iris', head: '', chance: 14, reach: 3.2, height: 0.5},
                     {material: 'marsh_grass', head: '', chance: 2, reach: 4.0, height: 0.3}]},
    aquatic: {emergent: {material: 'reed', chance: 9, minDepth: 0.1, maxDepth: 0.8, height: 1.6},
              floating: {chance: 10, minDepth: 0.4, maxDepth: 1.2, flowerChance: 4},
              submerged: {chance: 0}},
    placement: {tile: 76.8, rarity: 5, maxSlope: 32}
  },
  spring_pool: {
    name: 'spring_pool', displayName: 'Spring pool', kind: 'pool',
    footprint: {radius: 2.2, radiusV: 0.8, warpAmp: 0.06, squareness: 2.0},
    bathymetry: {depth: 1.6, rimDepth: 0.4, profile: [[0, 1], [0.6, 0.85], [1, 0]], floorNoise: 0.03},
    berm: {height: 0.3, width: 0.8},
    bed: {shallow: 'gravel', deep: 'gravel', shallowDepth: 3.0, thickness: 0.3},
    shore: {band: 1.0, lift: 0.8, mudWidth: 0.3, mossChance: 2,
            plants: [{material: 'fern', head: '', chance: 4, reach: 1.0, height: 0.4}]},
    aquatic: {emergent: {chance: 0}, floating: {chance: 0}, submerged: {chance: 0}},
    placement: {tile: 25.6, rarity: 9, maxSlope: 160}
  },
  oasis: {
    name: 'oasis', displayName: 'Oasis', kind: 'pond',
    footprint: {radius: 4.5, radiusV: 2.0, aspect: 1.3, warpAmp: 0.1},
    bathymetry: {depth: 1.4, rimDepth: 0.2, profile: [[0, 1], [0.5, 0.8], [1, 0]]},
    berm: {height: 0.3, width: 1.0},
    bed: {shallow: 'sand', deep: 'sand', shallowDepth: 3.0, thickness: 0.4},
    ground: {skin: 'sand', soil: 'sand', soilDepth: 1.5, rock: 'stone'},
    shore: {band: 2.6, lift: 1.0, mudWidth: 0.4, mossChance: 0,
            plants: [{material: 'reed', head: '', chance: 6, reach: 1.0, height: 1.4},
                     {material: 'dry_tussock', head: '', chance: 4, reach: 2.6, height: 0.3},
                     {material: 'desert_scrub', head: '', chance: 9, reach: 2.6, height: 0.5}]},
    aquatic: {emergent: {chance: 0}, floating: {chance: 0}, submerged: {chance: 0}},
    placement: {tile: 102.4, rarity: 6, maxSlope: 48}
  },
  playa: {
    name: 'playa', displayName: 'Playa (dry bed)', kind: 'dry',
    footprint: {radius: 8.0, radiusV: 4.0, squareness: 1.8, warpAmp: 0.2, warpFreq: 1.8},
    bathymetry: {depth: 0.5, rimDepth: 0.05, profile: [[0, 1], [0.6, 0.98], [1, 0]], floorNoise: 0.02},
    fill: {material: 'none'},
    berm: {height: 0, width: 0.5},
    bed: {shallow: 'ash', deep: 'ash', shallowDepth: 3.0, thickness: 0.2, substrate: 'sand'},
    ground: {skin: 'sand', soil: 'sand', soilDepth: 1.0, rock: 'stone'},
    shore: {band: 0, plants: []},
    aquatic: {emergent: {chance: 0}, floating: {chance: 0}, submerged: {chance: 0}},
    placement: {tile: 128.0, rarity: 5, maxSlope: 24}
  },
  crater_lake: {
    name: 'crater_lake', displayName: 'Crater lake', kind: 'lake',
    footprint: {radius: 7.0, radiusV: 2.0, warpAmp: 0.05, squareness: 2.0},
    bathymetry: {depth: 3.4, rimDepth: 1.2, profile: [[0, 1], [0.7, 0.96], [0.9, 0.5], [1, 0]]},
    berm: {height: 1.6, width: 3.0, coreFrac: 0.3},
    bed: {shallow: 'gravel', deep: 'stone', shallowDepth: 0.5, thickness: 0.3},
    ground: {skin: 'gravel', soil: 'stone', soilDepth: 0.5},
    shore: {band: 0.8, lift: 2.0, mudWidth: 0, mossChance: 0, plants: []},
    aquatic: {emergent: {chance: 0}, floating: {chance: 0},
              submerged: {chance: 200, minDepth: 2.0, height: 0.8}},
    placement: {tile: 102.4, rarity: 12, maxSlope: 128}
  },
  lava_pool: {
    name: 'lava_pool', displayName: 'Lava pool', kind: 'pool',
    footprint: {radius: 2.4, radiusV: 1.0, warpAmp: 0.1, squareness: 2.0},
    bathymetry: {depth: 1.8, rimDepth: 0.4, profile: [[0, 1], [0.6, 0.9], [1, 0]]},
    fill: {material: 'lava', level: -0.4},
    berm: {height: 0.6, width: 1.0},
    bed: {shallow: 'stone', deep: 'stone', shallowDepth: 0.1, thickness: 0.3},
    ground: {skin: 'stone', soil: 'stone', soilDepth: 0.4},
    shore: {band: 0, plants: []},
    aquatic: {emergent: {chance: 0}, floating: {chance: 0}, submerged: {chance: 0}},
    placement: {tile: 128.0, rarity: 16, maxSlope: 64}
  },
  spawn_lake: {
    name: 'spawn_lake', displayName: 'Spawn lake (authored)', kind: 'lake',
    footprint: {radius: 6.8, radiusV: 0, warpAmp: 0, squareness: 2.0, rotationRandom: false},
    bathymetry: {depth: 2.4, rimDepth: 2.4, profile: [[0, 1], [1, 1]], floorNoise: 0},
    berm: {height: 0.5, width: 1.2},
    bed: {shallow: 'sand', deep: 'sand', shallowDepth: 0.4, thickness: 0.3},
    shore: {band: 2.4, lift: 1.2},
    placement: {tile: 0, rarity: 0}
  }
};

export const PRESET_ORDER = ['tarn', 'kettle', 'marsh', 'spring_pool', 'oasis', 'playa',
                             'crater_lake', 'lava_pool', 'spawn_lake'];
