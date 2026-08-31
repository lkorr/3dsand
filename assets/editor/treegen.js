/* treegen.js — the ONE tree voxelizer.
 *
 * WHY THIS FILE IS THE ONLY PLACE A TREE IS BUILT
 * -----------------------------------------------
 * Worldgen is a pure per-cell function on the GPU: `genChunk` answers for one
 * voxel with no memory of its neighbours and no way to walk a turtle, so every
 * tree the engine has ever grown was an IMPLICIT SHAPE — an ellipsoid, a cone,
 * a hand-unrolled 5-limb skeleton — re-derived per cell. That ceiling is why
 * the forest read as lollipops.
 *
 * So the tree is voxelized ONCE, here, offline, and the engine samples the
 * result. This module is the voxelizer; `assets/trees/<species>.json` is the
 * authored parameter set; `<species>.svtree` is the baked column-RLE atlas the
 * C++ loader uploads and `worldgen.wgsl` reads. There is exactly ONE
 * implementation of "what a tree looks like", it runs in JavaScript, and the
 * tuner's Trees tab shows you its output byte-for-byte. A second copy of the
 * SDF/clump/shading logic in WGSL is precisely the drift this arrangement
 * exists to prevent (see the note at tuner.html's model bridge).
 *
 * PURE MODULE. No DOM, no fetch, no three.js, no WebGL — same discipline as
 * anim.js, and for the same reason: `scripts/test_treegen.mjs` and
 * `scripts/bake_trees.mjs` run it under Node with nothing but `fs`.
 *
 * NO Math.random(), ANYWHERE. Every random number comes from `frand(...)`,
 * a counter-based hash keyed on the stem's own path through the tree
 * (level, parent-path hash, child index). Two consequences, both load-bearing:
 *
 *   1. Same params + same seed  ->  byte-identical cells, on any machine, in
 *      any order. That is what lets the baked atlas be an ENGINE INPUT like
 *      tuning.json rather than a source of nondeterminism (CLAUDE.md rule 1,
 *      JS edition).
 *   2. Nudging one slider does not reshuffle the whole tree. A sequential RNG
 *      consumed in traversal order re-keys every draw downstream of the first
 *      changed branch, so the tree you were tuning jumps to a different tree
 *      the moment you touch anything (ez-tree visibly suffers this). Path
 *      keying means branch 7-of-12 rolls the same numbers no matter what
 *      happened elsewhere.
 *
 * THE ALGORITHM is Weber & Penn 1995 ("Creation and Rendering of Realistic
 * Trees") reduced to the parameters that earn their slider: a recursive stem
 * skeleton under a crown-envelope curve (`shape`), stamped as round-cone SDFs,
 * with ellipsoid leaf CLUMPS welded by a smooth-min at the outer stems. The
 * clumps are the whole visual thesis — foliage lives in lobes around branch
 * tips, never in one canopy-sized ball — and the shading bake (a mix of the
 * clump-sphere normal and the whole-canopy normal, plus depth-into-clump)
 * gives those lobes readable form with zero engine cost, because it resolves
 * to a MATERIAL choice out of a 3-step shade ramp.
 *
 * COORDINATES are the engine's: Y up, X/Z horizontal, one cell = one voxel.
 * Every length parameter below is authored in METRES and reads true next to a
 * 1.7 m player at ANY voxel size — the bake scale is a parameter (`opts.vpm`,
 * defaulting to DEFAULT_VOX_PER_M) and is recorded in the .svtree header, so
 * changing the engine's `kVoxelMeters` means re-baking rather than re-authoring.
 * Do not restate what kVoxelMeters currently is here; that comment has been
 * wrong once already.
 */

'use strict';

// =============================================================================
// constants
// =============================================================================

/** DEFAULT voxels per metre to bake at — the AUTHORING BASELINE, not a mirror
 *  of anything. Every length in a species file is metres; this is the scale the
 *  tuner previews at and the scale `bake_trees.mjs` falls back to.
 *
 *  It used to claim it "mirrors world.h kVoxelsPerMetre", by hand-copied
 *  literal, and that claim was the whole bug: when world.h moved to 5 cm this
 *  stayed at 10 and every tree in the world baked at half its authored height,
 *  with nothing anywhere able to notice. The scale is now a PARAMETER
 *  (`opts.vpm` on generateTree/bakeAtlas) and the value used is written into
 *  the .svtree header, where the C++ loader checks it against the real
 *  kVoxelsPerMetre and refuses a mismatch by name. */
export const DEFAULT_VOX_PER_M = 10;

/** Hard ceiling on a baked variant, per axis, in VOXELS at the bake scale.
 *  38 m is taller than any real sequoia; expressing it in metres is the point,
 *  since a fixed voxel count would silently become 19 m at 5 cm voxels and clip
 *  the redwood. Must stay inside the run encoding's Y0_BITS (see packRun). */
export const MAX_DIM_METRES = 38;
export function maxDim(vpm) { return Math.ceil(MAX_DIM_METRES * vpm); }

/** Run encoding field widths. The word is FULL — 16 bits of material+state,
 *  then Y0 and LEN share what is left — so these two trade against each other
 *  and nothing else.
 *
 *  Y0 was 9 bits (0..511) and LEN 7 (1..127), which caps a variant at 512
 *  voxels: fine at 10 cm, but a 22 m redwood needs ~520 at 5 cm and ~1040 at
 *  2.5 cm, so it clipped. Moving two bits from LEN to Y0 buys 2047 voxels of
 *  height — 102 m at 5 cm, 51 m at 2.5 cm — at the cost of splitting long runs
 *  more often. That trade is right for this data: tree columns are mostly short
 *  leaf runs, and only a solid trunk pays, at 4 words per 127 voxels instead of
 *  1. */
export const Y0_BITS = 11;
export const LEN_BITS = 5;
export const MAX_Y0 = (1 << Y0_BITS) - 1;      // 2047
export const MAX_RUN = (1 << LEN_BITS) - 1;    // 31

/** The bake's fixed key light. NOT the game's sun — the game's sun moves, and
 *  a tree whose leaves are baked to one sun angle would be wrong twice a day.
 *  This is a FORM light: it exists to give clumps readable volume (a lit crown
 *  and a shaded underside) the way a painted texture would, and the engine's
 *  real lighting multiplies on top of it. Up-weighted hard, because the one
 *  thing every real canopy does is go dark underneath. */
const SUN = normalize([0.36, 0.90, 0.24]);

// =============================================================================
// deterministic hashing
// =============================================================================
//
// A 32-bit integer mix (Murmur-style, the `lowbias32` constants). It does NOT
// have to match the engine's `hash3` — nothing compares the two — it only has
// to be STABLE, so a tree baked today re-bakes identically next year.

function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x >>> 0;
}

/** Hash an arbitrary list of integers to a u32. */
export function hashN(...vals) {
  let h = 0x9e3779b9;
  for (let i = 0; i < vals.length; i++) {
    h = mix32((h ^ (vals[i] | 0)) >>> 0);
    h = (h + 0x9e3779b9) >>> 0;
  }
  return mix32(h);
}

/** Uniform in [0,1). */
function frand(...key) { return hashN(...key) / 4294967296; }
/** Uniform in [-1,1). */
function frandS(...key) { return frand(...key) * 2 - 1; }

// =============================================================================
// small vector helpers (plain arrays; this module allocates freely because it
// runs offline, not per frame)
// =============================================================================

function v3(x, y, z) { return [x, y, z]; }
function add(a, b) { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function scale(a, s) { return [a[0] * s, a[1] * s, a[2] * s]; }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]];
}
function len(a) { return Math.sqrt(dot(a, a)); }
function normalize(a) {
  const l = len(a);
  return l > 1e-9 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 1, 0];
}
function lerp3(a, b, t) {
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
          a[2] + (b[2] - a[2]) * t];
}
function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
const D2R = Math.PI / 180;

/** Rotate `v` about unit axis `k` by `ang` radians (Rodrigues). */
function rotAxis(v, k, ang) {
  const c = Math.cos(ang), s = Math.sin(ang);
  const kv = cross(k, v), kd = dot(k, v);
  return [v[0] * c + kv[0] * s + k[0] * kd * (1 - c),
          v[1] * c + kv[1] * s + k[1] * kd * (1 - c),
          v[2] * c + kv[2] * s + k[2] * kd * (1 - c)];
}

/** An orthonormal growth frame. `y` is the direction of growth; `x`/`z` span
 *  the cross-section. Carried explicitly rather than as a quaternion so the
 *  phyllotaxis roll ("rotate about the PARENT's axis") is one line. */
function frameFrom(y, ref) {
  const yy = normalize(y);
  let r = ref || [1, 0, 0];
  if (Math.abs(dot(r, yy)) > 0.95) r = [0, 0, 1];
  const x = normalize(cross(r, yy));
  const z = cross(yy, x);
  return { x: x, y: yy, z: z };
}
function frameRot(f, axis, ang) {
  return { x: rotAxis(f.x, axis, ang), y: normalize(rotAxis(f.y, axis, ang)),
           z: rotAxis(f.z, axis, ang) };
}

// =============================================================================
// PARAMETERS
// =============================================================================
//
// The authored surface. Every key here is a slider in the Trees tab and a
// field in assets/trees/<species>.json, and nothing else in this file reads
// anything that is not in here (bar the seed) — so "what can I change about a
// tree" has exactly one answer, and adding a knob means adding a row.
//
// Per-LEVEL values are arrays indexed by branch level: 0 = trunk, 1 = primary
// branches, 2 = secondary, 3 = twigs. Weber & Penn's own convention, kept
// because their published presets (aspen, black tupelo, weeping willow, black
// oak) then transfer straight across.

/** One level's shape. Angles in degrees, lengths as a FRACTION of the parent's
 *  length (except level 0, which is a fraction of `scale`). */
function defaultLevel(n) {
  return {
    length: n === 0 ? 1.0 : 0.4,      // x parent length
    lengthV: 0.1,                     // +- fraction
    taper: 1.0,                       // 0 cylinder .. 1 cone to a point
    branches: n === 0 ? 0 : 14,       // children spawned ON this level
    downAngle: 60,                    // child pitch away from the parent axis
    downAngleV: 0,                    // +-; NEGATIVE varies along the parent
    rotate: 137.5,                    // phyllotaxis roll between children
    rotateV: 12,                      // +-; NEGATIVE alternates sides
    curve: 0,                         // total bend over the stem, degrees
    curveBack: 0,                     // second-half bend (S-curves)
    curveV: 40,                       // per-segment random wander, degrees
    curveRes: n === 0 ? 8 : 5,        // segments per stem
    segSplits: 0,                     // forks per segment (fractional)
    splitAngle: 20,                   // fork divergence, degrees
    splitAngleV: 8
  };
}

export function defaultParams() {
  return {
    name: 'untitled',
    displayName: 'Untitled',
    /** How many baked variants this species ships. Variety in game is
     *  variants x 4 rotations x mirror, so 3 is already 24 appearances. */
    variants: 3,

    // ---- global form -------------------------------------------------------
    /** Weber-Penn crown envelope. THE single most identity-defining knob: it
     *  is the curve that says how long a branch may be as a function of where
     *  it leaves the trunk, and it is what separates a conifer from an oak
     *  before any other parameter is touched.
     *  0 conical, 1 spherical, 2 hemispherical, 3 cylindrical,
     *  4 tapered cylindrical, 5 flame, 6 inverse conical, 7 tend flame. */
    shape: 2,
    /** Fraction of the trunk that carries no branches at all. A forest tree
     *  self-prunes its lower limbs; a field oak does not. */
    baseSize: 0.30,
    scale: 8.0,                       // metres, trunk length
    scaleV: 1.2,
    levels: 3,                        // recursion depth, 1..4
    /** Trunk radius = scale * ratio. 0.02 is a slender birch, 0.05 a redwood. */
    ratio: 0.028,
    /** How fast radius falls with each level. >1 = children much thinner. */
    ratioPower: 1.2,
    /** Root buttress: extra radius in the bottom 10% of the trunk. */
    flare: 0.8,
    /** Vertical tropism applied per segment. Positive lifts branch tips toward
     *  the sky (most trees); NEGATIVE droops them (willow, some eucalypts). */
    attractionUp: 0.45,
    /** Pseudo-whorls: branches emitted in rings of N rather than a continuous
     *  spiral. Vanilla Weber-Penn cannot make a convincing conifer without it.
     *  0 = pure phyllotaxis. */
    whorlCount: 0,
    /** Trunk cross-section lobing — a fluted, non-circular bole. */
    lobes: 0,
    lobeDepth: 0.10,
    /** Bark relief: SDF minus value noise, in voxels. Under ~0.4 it does
     *  nothing; over ~1.5 the trunk starts shedding disconnected chips. */
    barkNoise: 0.07,

    levelsData: [defaultLevel(0), defaultLevel(1), defaultLevel(2),
                 defaultLevel(3)],

    // ---- foliage -----------------------------------------------------------
    foliage: {
      /** Lowest branch level that grows leaf clumps. `levels` is the RECURSION
       *  DEPTH and makeStem recurses while `level + 1 < levels`, so the
       *  deepest stem that exists is `levels - 1` — set this to that for
       *  foliage at the extremities only, and to `levels` for a bare tree.
       *  Anything higher is the same bare tree. */
      startLevel: 2,
      clumpsPerStem: 3,
      /** Where along the stem clumps begin, 0..1. High = tip tufts only. */
      tipBias: 0.45,
      radius: 0.55,                   // metres
      radiusV: 0.18,
      /** THE CLUMP PRIMITIVE. A crown built out of one shape can only ever be
       *  a pile of that shape, which is why every early tree here read as a
       *  bunch of grapes. All four are the same spheroid field measured in the
       *  clump's own frame (see `clumpAxis`), so they cost the same and weld
       *  to each other the same way:
       *    0 blob   — a lobe. Broadleaf mass; what a beech or an oak is made of.
       *    1 plate  — squashed ALONG the axis into a disc. With the axis
       *               upright this is the layered, tiered spray of a cedar or
       *               a mature pine; the gaps between the tiers are the read.
       *    2 spray  — stretched along the axis and thin across: a leafy SHOOT
       *               rather than a ball. Birch and willow whips, and the way
       *               to get "lots of leaves" without any blob at all.
       *    3 cone   — fat at the base, tapering to a point up the axis. A
       *               spruce/fir sprig, and the only shape with a direction
       *               you can see. */
      clumpShape: 0,
      /** What the clump's own axis IS: 0 = world up (lobes are oriented the
       *  same way everywhere, which is what a canopy tier wants), 1 = the
       *  direction of the twig it grows on (which is what a shoot wants).
       *  Only shapes 1-3 have a visible axis; a blob is a blob either way. */
      clumpAxis: 0,
      /** Eat the CORE out of every lobe, 0..1. 0 is solid; 0.7 leaves a shell
       *  of leaves three deep over an empty middle. A crown is a surface — the
       *  inside of a real one is bare twig and shade — so this is the cheapest
       *  way to trade green mass for leaf COUNT, and it deletes voxels the
       *  player could never see from outside. */
      hollow: 0,
      /** Extent along the clump's axis, as a multiple of the radius. For a
       *  blob >1 is a vertical egg and <1 a squashed bun; for a plate it is
       *  how THIN the disc is; for a spray and a cone, how long. */
      elongation: 0.65,
      /** Downward offset of the clump from its stem, as a fraction of radius.
       *  What makes a eucalypt's foliage hang off the branch. */
      droop: 0.25,
      /** 1 = solid lobe, 0.5 = half the rim eaten away. The GAPS are the
       *  identity of a eucalypt or an old pine, so this earns its slider. */
      density: 0.88,
      /** Feature size of the erosion, in voxels. Per-VOXEL erosion (noise
       *  scale 1) is what made the previous generation's crowns read as green
       *  dust at any distance, and it also multiplies the baked run count by
       *  ~8x. Erode in blobs. */
      noiseScale: 0.26,
      /** Smooth-min weld radius between neighbouring clumps, in voxels. 0
       *  leaves them as separate beads; too high melts the lobes back into
       *  the ball this whole design exists to avoid. */
      sminK: 0.25,
      /** 0 = shade purely by the clump's own sphere normal (every lobe reads
       *  as its own ball); 1 = shade by the whole canopy (the lobes vanish
       *  into one mass). The interesting range is 0.35..0.7. */
      canopyShadeMix: 0.5,
      /** How much deep-inside-a-clump darkens, 0..1. */
      depthShade: 0.75
    },

    // ---- materials, BY NAME ------------------------------------------------
    // Resolved against materials.json at bake time into a name table the
    // engine re-resolves at LOAD (design guideline 4: author by name, resolve
    // at load). Each ramp is dark -> mid -> lit; the mid entry is the plain
    // material the rest of the game already knows.
    bark: ['bark_dark', 'wood', 'bark_light'],
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    /** The AUTUMN dress, as a parallel three-step ramp. A slice of every
     *  broadleaf stand turns, by TREE and never by voxel, and the engine does
     *  it as a material substitution at sample time — `leaf[i] -> autumnLeaf[i]`
     *  for a tree whose hash rolled it. That is why it is a ramp and not one
     *  colour: substituting a flat autumn material for a shaded green one would
     *  throw away the whole shading bake on exactly the trees the eye goes to.
     *  `autumnChance` is 1-in-N trees; 0 disables it (conifers do not turn). */
    autumnLeaf: ['autumn_dark', 'autumn_leaves', 'autumn_lit'],
    autumnChance: 0,

    // ---- placement, read by the ENGINE (not by the voxelizer) --------------
    // Shipped inside the species file because "where does this tree grow" is
    // a property of the species, and putting it anywhere else means adding a
    // tree touches two files. The C++ loader lifts these into the atlas
    // directory; worldgen samples them.
    placement: {
      /** Relative weight per biome. Zero means "never here". The engine
       *  normalises across whatever species are loaded, so adding a species
       *  dilutes the others rather than needing every table rewritten. */
      biomes: { forest: 40, meadow: 10, pine: 0, desert: 0 },
      /** Absolute world-Y band this species tolerates. `maxY` is a per-species
       *  treeline: a spruce climbs higher than an oak. -1 = no bound. */
      minY: -1,
      maxY: -1,
      /** Steepest ground it will root on, in the engine's Q8 slope units
       *  (256 == 1 voxel/voxel == angle of repose). 1024 = no bound. */
      maxSlope: 420,
      /** Extra rarity: 1 = as common as its weight says, 4 = a quarter that. */
      sparsity: 1,
      /** How dark this species' canopy makes the forest floor, 0..255. Read by
       *  worldgen's undergrowth layer (fern and moss under deep shade, grass
       *  and flowers in the gaps) and by nothing else. 0 means "shades
       *  nothing", which is the right answer for a bush and is what keeps a
       *  meadow full of shrubs from reading as closed forest. */
      shade: 200
    }
  };
}

/** Deep-merge an authored (possibly partial) params object onto the defaults,
 *  so a species file only has to state what it changes and old files keep
 *  loading when a knob is added. */
// `vpm` is the BAKE SCALE, not a species parameter: it is carried on the
// normalized object so every downstream function that already receives `P` can
// convert metres without a new argument, and it survives re-normalization
// (bakeAtlas normalizes, then generateTree normalizes the result again) because
// an already-normalized P supplies it through `src`.
export function normalizeParams(src, vpm) {
  const p = defaultParams();
  p.vpm = (vpm | 0) > 0 ? (vpm | 0)
        : (src && (src.vpm | 0) > 0 ? (src.vpm | 0) : DEFAULT_VOX_PER_M);
  if (!src) return p;
  for (const k of Object.keys(p)) {
    if (k === 'vpm') continue;   // set above; never authored in a species file
    if (!(k in src)) continue;
    const a = p[k], b = src[k];
    if (k === 'levelsData') {
      for (let i = 0; i < 4; i++) {
        if (b && b[i]) Object.assign(a[i], b[i]);
      }
    } else if (k === 'placement') {
      Object.assign(a, b || {});
      if (b && b.biomes) Object.assign(a.biomes, b.biomes);
    } else if (a && typeof a === 'object' && !Array.isArray(a)) {
      Object.assign(a, b || {});
    } else {
      p[k] = b;
    }
  }
  return p;
}

// =============================================================================
// the crown envelope
// =============================================================================

/** Weber & Penn's `ShapeRatio`: the fraction of the maximum child length
 *  available at height `r` (0 at the base of the branch-bearing trunk, 1 at
 *  the top). This ONE function is the difference between a fir and an oak. */
function shapeRatio(shape, r) {
  r = clamp(r, 0, 1);
  switch (shape | 0) {
    case 0: return 0.2 + 0.8 * r;                        // conical
    case 1: return 0.2 + 0.8 * Math.sin(Math.PI * r);    // spherical
    case 2: return 0.2 + 0.8 * Math.sin(0.5 * Math.PI * r); // hemispherical
    case 3: return 1.0;                                  // cylindrical
    case 4: return 0.5 + 0.5 * r;                        // tapered cylindrical
    case 5: return r <= 0.7 ? r / 0.7 : (1 - r) / 0.3;   // flame
    case 6: return 1 - 0.8 * r;                          // inverse conical
    default:                                             // 7: tend flame
      return r <= 0.7 ? 0.5 + 0.5 * r / 0.7 : 0.5 + 0.5 * (1 - r) / 0.3;
  }
}

// =============================================================================
// SKELETON
// =============================================================================
//
// A stem is a polyline with a radius at every point, plus the bookkeeping the
// children need. Everything is in VOXELS by the time it lands here (params are
// metres; the conversion happens once, at the top of buildSkeleton).

// Safety rails. A slider set to 30 branches on four levels is 810,000 stems,
// which is a hung tab rather than a tree. These are not tuning — they are the
// bound that makes the generator answer in finite time whatever is typed.
const MAX_STEMS = 6000;
// Clumps are the expensive half (each one marches its own AABB into the
// distance field), and past a few thousand they stop adding anything the eye
// can see — the lobes are already overlapping 8-deep.
const MAX_CLUMPS = 4000;

function buildSkeleton(P, seed) {
  const S = { stems: [], clumps: [], truncated: false };
  const vpm = P.vpm;
  const rootHash = hashN(seed, 0x7A11, P.levels | 0);

  // Trunk length, jittered per seed. `scaleV` is why two oaks in a stand are
  // not the same oak.
  const trunkLen = Math.max(4,
      (P.scale + P.scaleV * frandS(rootHash, 1)) * vpm);
  const trunkRad = Math.max(0.8, trunkLen * P.ratio);

  growStem(S, P, {
    level: 0,
    origin: [0, 0, 0],
    frame: frameFrom([0, 1, 0], [1, 0, 0]),
    length: trunkLen,
    radius: trunkRad,
    pathHash: rootHash,
    // Where on the PARENT this stem started (0..1). The trunk starts at its
    // own base; children carry the real value, which the crown envelope and
    // the down-angle-varies-along-the-parent rule both read.
    parentT: 0,
    parentLen: trunkLen
  });

  return S;
}

/** Grow one stem, then recurse into its children. Iterative over segments,
 *  recursive over levels — depth is `P.levels`, at most 4. */
function growStem(S, P, spec) {
  if (S.stems.length >= MAX_STEMS) { S.truncated = true; return; }
  const L = P.levelsData[Math.min(spec.level, 3)];
  const res = Math.max(1, Math.min(20, L.curveRes | 0));
  const segLen = spec.length / res;
  const ph = spec.pathHash;

  const pts = [spec.origin.slice()];
  const rads = [spec.radius];
  let f = spec.frame;
  let p = spec.origin.slice();

  // The bend budget. `curve` is the total declination over the whole stem;
  // `curveBack` bends the second half the other way, which is what makes an
  // S-curve rather than an arc. Both are split across segments so curveRes is
  // a RESOLUTION knob and not a shape knob — raise it and you get a smoother
  // version of the same stem, not a different one.
  for (let i = 0; i < res; i++) {
    const t0 = i / res;
    const bend = (t0 < 0.5 || L.curveBack === 0)
        ? L.curve / res
        : (L.curve * 0.5 - L.curveBack * 0.5) / (res * 0.5);
    // Wander: a rotation of random magnitude about a random axis in the
    // cross-section. `curveV` divided by res for the same reason as above.
    const wob = (L.curveV / res) * frandS(ph, 0x11, i);
    const roll = frand(ph, 0x12, i) * Math.PI * 2;
    const axis = normalize(add(scale(f.x, Math.cos(roll)),
                               scale(f.z, Math.sin(roll))));
    f = frameRot(f, axis, (bend + wob) * D2R);

    // Vertical tropism, applied in WORLD space, so it accumulates along the
    // stem the way gravity and phototropism actually do. Negative droops.
    if (P.attractionUp !== 0) {
      const up = [0, 1, 0];
      const horiz = Math.sqrt(f.y[0] * f.y[0] + f.y[2] * f.y[2]);
      if (horiz > 1e-4) {
        // Strength falls off as the stem approaches vertical: a branch already
        // pointing up has nothing to be pulled toward, and applying the full
        // rotation there is what makes every tip converge into a spike.
        const k = P.attractionUp * horiz / res;
        const ax = normalize(cross(f.y, up));
        f = frameRot(f, ax, k);
      }
    }

    p = add(p, scale(f.y, segLen));
    pts.push(p.slice());
    // Radius law. `taper` 1 tapers to a point, 0 stays a cylinder;
    // `ratioPower` is applied at the CHILD's birth, not here.
    const u = (i + 1) / res;
    // THE FLOOR IS A CONNECTIVITY BOUND, not a cosmetic one. A tube of radius
    // r swept through a voxel lattice is only guaranteed to leave a
    // 6-CONNECTED trail once r is about 0.9: below that a diagonal run touches
    // its neighbours corner-to-corner, which reads as a dotted line and, far
    // worse, is DISCONNECTED as far as both the connectivity prune below and
    // the engine's own support scan are concerned. At 0.62 the prune was
    // throwing away 39% of the dead tree's wood and a third of the willow's
    // foliage, because the twigs holding them on were not actually touching
    // anything.
    rads.push(Math.max(0.95, spec.radius * (1 - L.taper * u)));

    // Dichotomous split: the stem forks and BOTH halves continue. Handled by
    // spawning a clone as a child stem at the same level and stopping the
    // accumulation of new splits on it, which keeps the count bounded — real
    // segSplits recursion is exponential.
    if (L.segSplits > 0 && i < res - 1 && !spec.noSplit) {
      const chance = L.segSplits - Math.floor(L.segSplits);
      const n = Math.floor(L.segSplits) +
                (frand(ph, 0x13, i) < chance ? 1 : 0);
      for (let s = 0; s < n && s < 2; s++) {
        const ang = (L.splitAngle + L.splitAngleV * frandS(ph, 0x14, i * 4 + s))
                    * D2R;
        const roll2 = frand(ph, 0x15, i * 4 + s) * Math.PI * 2;
        const ax2 = normalize(add(scale(f.x, Math.cos(roll2)),
                                  scale(f.z, Math.sin(roll2))));
        growStem(S, P, {
          level: spec.level,
          origin: p.slice(),
          frame: frameRot(f, ax2, ang),
          length: spec.length * (1 - u) * 0.85,
          radius: rads[rads.length - 1] * 0.8,
          pathHash: hashN(ph, 0x5717, i, s),
          parentT: spec.parentT,
          parentLen: spec.parentLen,
          noSplit: true
        });
      }
    }
  }

  const stem = {
    level: spec.level, pts: pts, rads: rads, pathHash: ph,
    length: spec.length
  };
  S.stems.push(stem);

  // ---- children ------------------------------------------------------------
  const nextLevel = spec.level + 1;
  if (nextLevel < P.levels) {
    const C = P.levelsData[Math.min(nextLevel, 3)];
    const count = Math.max(0, Math.min(60, C.branches | 0));
    // The trunk keeps its bare lower `baseSize`; branches spawn children over
    // their whole length.
    const start = spec.level === 0 ? clamp(P.baseSize, 0, 0.95) : 0.05;
    const whorl = Math.max(0, P.whorlCount | 0);

    for (let i = 0; i < count; i++) {
      if (S.stems.length >= MAX_STEMS) { S.truncated = true; break; }
      // Position along the parent. Whorled species put `whorl` children at the
      // same height and step the height per RING; unwhorled species spread
      // them continuously.
      let t;
      if (whorl > 1) {
        const ring = Math.floor(i / whorl);
        const rings = Math.max(1, Math.ceil(count / whorl));
        t = start + (1 - start) * (ring + 0.5) / rings;
      } else {
        t = start + (1 - start) * (i + 0.5) / count;
      }
      t = clamp(t + 0.02 * frandS(ph, 0x21, i), start, 0.999);

      // Phyllotaxis. `rotate` negative alternates sides instead of spiralling
      // — the flat, fern-like spray a lot of conifers actually have.
      let az;
      if (C.rotate < 0) {
        az = (i % 2 ? 180 : 0) + (-C.rotate) * Math.floor(i / 2);
      } else if (whorl > 1) {
        az = (i % whorl) * (360 / whorl) + Math.floor(i / whorl) * C.rotate;
      } else {
        az = i * C.rotate;
      }
      az += C.rotateV * frandS(ph, 0x22, i);

      // THE ENVELOPE ARGUMENT IS MEASURED FROM THE TOP, not from the bottom.
      // Weber & Penn's ratio is `(length_parent - offset_child) / (length_parent
      // - base_length)`: 1 at the bottom of the branch-bearing zone, 0 at the
      // tip. Feeding it the other way round (the natural reading of "how far up
      // am I") INVERTS every shape in the table — `shape 0 conical` grows its
      // longest branches at the TOP, which is a funnel, and it is why the first
      // pine out of this generator was a lumpy column rather than a conifer.
      const frac = (t - start) / Math.max(1e-3, 1 - start);   // 0 base .. 1 tip
      const wp = 1 - frac;                                    // 1 base .. 0 tip

      // Down-angle. Weber-Penn's NEGATIVE downAngleV is the good trick: the
      // pitch becomes a function of WHERE ON THE PARENT the child sits, so the
      // lower branches lie flat and the upper ones sweep up. That single rule
      // is most of what makes a Black Tupelo (or any conifer) read as a real
      // tree instead of a bottle brush. The formula is the paper's verbatim;
      // note that `downAngleV` enters SIGNED, so the authored negative is what
      // makes the base angle larger and the tip angle smaller.
      let down;
      if (C.downAngleV < 0) {
        down = C.downAngle + C.downAngleV * (1 - 2 * shapeRatio(0, wp));
      } else {
        down = C.downAngle + C.downAngleV * frandS(ph, 0x23, i);
      }

      // Length, under the crown envelope.
      const envelope = spec.level === 0 ? shapeRatio(P.shape, wp) : 1.0;
      const lenScale = C.length * (1 + C.lengthV * frandS(ph, 0x24, i));
      const childLen = spec.length * lenScale * envelope;
      if (childLen < 1.2) continue;   // sub-voxel stems are just noise

      // Where on the parent polyline, and the local frame there.
      const at = pointAlong(pts, rads, t);
      const parentDir = dirAlong(pts, t);
      const pf = frameFrom(parentDir, [1, 0, 0]);
      // Roll about the parent axis, then pitch away from it.
      let cf = frameRot(pf, pf.y, az * D2R);
      cf = frameRot(cf, cf.x, down * D2R);

      const childRad = Math.max(0.95,
          at.r * Math.pow(childLen / Math.max(spec.length, 1e-3), P.ratioPower));

      growStem(S, P, {
        level: nextLevel,
        origin: at.p,
        frame: cf,
        length: childLen,
        radius: Math.min(childRad, at.r * 0.9),
        pathHash: hashN(ph, 0xB2A4, nextLevel, i),
        parentT: t,
        parentLen: spec.length
      });
    }
  }

  // ---- foliage clumps ------------------------------------------------------
  // Attached to the stem AFTER its children exist, so `startLevel === levels`
  // (leaves only at the extremities) and `startLevel < levels` (leaves along
  // the inner branches too) are both just a compare.
  const F = P.foliage;
  if (spec.level >= F.startLevel) {
    // FRACTIONAL, and that is the point. A tree's tip count is set by the
    // branching parameters, which are chosen for the SILHOUETTE; the clump
    // count has to be tunable independently or the two fight. At 0.4 only two
    // stems in five carry a lobe (chosen by the stem's own path hash, so it is
    // stable under every other slider), which is how a crown gets gaps without
    // eroding every lobe into lace.
    const want = Math.max(0, Math.min(12, +F.clumpsPerStem || 0));
    let n = Math.floor(want);
    if (frand(ph, 0x30, 0) < want - n) n++;
    for (let i = 0; i < n; i++) {
      if (S.clumps.length >= MAX_CLUMPS) { S.truncated = true; break; }
      const t = clamp(F.tipBias + (1 - F.tipBias) *
                      ((i + 0.5) / n + 0.14 * frandS(ph, 0x31, i)), 0, 1);
      const at = pointAlong(pts, rads, t);
      const rad = Math.max(1.2,
          (F.radius + F.radiusV * frandS(ph, 0x32, i)) * P.vpm);
      // Clumps sit slightly OFF the stem, in a hashed direction, so a branch
      // carries a row of lobes rather than a sausage centred on itself.
      const off = normalize([frandS(ph, 0x33, i), frandS(ph, 0x34, i) * 0.5,
                             frandS(ph, 0x35, i)]);
      const c = add(at.p, scale(off, rad * 0.35));
      c[1] -= rad * F.droop;
      // The clump's own axis, resolved HERE and not in the field pass: it is
      // the one thing a lobe needs from the skeleton, and the skeleton is not
      // in scope down there. `clumpAxis` 0 keeps every lobe upright (a canopy
      // tier), 1 lays it along the twig it grows on (a shoot).
      const align = clamp(F.clumpAxis, 0, 1);
      const axis = align > 0
          ? normalize(lerp3([0, 1, 0], dirAlong(pts, t), align))
          : [0, 1, 0];
      S.clumps.push({ c: c, r: rad, elong: Math.max(0.15, F.elongation),
                      a: axis, h: hashN(ph, 0x36, i) });
    }
  }
}

/** Point + radius a fraction `t` along a polyline, by arc length. */
function pointAlong(pts, rads, t) {
  const segs = pts.length - 1;
  const ft = clamp(t, 0, 1) * segs;
  const i = Math.min(segs - 1, Math.floor(ft));
  const f = ft - i;
  return { p: lerp3(pts[i], pts[i + 1], f), r: rads[i] + (rads[i + 1] - rads[i]) * f };
}
function dirAlong(pts, t) {
  const segs = pts.length - 1;
  const i = Math.min(segs - 1, Math.floor(clamp(t, 0, 1) * segs));
  return normalize(sub(pts[i + 1], pts[i]));
}

// =============================================================================
// SDFs
// =============================================================================

/** iq's exact round cone: the swept sphere from (a, r1) to (b, r2). This is
 *  the primitive a branch IS — a tapering tube with hemispherical caps — so
 *  joints weld themselves and no explicit joint geometry is needed. */
function sdRoundCone(px, py, pz, a, b, r1, r2) {
  const bax = b[0] - a[0], bay = b[1] - a[1], baz = b[2] - a[2];
  const l2 = bax * bax + bay * bay + baz * baz;
  if (l2 < 1e-9) {
    const dx = px - a[0], dy = py - a[1], dz = pz - a[2];
    return Math.sqrt(dx * dx + dy * dy + dz * dz) - Math.max(r1, r2);
  }
  const rr = r1 - r2;
  const a2 = l2 - rr * rr;
  const il2 = 1 / l2;
  const pax = px - a[0], pay = py - a[1], paz = pz - a[2];
  const y = pax * bax + pay * bay + paz * baz;
  const z = y - l2;
  const xx = pax * l2 - bax * y, xy = pay * l2 - bay * y, xz = paz * l2 - baz * y;
  const x2 = xx * xx + xy * xy + xz * xz;
  const y2 = y * y * l2;
  const z2 = z * z * l2;
  const k = Math.sign(rr) * rr * rr * x2;
  if (Math.sign(z) * a2 * z2 > k) return Math.sqrt(x2 + z2) * il2 - r2;
  if (Math.sign(y) * a2 * y2 < k) return Math.sqrt(x2 + y2) * il2 - r1;
  return (Math.sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

/* The clump primitive, as two numbers. Every shape is the same spheroid field
 * measured in the clump's own frame — `alongScale` multiplies the radius up the
 * clump axis, `acrossScale` perpendicular to it — so a crown may mix shapes and
 * they still smooth-min into one surface. Shape 3 (cone) reads `alongScale` as
 * its length and does its own taper. */
function alongScale(shape, elong) {
  const e = Math.max(0.15, elong);
  return shape === 1 ? 1 / e : e;   // 1 = plate: SQUASHED along the axis
}
function acrossScale(shape) {
  return shape === 2 ? 0.45 : 1;    // 2 = spray: thin across, a shoot not a ball
}

/** Polynomial smooth-min. The weld between neighbouring leaf clumps: without
 *  it a crown is a bag of separate beads, with too much of it the beads
 *  dissolve back into one ball. */
function smin(a, b, k) {
  if (k <= 0) return Math.min(a, b);
  const h = clamp(0.5 + 0.5 * (b - a) / k, 0, 1);
  return b * (1 - h) + a * h - k * h * (1 - h);
}

/** Cheap 3D value noise on the integer lattice, in 0..1. Used for bark relief
 *  and for clump erosion; NOT for anything the eye reads as structure. */
function vnoise(x, y, z, s, salt) {
  const fx = x / s, fy = y / s, fz = z / s;
  const ix = Math.floor(fx), iy = Math.floor(fy), iz = Math.floor(fz);
  const tx = fx - ix, ty = fy - iy, tz = fz - iz;
  const sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty),
        sz = tz * tz * (3 - 2 * tz);
  let acc = 0;
  for (let k = 0; k < 2; k++) for (let j = 0; j < 2; j++) for (let i = 0; i < 2; i++) {
    const w = (i ? sx : 1 - sx) * (j ? sy : 1 - sy) * (k ? sz : 1 - sz);
    acc += w * (hashN(salt, ix + i, iy + j, iz + k) / 4294967296);
  }
  return acc;
}

// =============================================================================
// VOXELIZATION
// =============================================================================

/**
 * Build one tree.
 *
 * @param {object} params   authored parameters (see defaultParams)
 * @param {number} seed     variant index / integer seed
 * @param {object} opts     { palette } — a name -> local-index resolver.
 *                          When absent, cells carry LOCAL palette indices and
 *                          `names` lists them in order. That is what .svtree
 *                          stores; the C++ loader remaps names to engine ids
 *                          at load, so a material id renumbering in
 *                          materials.json can never silently recolour a forest.
 * @returns {{dim:{x,y,z}, cells:Uint16Array, anchor:{x,z}, meta:object,
 *            names:string[], skeleton:object}}
 */
export function generateTree(params, seed, opts) {
  const P = normalizeParams(params, opts && opts.vpm);
  const t0 = (typeof performance !== 'undefined') ? performance.now() : 0;
  const S = buildSkeleton(P, seed | 0);

  // Metre-authored bark relief, resolved to bake-scale voxels. It is a SHAPE
  // parameter, not a texture one — it is what a trunk's silhouette is roughened
  // by — so leaving it in voxels would have quietly halved the relief on the
  // same tree at 5 cm. (`foliage.noiseScale` and `foliage.sminK` get the same
  // treatment where they are unpacked, below.)
  const barkNoiseVox = P.barkNoise * P.vpm;

  // ---- local palette -------------------------------------------------------
  // Index 0 is always air. Everything else is assigned in first-use order and
  // recorded by NAME.
  const names = [];
  const nameIdx = new Map();
  const pal = (n) => {
    let i = nameIdx.get(n);
    if (i === undefined) { i = names.push(n); nameIdx.set(n, i); }
    return i;
  };
  const barkRamp = P.bark.map(pal);
  const leafRamp = P.leaf.map(pal);

  // ---- bounds --------------------------------------------------------------
  let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
  const grow = (p, r) => {
    for (let a = 0; a < 3; a++) {
      if (p[a] - r < lo[a]) lo[a] = p[a] - r;
      if (p[a] + r > hi[a]) hi[a] = p[a] + r;
    }
  };
  for (const st of S.stems) {
    for (let i = 0; i < st.pts.length; i++) {
      grow(st.pts[i], st.rads[i] + barkNoiseVox + 1);
    }
  }
  // The isotropic half-extent a clump can reach, for the grid bound.
  //
  // PRE-EXISTING, AND LEFT ALONE ON PURPOSE: the default arm's reciprocal is
  // backwards. A clump reaches `r * elong` along its axis, not `r / elong`, so
  // this over-pads a broadleaf (elong 0.65 -> 1.54r of empty margin, which also
  // inflates the species' `reach` and therefore the nine-candidate bound) and
  // CLIPS a conifer (elong 1.9 needs 1.9r and gets r). Correcting it changes
  // the grid of every committed species, which re-bakes ten atlases and moves
  // the world hash — a change that deserves its own commit, not a ride-along on
  // an opt-in knob. The new shapes get the right bound because nothing is baked
  // against the wrong one yet.
  const fShape = Math.max(0, Math.min(3, P.foliage.clumpShape | 0));
  const fAxis = clamp(P.foliage.clumpAxis, 0, 1);
  for (const c of S.clumps) {
    const iso = (fShape === 0 && fAxis === 0)
        ? c.r * Math.max(1, 1 / c.elong)
        : c.r * Math.max(alongScale(fShape, c.elong), acrossScale(fShape));
    grow(c.c, iso + 1);
  }

  // LOCAL Y 0 IS THE TRUNK BASE. Not "the lowest voxel the tree happens to
  // reach" — the CONTRACT, because the engine plants local y 0 on the first air
  // cell above the ground and has no other way to know where the ground goes.
  // Padding the bottom the way the X and Z axes are padded put an empty row
  // under every tree: harmless to look at, one wasted layer per variant, and a
  // silent ambiguity about which row stands on the dirt. The `tree-atlas` gate
  // asserts it (27 of 28 variants failed the day it was written).
  //
  // Anything the generator puts BELOW y 0 — a willow's droop, a clump hanging
  // off a low branch — is underground and is clipped by the stamping loops'
  // own bounds. That is the right answer: a leaf below the soil is not a leaf.
  const ox = Math.floor(lo[0]) - 1, oy = 0, oz = Math.floor(lo[2]) - 1;
  let nx = Math.ceil(hi[0]) - ox + 2;
  let ny = Math.ceil(Math.max(hi[1], 1)) + 2;
  let nz = Math.ceil(hi[2]) - oz + 2;
  // The ceiling is metres, resolved at the bake scale — see MAX_DIM_METRES.
  // Also hard-capped by the run encoding's Y0 field, which is the format limit
  // rather than a design one: a variant taller than MAX_Y0 could not address
  // its own top voxel.
  const dimCap = Math.min(maxDim(P.vpm), MAX_Y0);
  const clipped = (nx > dimCap || ny > dimCap || nz > dimCap);
  nx = Math.max(1, Math.min(dimCap, nx));
  ny = Math.max(1, Math.min(dimCap, ny));
  nz = Math.max(1, Math.min(dimCap, nz));

  const cells = new Uint16Array(nx * ny * nz);
  const at = (x, y, z) => (z * ny + y) * nx + x;
  // world (voxel) coords -> grid coords
  const gx = -ox, gy = -oy, gz = -oz;

  // ---- pass ordering, and why it is what it is -----------------------------
  //
  //   1. thin stems (level >= foliage.startLevel)  -- the twigs
  //   2. leaf clumps, overwriting anything present -- i.e. only those twigs
  //   3. structural stems (level < startLevel)     -- trunk and boughs, over all
  //
  // A leaf clump has to ENVELOP the twig it grows on, or every clump has a
  // wire sticking through it. A bough has to sit IN FRONT of the clump, or an
  // oak's scaffold limbs vanish into the canopy. Ordering the three passes
  // this way gets both with no per-voxel bookkeeping: at step 2 the only thing
  // in the grid is twig, so "overwrite unconditionally" is exactly "overwrite
  // twigs".
  const thin = [], thick = [];
  for (const st of S.stems) {
    (st.level >= P.foliage.startLevel ? thin : thick).push(st);
  }

  function stampStems(list) {
    for (const st of list) {
      for (let i = 0; i + 1 < st.pts.length; i++) {
        stampSegment(st.pts[i], st.pts[i + 1], st.rads[i], st.rads[i + 1]);
      }
    }
  }

  // PASS 1 — the twigs, so the clump pass below has something to bury.
  stampStems(thin);

  function stampSegment(a, b, r1, r2) {
    const pad = Math.max(r1, r2) + barkNoiseVox + 1.5;
    const x0 = Math.max(0, Math.floor(Math.min(a[0], b[0]) - pad) + gx);
    const x1 = Math.min(nx - 1, Math.ceil(Math.max(a[0], b[0]) + pad) + gx);
    const y0 = Math.max(0, Math.floor(Math.min(a[1], b[1]) - pad) + gy);
    const y1 = Math.min(ny - 1, Math.ceil(Math.max(a[1], b[1]) + pad) + gy);
    const z0 = Math.max(0, Math.floor(Math.min(a[2], b[2]) - pad) + gz);
    const z1 = Math.min(nz - 1, Math.ceil(Math.max(a[2], b[2]) + pad) + gz);
    if (x1 < x0 || y1 < y0 || z1 < z0) return;
    const axis = normalize(sub(b, a));
    // Bark relief is a FRACTION of the stem's own radius, not an absolute
    // number of voxels. Authored absolutely (the obvious reading of a
    // "barkNoise: 0.085" slider) it eats a 0.8-voxel twig alive: the amplitude
    // exceeds the radius, half the branch falls below the surface, and an oak
    // came out with 1,724 wood voxels — a bare trunk under a floating crown,
    // which is the exact failure this whole rewrite exists to kill. Trunks keep
    // the full authored relief; twigs get what fits.
    const relief = Math.min(barkNoiseVox, Math.max(r1, r2) * 0.45);
    for (let z = z0; z <= z1; z++) {
      const wz = z - gz + 0.5;
      for (let y = y0; y <= y1; y++) {
        const wy = y - gy + 0.5;
        for (let x = x0; x <= x1; x++) {
          const wx = x - gx + 0.5;
          let d = sdRoundCone(wx, wy, wz, a, b, r1, r2);
          if (relief > 0) {
            d += (vnoise(wx, wy, wz, 2.4, 0xBA2C) - 0.5) * 2 * relief;
          }
          if (d > 0) continue;
          // Bark shade: which way this bit of trunk faces, taken as the
          // component of the surface direction perpendicular to the stem axis.
          // Cheap, exact enough at one voxel, and it is what puts a lit and a
          // shaded side on a bole instead of one flat brown column.
          const rel = sub([wx, wy, wz], a);
          const along = dot(rel, axis);
          const perp = sub(rel, scale(axis, along));
          const n = normalize(perp);
          const lit = dot(n, SUN);
          const tier = lit < -0.18 ? 0 : (lit > 0.30 ? 2 : 1);
          const jit = hashN(x, y, z, 0xBA12) % 3;
          cells[at(x, y, z)] = (barkRamp[tier] | (jit << 12));
        }
      }
    }
  }

  // PASS 2 — leaf clumps ------------------------------------------------------
  //
  // THE COST MODEL IS THE DESIGN HERE, so it is worth stating: the obvious
  // shape — march each clump's AABB and smooth-min against its neighbours —
  // is quadratic in the clumps that overlap, and a mature oak's crown holds
  // ~2,000 lobes at 8x mutual overlap. Measured, that arrangement did not
  // finish a single oak in two minutes.
  //
  // Instead the clumps ACCUMULATE INTO A FIELD. One Float32 distance buffer and
  // one Int32 "which lobe owns this cell" buffer over the grid; every clump
  // marches its own AABB exactly once, smooth-min-ing into the field and
  // claiming cells it is closest to. Total work is the SUM of clump volumes,
  // with no neighbour term at all, and the ownership buffer is what the shading
  // bake below needs anyway. Same result, ~30 ms instead of unbounded.
  //
  // The smooth-min is order-dependent (it is not associative), and the order is
  // the clump list's — which is a pure function of the skeleton walk. So the
  // field is deterministic, which is the property that matters.
  const F = P.foliage;
  const clumps = S.clumps;
  let canopy = [0, 0, 0];
  if (clumps.length) {
    for (const c of clumps) canopy = add(canopy, c.c);
    canopy = scale(canopy, 1 / clumps.length);
  }

  const depthShade = clamp(F.depthShade, 0, 1);
  const shadeMix = clamp(F.canopyShadeMix, 0, 1);
  const density = clamp(F.density, 0.05, 1);
  // Both authored in METRES, resolved here to bake-scale voxels. The floors
  // stay in voxels on purpose: an erosion cell finer than ~0.6 voxels has
  // nothing left to erode, which is a property of the lattice and not of the
  // species.
  const nscale = Math.max(0.6, F.noiseScale * P.vpm);
  const sminK = Math.max(0, F.sminK * P.vpm);
  const shape = Math.max(0, Math.min(3, F.clumpShape | 0));
  const axisAmt = clamp(F.clumpAxis, 0, 1);
  const hollow = clamp(F.hollow, 0, 1);

  if (clumps.length) {
    const N = nx * ny * nz;
    const dfield = new Float32Array(N).fill(1e9);
    const owner = new Int32Array(N).fill(-1);
    // Crown sub-box, so the erosion/shading sweep below touches only cells a
    // clump could have reached rather than the whole grid (a redwood is mostly
    // trunk, and the trunk has no leaves).
    let cx0 = nx, cx1 = -1, cy0 = ny, cy1 = -1, cz0 = nz, cz1 = -1;

    // THE DEFAULT SHAPE KEEPS ITS OWN LOOP, and that is deliberate rather than
    // lazy. The general loop below computes the same spheroid for shape 0 with
    // an upright axis, but it REASSOCIATES the arithmetic (a dot product and a
    // Pythagorean subtraction instead of one scaled component), and a float
    // that reassociates moves a voxel, which moves the baked atlas, which moves
    // the WORLD HASH. Adding an opt-in knob must not re-bake ten species.
    //
    // (Known and left alone for the same reason: `ry` below should be
    // `(r + sminK) * elong` — the weld skirt reaches `elong` times further
    // along the long axis than across it — so an elongated lobe has its two
    // poles clipped by a voxel or two. The general path gets this right.)
    const simple = (shape === 0 && axisAmt === 0);
    for (let ci = 0; ci < clumps.length; ci++) {
      const c = clumps[ci];
      const invE = 1 / Math.max(0.15, c.elong);
      let ry, rxz, ex, ez;
      if (simple) {
        ry = c.r * Math.max(1, c.elong) + sminK + 1;
        rxz = c.r + sminK + 1;
        ex = rxz; ez = rxz;
      } else {
        // The exact support function of an axis-aligned spheroid: the extent
        // along world axis i is |a_i| of the long half-extent plus the
        // perpendicular share of the short one. A conservative cube of
        // max(ha,hc) would work and would also scan ~4x the cells for a spray.
        const ha = (c.r + sminK) * alongScale(shape, c.elong) + 1;
        const hc = (c.r + sminK) * acrossScale(shape) + 1;
        const a = c.a;
        ex = Math.abs(a[0]) * ha + Math.sqrt(Math.max(0, 1 - a[0] * a[0])) * hc;
        ry = Math.abs(a[1]) * ha + Math.sqrt(Math.max(0, 1 - a[1] * a[1])) * hc;
        ez = Math.abs(a[2]) * ha + Math.sqrt(Math.max(0, 1 - a[2] * a[2])) * hc;
        rxz = Math.max(ex, ez);
      }
      const x0 = Math.max(0, Math.floor(c.c[0] - ex) + gx);
      const x1 = Math.min(nx - 1, Math.ceil(c.c[0] + ex) + gx);
      const y0 = Math.max(0, Math.floor(c.c[1] - ry) + gy);
      const y1 = Math.min(ny - 1, Math.ceil(c.c[1] + ry) + gy);
      const z0 = Math.max(0, Math.floor(c.c[2] - ez) + gz);
      const z1 = Math.min(nz - 1, Math.ceil(c.c[2] + ez) + gz);
      if (x1 < x0 || y1 < y0 || z1 < z0) continue;
      if (x0 < cx0) cx0 = x0; if (x1 > cx1) cx1 = x1;
      if (y0 < cy0) cy0 = y0; if (y1 > cy1) cy1 = y1;
      if (z0 < cz0) cz0 = z0; if (z1 > cz1) cz1 = z1;

      if (simple) {
        for (let z = z0; z <= z1; z++) {
          const dz = z - gz + 0.5 - c.c[2];
          for (let y = y0; y <= y1; y++) {
            const dy = (y - gy + 0.5 - c.c[1]) * invE;
            const base = (z * ny + y) * nx;
            const q = dy * dy + dz * dz;
            for (let x = x0; x <= x1; x++) {
              const dx = x - gx + 0.5 - c.c[0];
              const d = Math.sqrt(dx * dx + q) - c.r;
              if (d > sminK) continue;
              const i = base + x;
              const prev = dfield[i];
              // Ownership goes to the NEAREST lobe (a hard min), while the
              // field itself is the WELDED surface (a smooth min). Using the
              // smooth value for ownership would make the shading normal
              // wander across the weld, which is exactly the seam the weld
              // exists to hide.
              if (d < prev) owner[i] = ci;
              dfield[i] = prev >= 1e8 ? d : smin(prev, d, sminK);
            }
          }
        }
        continue;
      }

      const ax = c.a[0], ay = c.a[1], az = c.a[2];
      const along = alongScale(shape, c.elong);
      const across = acrossScale(shape);
      for (let z = z0; z <= z1; z++) {
        const dz = z - gz + 0.5 - c.c[2];
        for (let y = y0; y <= y1; y++) {
          const dy = y - gy + 0.5 - c.c[1];
          const base = (z * ny + y) * nx;
          const pu = dy * ay + dz * az;      // the x term is added per cell
          const pq = dy * dy + dz * dz;
          for (let x = x0; x <= x1; x++) {
            const dx = x - gx + 0.5 - c.c[0];
            // Split into ALONG the clump axis and ACROSS it. Everything the
            // four shapes differ by is a function of those two numbers, so one
            // loop covers all of them and they weld to each other for free.
            const u = pu + dx * ax;
            const v = Math.sqrt(Math.max(0, pq + dx * dx - u * u));
            let d;
            if (shape === 3) {
              // CONE: fat at -ha, a point at +ha. The only clump with a
              // direction you can see, which is why it needs the axis.
              const ha = c.r * along;
              const t = clamp((u + ha) / (2 * ha), 0, 1);
              const dr = v - c.r * (1 - t);
              const du = u < -ha ? (-ha - u) : (u > ha ? u - ha : 0);
              d = du > 0 ? Math.hypot(Math.max(dr, 0), du) : dr;
            } else {
              const su = u / along, sv = v / across;
              d = Math.sqrt(su * su + sv * sv) - c.r;
            }
            if (d > sminK) continue;
            const i = base + x;
            const prev = dfield[i];
            if (d < prev) owner[i] = ci;
            dfield[i] = prev >= 1e8 ? d : smin(prev, d, sminK);
          }
        }
      }
    }

    for (let z = cz0; z <= cz1; z++) {
      const wz = z - gz + 0.5;
      for (let y = cy0; y <= cy1; y++) {
        const wy = y - gy + 0.5;
        const base = (z * ny + y) * nx;
        for (let x = cx0; x <= cx1; x++) {
          const i = base + x;
          const d = dfield[i];
          if (d > 0) continue;
          const wx = x - gx + 0.5;
          const oc = clumps[owner[i]];
          // EROSION. A blob-scale noise field, not a per-voxel coin flip:
          // per-voxel erosion reads as green dust at any distance and
          // multiplies the baked run count by ~8x. `rim` is 0 deep in the lobe
          // and 1 at its surface, so the kept fraction ramps from 1 (solid
          // core) to `density` (ragged rim) — a lobe with a chewed edge, not a
          // cloud of specks.
          const rim = clamp(1 + d / Math.max(1, oc.r), 0, 1);
          const n = vnoise(wx, wy, wz, nscale, 0x1EAF);
          if (n > density + (1 - rim) * (1 - density)) continue;

          // How deep inside the lobe: 0 at the surface, 1 at the core.
          const depth = clamp(-d / Math.max(1, oc.r), 0, 1);
          // HOLLOW. A real crown is a surface — the inside is bare twig and
          // shade — so eating the core is free detail: the voxels this drops
          // are the ones no ray from outside could reach. At 0 the compare is
          // `depth > 1`, which `clamp` has already made impossible, so the
          // default path is untouched.
          if (depth > 1 - hollow) continue;

          // ---- the shading bake ------------------------------------------
          // The Rundlett trick: shade a leaf voxel by a normal that is part
          // "which way does my own lobe face" and part "which way does the
          // whole canopy face". Pure clump normal makes every lobe read as a
          // separate ball; pure canopy normal makes the lobes disappear into
          // one mass. The mix is the whole look, and it costs the engine
          // nothing because it resolves to a MATERIAL choice here, offline.
          const nc = normalize([wx - oc.c[0], wy - oc.c[1], wz - oc.c[2]]);
          const ncan = normalize([wx - canopy[0], wy - canopy[1], wz - canopy[2]]);
          const nn = normalize(lerp3(nc, ncan, shadeMix));
          const lit = nn[0] * SUN[0] + nn[1] * SUN[1] + nn[2] * SUN[2];
          const shade = 0.5 + 0.5 * lit - depthShade * depth;
          const tier = shade < 0.34 ? 0 : (shade > 0.70 ? 2 : 1);
          const jit = hashN(x, y, z, 0x1EA5) % 3;
          cells[i] = (leafRamp[tier] | (jit << 12));
        }
      }
    }
  }

  // PASS 3 — trunk and structural boughs, over everything.
  stampStems(thick);

  // ---- trunk flare ---------------------------------------------------------
  // A buttress, added after the stems so it welds onto whatever the trunk
  // turned out to be rather than being a separate cone floating at the base.
  if (P.flare > 0 && S.stems.length) {
    const trunk = S.stems[0];
    const r0 = trunk.rads[0];
    // A root collar, not a cone. Both the height and the swell are a modest
    // multiple of the trunk radius: at `flare 2.2` the first version reached
    // 4.5x the bole radius over 55 voxels, which on a redwood was a 3 m-wide
    // funnel that swallowed the lower trunk entirely.
    const h = Math.max(2, r0 * 2.2 * P.flare);
    for (let y = 0; y < Math.min(ny, Math.ceil(h) + gy); y++) {
      const wy = y - gy + 0.5;
      if (wy < 0 || wy > h) continue;
      // Quadratic swell: nearly nothing at the top of the flare, widest at
      // the ground, which is the shape a root collar actually has.
      const f = (1 - wy / h);
      const rr = r0 * (1 + P.flare * f * f * 0.75);
      const lob = P.lobes | 0;
      const x0 = Math.max(0, Math.floor(-rr) + gx), x1 = Math.min(nx - 1, Math.ceil(rr) + gx);
      const z0 = Math.max(0, Math.floor(-rr) + gz), z1 = Math.min(nz - 1, Math.ceil(rr) + gz);
      for (let z = z0; z <= z1; z++) {
        const wz = z - gz + 0.5;
        for (let x = x0; x <= x1; x++) {
          const wx = x - gx + 0.5;
          const dd = Math.sqrt(wx * wx + wz * wz);
          let rlim = rr;
          if (lob > 0) rlim *= 1 + P.lobeDepth * Math.cos(lob * Math.atan2(wz, wx));
          if (dd > rlim) continue;
          const n = normalize([wx, 0.35, wz]);
          const litv = dot(n, SUN);
          const tier = litv < -0.18 ? 0 : (litv > 0.30 ? 2 : 1);
          const jit = hashN(x, y, z, 0xBA12) % 3;
          cells[at(x, y, z)] = (barkRamp[tier] | (jit << 12));
        }
      }
    }
  }

  // ---- PRUNE EVERYTHING NOT ATTACHED TO THE TRUNK -------------------------
  //
  // A tree is ONE connected object standing on the ground. The stamping passes
  // do not guarantee that: erosion chews crumbs off a clump rim, a drooping
  // lobe can miss its own twig, and a smooth-min weld can leave a bead
  // floating a voxel clear of everything.
  //
  // Left in, those cost twice. Visually they are the "green dust" that made
  // eroded crowns read as noise at any distance. Mechanically they are worse:
  // the engine's support scan (src/phys/debris.cpp) finds unanchored
  // components near a tree and either deletes them (under 8 voxels, as
  // foliage) or CONVERTS THEM TO RIGIDBODIES — and a detached lobe of a few
  // hundred voxels is well over that line, so it becomes a chunk of canopy
  // that materialises on top of the tree and falls off. Measured, before this
  // pass: the `debris` gate found twelve bodies still awake 30-45 voxels above
  // their ground, six of them 400-1,500 voxels.
  //
  // So: 6-connected flood fill from the trunk's own ground row, and anything
  // it does not reach is not part of this tree.
  {
    const seen = new Uint8Array(nx * ny * nz);
    const stack = [];
    // Seed from every occupied cell in the bottom row — the bole, and whatever
    // the root flare put beside it.
    for (let z = 0; z < nz; z++)
      for (let x = 0; x < nx; x++) {
        const i = (z * ny + 0) * nx + x;
        if (cells[i] && !seen[i]) { seen[i] = 1; stack.push(i); }
      }
    while (stack.length) {
      const i = stack.pop();
      // The index is (z*ny + y)*nx + x — decode in that order.
      const x = i % nx;
      const ly = ((i / nx) | 0) % ny;
      const lz = (i / (nx * ny)) | 0;
      const push = (jx, jy, jz) => {
        if (jx < 0 || jy < 0 || jz < 0 || jx >= nx || jy >= ny || jz >= nz) return;
        const j = (jz * ny + jy) * nx + jx;
        if (cells[j] && !seen[j]) { seen[j] = 1; stack.push(j); }
      };
      push(x - 1, ly, lz); push(x + 1, ly, lz);
      push(x, ly - 1, lz); push(x, ly + 1, lz);
      push(x, ly, lz - 1); push(x, ly, lz + 1);
    }
    let dropped = 0;
    for (let i = 0; i < cells.length; i++) {
      if (cells[i] && !seen[i]) { cells[i] = 0; dropped++; }
    }
    S.dropped = dropped;
  }

  // ---- meta ----------------------------------------------------------------
  // Everything the ENGINE needs that is not a voxel: the horizontal reach (for
  // the candidate-tile reject), the height above base (for the sky
  // short-circuit) and a crown proxy (for the far cascades, and for hanging
  // vines off the canopy underside).
  let leafCount = 0, woodCount = 0;
  let cLo = [1e9, 1e9, 1e9], cHi = [-1e9, -1e9, -1e9], cSum = [0, 0, 0];
  const isLeaf = new Uint8Array(names.length + 1);
  for (const i of leafRamp) isLeaf[i] = 1;
  let top = 0;
  for (let z = 0; z < nz; z++) for (let y = 0; y < ny; y++) for (let x = 0; x < nx; x++) {
    const w = cells[at(x, y, z)];
    if (!w) continue;
    if (y > top) top = y;
    if (isLeaf[w & 0xFFF]) {
      leafCount++;
      cSum[0] += x; cSum[1] += y; cSum[2] += z;
      cLo = [Math.min(cLo[0], x), Math.min(cLo[1], y), Math.min(cLo[2], z)];
      cHi = [Math.max(cHi[0], x), Math.max(cHi[1], y), Math.max(cHi[2], z)];
    } else woodCount++;
  }
  const anchor = { x: gx, z: gz };
  const meta = {
    leafCount: leafCount, woodCount: woodCount,
    stems: S.stems.length, clumps: clumps.length,
    truncated: S.truncated, clipped: clipped,
    // Voxels the connectivity prune above threw away. A large number here is a
    // species whose foliage is not actually attached to it — worth seeing.
    dropped: S.dropped || 0,
    // Reach: the farthest a voxel of this variant sits from its own trunk
    // column, in each horizontal direction. Everything the engine's candidate
    // reject does is derived from this number, so it is measured from the
    // BAKED GRID and never from the parameters — a bound derived from params
    // and then invalidated by a change to the stamping is exactly the class of
    // bug that shears canopies (worldgen.wgsl's TREE_MAX note).
    reachXZ: Math.max(anchor.x, nx - 1 - anchor.x, anchor.z, nz - 1 - anchor.z),
    above: top + 1,
    crownY: leafCount ? Math.round(cSum[1] / leafCount) : Math.round(ny * 0.7),
    crownR: leafCount
        ? Math.round(Math.max(cHi[0] - cLo[0], cHi[2] - cLo[2]) / 2)
        : Math.round(Math.max(nx, nz) / 2),
    ms: (typeof performance !== 'undefined') ? performance.now() - t0 : 0
  };

  return {
    dim: { x: nx, y: ny, z: nz },
    cells: cells,
    anchor: anchor,
    names: names,
    meta: meta,
    skeleton: S
  };
}

// =============================================================================
// .svtree — the baked atlas
// =============================================================================
//
// ONE BINARY PER SPECIES, holding N pre-voxelized variants as a column-RLE
// grid. Deliberately dumb: the C++ reader is ~120 lines and the WGSL sampler
// is a bounds check, one column lookup and a short linear scan.
//
//   header (24 words)
//   name table       UTF-8, '\n'-separated, length-prefixed
//   variant dir      12 words per variant
//   per variant:  columns[nx*nz] as (runOffset, runCount) word pairs
//                 runs, one word each
//
// Every offset in the file is a WORD index from the start of the file.
//
// Materials in a run word are LOCAL palette indices into the name table, not
// engine material ids — the loader remaps. See generateTree's `opts.palette`
// note for why. The engine also IGNORES the baked state nibble and applies its
// own positional palette jitter (worldgen's `rnd % 3`), because a voxel word
// with an authored state cannot be represented by the page table's JITTER
// sentinel and a forest that defeats page compression is not worth three
// colours the engine already provides. The nibble is kept in the file because
// the editor preview and the .vox export path both read it.
//
// PLACEMENT RIDES ALONG (header words 12..19). "Where does this species grow"
// is a property of the species, so it lives in the species file; putting it in
// tuning.json instead would mean adding a tree touches two files and that the
// two can disagree. The C++ loader lifts it straight into the atlas directory.

export const SVTREE_MAGIC = 0x52545653;   // 'SVTR' little-endian
// v2 spends one of the three reserved header words on the BAKE SCALE
// (voxels/metre). Bumped rather than sneaked into a spare word silently,
// because the point of recording it is that the loader can REFUSE a mismatch —
// and a v1 file has no honest answer to "what scale is this", only the
// historical assumption that it is DEFAULT_VOX_PER_M.
export const SVTREE_VERSION = 2;
const HEADER_WORDS = 32;
const VARIANT_WORDS = 12;
/** Biome order, and it is the ENGINE's: worldgen.wgsl B_FOREST=0, B_MEADOW=1,
 *  B_PINE=2, B_DESERT=3. Written in this order so the loader can index the
 *  table by the biome id with no lookup. */
export const BIOME_ORDER = ['forest', 'meadow', 'pine', 'desert'];

// material(12) | state(4) | y0(Y0_BITS) | len(LEN_BITS). Mirrored in
// worldgen.wgsl's treeCellFrom and asserted by scripts/check_invariants.py —
// three places, one layout, so the widths live in the constants above rather
// than as literals here.
function packRun(word16, y0, runLen) {
  return ((word16 & 0xFFFF) | ((y0 & MAX_Y0) << 16) |
          ((runLen & MAX_RUN) << (16 + Y0_BITS))) >>> 0;
}
export function unpackRun(w) {
  return { word: w & 0xFFFF, y0: (w >>> 16) & MAX_Y0,
           len: (w >>> (16 + Y0_BITS)) & MAX_RUN };
}

/**
 * Bake `variants` seeds of one species into .svtree bytes.
 *
 * @param {object} params  authored species parameters
 * @param {number[]} seeds explicit seed list (defaults to 0..variants-1)
 * @returns {{buf:ArrayBuffer, meta:object}}
 */
export function bakeAtlas(params, seeds, opts) {
  const P = normalizeParams(params, opts && opts.vpm);
  const list = seeds && seeds.length ? seeds
      : Array.from({ length: Math.max(1, P.variants | 0) }, (_, i) => i);

  // P already carries the bake scale, and normalizeParams preserves it through
  // the re-normalize inside generateTree — so the variants cannot be baked at a
  // different scale than the header this function goes on to write.
  const trees = list.map(s => generateTree(P, s));

  // One name table across all variants — they share a params object so they
  // share materials, but do not ASSUME that: merge, and remap each variant's
  // local indices onto the merged table.
  const names = [];
  const nameIdx = new Map();
  const pal = (n) => {
    let g = nameIdx.get(n);
    if (g === undefined) { g = names.push(n); nameIdx.set(n, g); }
    return g;
  };
  const remaps = trees.map(t => {
    const m = new Uint16Array(t.names.length + 1);
    t.names.forEach((n, i) => { m[i + 1] = pal(n); });
    return m;
  });
  // The autumn ramp is named in the header but appears in no voxel, so it has
  // to be interned HERE, before the table is serialised — a name added after
  // the byte length is computed lands outside the file.
  // `< P.levels`, NOT `<=`. makeStem recurses while `level + 1 < P.levels`, so
  // the DEEPEST stem that exists is level `levels - 1` and a startLevel of
  // `levels` already means no clump is ever attached. The `<=` here declared an
  // autumn ramp for a tree with no leaves to turn.
  const turns = (P.autumnChance | 0) > 0 &&
                P.foliage.clumpsPerStem > 0 && P.foliage.startLevel < P.levels;
  if (turns) P.autumnLeaf.forEach(pal);

  // ---- name table ----------------------------------------------------------
  const nameBytes = new TextEncoder().encode(names.join('\n'));
  const nameWords = 1 + ((nameBytes.length + 3) >> 2);

  // ---- lay out --------------------------------------------------------------
  const nameOff = HEADER_WORDS;
  const varDirOff = nameOff + nameWords;
  let cursor = varDirOff + trees.length * VARIANT_WORDS;

  // Encode each variant's columns + runs into a scratch array first, so the
  // absolute offsets can be fixed up once the sizes are known.
  const bodies = trees.map((t, ti) => {
    const { x: nx, y: ny, z: nz } = t.dim;
    const remap = remaps[ti];
    const cols = new Uint32Array(nx * nz * 2);
    const runs = [];
    for (let z = 0; z < nz; z++) {
      for (let x = 0; x < nx; x++) {
        const start = runs.length;
        let y = 0;
        while (y < ny) {
          const w = t.cells[(z * ny + y) * nx + x];
          if (!w) { y++; continue; }
          const remapped = ((remap[w & 0xFFF] & 0xFFF) | (w & 0xF000)) >>> 0;
          let l = 1;
          while (y + l < ny && l < MAX_RUN) {
            const w2 = t.cells[(z * ny + y + l) * nx + x];
            if (w2 !== w) break;
            l++;
          }
          runs.push(packRun(remapped, y, l));
          y += l;
        }
        const ci = (z * nx + x) * 2;
        cols[ci] = start;                       // relative; fixed up below
        cols[ci + 1] = runs.length - start;
      }
    }
    return { cols: cols, runs: Uint32Array.from(runs) };
  });

  const varOffsets = [];
  for (let i = 0; i < trees.length; i++) {
    const colsOff = cursor;
    cursor += bodies[i].cols.length;
    const runsOff = cursor;
    cursor += bodies[i].runs.length;
    varOffsets.push({ colsOff, runsOff });
    // fix the column run offsets to absolute word indices
    const c = bodies[i].cols;
    for (let k = 0; k < c.length; k += 2) c[k] += runsOff;
  }

  const total = cursor;
  const out = new Uint32Array(total);

  // header
  out[0] = SVTREE_MAGIC;
  out[1] = SVTREE_VERSION;
  out[2] = HEADER_WORDS;
  out[3] = trees.length;
  out[4] = names.length;
  out[5] = nameOff;
  out[6] = varDirOff;
  out[7] = total;
  out[8] = Math.max(...trees.map(t => t.meta.reachXZ));
  out[9] = Math.max(...trees.map(t => t.meta.above));
  out[10] = Math.round(trees.reduce((a, t) => a + t.meta.crownY, 0) / trees.length);
  out[11] = Math.max(...trees.map(t => t.meta.crownR));
  const PL = P.placement;
  BIOME_ORDER.forEach((b, i) => {
    out[12 + i] = Math.max(0, Math.min(65535, Math.round(PL.biomes[b] || 0)));
  });
  // The FAR-FIELD PROXY material: at cascade levels where a whole tree is
  // thinner than one cell there is no tree left to sample, so the horizon
  // paints this over the terrain skin instead. The mid entry of the leaf ramp,
  // by name; 0 for a species with no foliage at all (the dead tree), which the
  // far field then simply does not paint.
  // Same off-by-one as `turns` above: the deepest stem level is `levels - 1`.
  const canopyName = (P.foliage.clumpsPerStem > 0 && P.foliage.startLevel < P.levels)
      ? P.leaf[1] : null;
  out[20] = canopyName ? (names.indexOf(canopyName) + 1) : 0;
  out[21] = Math.max(0, Math.min(255, PL.shade | 0));
  // The autumn substitution table: leaf ramp -> autumn ramp, as parallel local
  // palette indices. Only written when the species actually turns, so a conifer
  // costs three zero words and the engine skips the whole thing.
  out[22] = turns ? (P.autumnChance | 0) : 0;
  for (let i = 0; i < 3; i++) {
    out[23 + i] = nameIdx.get(P.leaf[i]) || 0;
    out[26 + i] = turns ? (nameIdx.get(P.autumnLeaf[i]) || 0) : 0;
  }
  // v2, word 29: the scale this file was baked at. The C++ loader compares it
  // against kVoxelsPerMetre and refuses the atlas by name on a mismatch, so a
  // stale bake is a loud failure instead of a forest at the wrong size.
  out[29] = P.vpm >>> 0; out[30] = 0; out[31] = 0;
  // Signed values go through the u32 array as two's complement; the C++ and
  // WGSL sides both read them back as i32.
  //
  // minY/maxY are authored in METRES and resolved to absolute world voxels
  // here, at the bake scale — they are compared against terrain height, which
  // IS scaled (worldgen.treeline goes through ReadWgLen). Leaving them as raw
  // voxels made every species' ceiling stay put while the ground doubled, which
  // silently stopped nine of ten species from spawning at 5 cm. Negative is the
  // "unbounded" sentinel (worldgen.wgsl checks `>= 0`) and must survive the
  // conversion, so it is passed through rather than scaled.
  const plY = (m) => (m < 0 ? -1 : Math.round(m * P.vpm));
  out[16] = plY(+PL.minY) >>> 0;
  out[17] = plY(+PL.maxY) >>> 0;
  out[18] = Math.max(0, PL.maxSlope | 0);
  out[19] = Math.max(1, PL.sparsity | 0);


  // name table
  out[nameOff] = nameBytes.length;
  const nb = new Uint8Array(out.buffer, (nameOff + 1) * 4, nameWords * 4 - 4);
  nb.set(nameBytes);

  // variant directory
  trees.forEach((t, i) => {
    const o = varDirOff + i * VARIANT_WORDS;
    out[o + 0] = t.dim.x;
    out[o + 1] = t.dim.y;
    out[o + 2] = t.dim.z;
    out[o + 3] = t.anchor.x;
    out[o + 4] = t.anchor.z;
    out[o + 5] = varOffsets[i].colsOff;
    out[o + 6] = varOffsets[i].runsOff;
    out[o + 7] = t.meta.reachXZ;
    out[o + 8] = t.meta.above;
    out[o + 9] = t.meta.crownY;
    out[o + 10] = t.meta.crownR;
    out[o + 11] = 0;
  });

  // bodies
  trees.forEach((t, i) => {
    out.set(bodies[i].cols, varOffsets[i].colsOff);
    out.set(bodies[i].runs, varOffsets[i].runsOff);
  });

  return {
    buf: out.buffer,
    meta: {
      variants: trees.length,
      names: names,
      bytes: total * 4,
      reachXZ: out[8], above: out[9], crownY: out[10], crownR: out[11],
      placement: P.placement,
      per: trees.map(t => t.meta)
    }
  };
}

/** Read a .svtree back. Used by the round-trip test and by the tuner's
 *  "verify bake" path; the engine has its own C++ reader of the same layout
 *  (src/sim/treeatlas.cpp), and `scripts/test_treegen.mjs` asserts the two
 *  agree by decoding a pinned variant cell-for-cell. */
export function readAtlas(buf) {
  const w = new Uint32Array(buf);
  if (w[0] !== SVTREE_MAGIC) throw new Error('not a .svtree (bad magic)');
  if (w[1] !== SVTREE_VERSION) throw new Error('.svtree version ' + w[1]);
  const nameOff = w[5], varDirOff = w[6];
  const nlen = w[nameOff];
  const nbytes = new Uint8Array(buf, (nameOff + 1) * 4, nlen);
  const names = nlen ? new TextDecoder().decode(nbytes).split('\n') : [];
  const variants = [];
  for (let i = 0; i < w[3]; i++) {
    const o = varDirOff + i * VARIANT_WORDS;
    variants.push({
      nx: w[o], ny: w[o + 1], nz: w[o + 2],
      anchorX: w[o + 3], anchorZ: w[o + 4],
      colsOff: w[o + 5], runsOff: w[o + 6],
      reachXZ: w[o + 7], above: w[o + 8], crownY: w[o + 9], crownR: w[o + 10]
    });
  }
  const biomes = {};
  BIOME_ORDER.forEach((b, i) => { biomes[b] = w[12 + i]; });
  return {
    words: w, names: names, variants: variants,
    // The bake scale (v2, word 29). Every voxel figure below — reach, above,
    // crownY/R, the variant dims, minY/maxY — is in THIS scale's cells, not the
    // engine's, and the two are only interchangeable because the loader refuses
    // a file whose vpm does not match kVoxelsPerMetre.
    vpm: w[29],
    reachXZ: w[8], above: w[9], crownY: w[10], crownR: w[11],
    placement: { biomes: biomes, minY: w[16] | 0, maxY: w[17] | 0,
                 maxSlope: w[18], sparsity: w[19], shade: w[21] },
    canopyMat: w[20] ? names[w[20] - 1] : null,
    autumnChance: w[22],
    leafRamp: [w[23], w[24], w[25]].map(i => i ? names[i - 1] : null),
    autumnRamp: [w[26], w[27], w[28]].map(i => i ? names[i - 1] : null),
    /** The exact decode path the shader takes, in JS: bounds, column, scan. */
    cellAt(vi, lx, ly, lz) {
      const v = variants[vi];
      if (lx < 0 || lz < 0 || ly < 0 || lx >= v.nx || ly >= v.ny || lz >= v.nz) return 0;
      const ci = v.colsOff + (lz * v.nx + lx) * 2;
      const off = w[ci], cnt = w[ci + 1];
      for (let k = 0; k < cnt; k++) {
        const r = unpackRun(w[off + k]);
        if (ly < r.y0) return 0;
        if (ly < r.y0 + r.len) return r.word;
      }
      return 0;
    }
  };
}

// =============================================================================
// PRESETS
// =============================================================================
//
// Hand-authored species, from the botany the algorithm's parameters actually
// encode. These ship as assets/trees/<name>.json; the objects here are the
// fallback the editor offers as "new from preset" and what
// scripts/bake_trees.mjs writes on a fresh checkout.
//
// The shape numbers are the identity. Read them first:
//   0 conical      -> spruce, fir, young pine
//   2 hemispherical-> oak, most broadleaves
//   4 tapered cyl  -> redwood, sequoia (a column, not a cone)
//   5 flame        -> birch, aspen, elm
//   6 inv conical  -> weeping willow, some eucalypts

function preset(over) {
  const p = defaultParams();
  const lv = over.levelsData;
  delete over.levelsData;
  Object.assign(p, over);
  if (over.foliage) p.foliage = Object.assign(defaultParams().foliage, over.foliage);
  if (over.placement) {
    p.placement = Object.assign(defaultParams().placement, over.placement);
    if (over.placement.biomes)
      p.placement.biomes = Object.assign({ forest: 0, meadow: 0, pine: 0, desert: 0 },
                                         over.placement.biomes);
  }
  if (lv) for (let i = 0; i < 4; i++) if (lv[i]) Object.assign(p.levelsData[i], lv[i]);
  return p;
}

export const PRESETS = {

  // A field oak: short bole, low scaffold split, heavy gnarl, a few LARGE
  // welded clumps. The splits at level 0 are what give it the wide crotch that
  // says "oak" before anything else does.
  oak: preset({
    name: 'oak', autumnChance: 5, displayName: 'Oak', variants: 3,
    shape: 2, baseSize: 0.30, scale: 7.5, scaleV: 1.4, levels: 3,
    ratio: 0.042, ratioPower: 0.95, flare: 1.0, attractionUp: 0.42,
    lobes: 5, lobeDepth: 0.09, barkNoise: 0.085,
    levelsData: [
      { length: 1.0, taper: 0.5, curveRes: 7, curve: 8, curveV: 45,
        segSplits: 0.25, splitAngle: 24, splitAngleV: 10 },
      { length: 0.62, lengthV: 0.22, branches: 8, downAngle: 48, downAngleV: -38,
        rotate: 137.5, rotateV: 22, curveRes: 6, curve: -20, curveBack: 26,
        curveV: 60, taper: 0.85, segSplits: 0.12, splitAngle: 22 },
      { length: 0.46, lengthV: 0.28, branches: 5, downAngle: 44, downAngleV: 22,
        rotate: 137.5, rotateV: 30, curveRes: 4, curve: 14, curveV: 75, taper: 0.9 },
      { length: 0.4, branches: 4, downAngle: 45, rotate: 137.5, curveRes: 3,
        curveV: 60, taper: 1.0 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.0, tipBias: 0.5, radius: 0.62,
               radiusV: 0.2, elongation: 0.72, droop: 0.18, density: 0.8,
               noiseScale: 0.28, sminK: 0.22, canopyShadeMix: 0.5, depthShade: 0.8 },
    bark: ['bark_dark', 'wood', 'bark_light'],
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    placement: { biomes: { forest: 42, meadow: 16, pine: 4, desert: 0 },
                 maxY: 21.4, maxSlope: 420, sparsity: 1, shade: 210 }
  }),

  // The ancient one. Same species, twice the mass: a thicker bole, a deeper
  // buttress, a second tier of scaffold, and clumps big enough to close a
  // canopy on their own. Kept as its own file rather than a size jitter of the
  // oak because its PLACEMENT is different — it is rare, and it wants room.
  great_oak: preset({
    name: 'great_oak', autumnChance: 6, displayName: 'Great oak', variants: 3,
    shape: 2, baseSize: 0.26, scale: 11.0, scaleV: 1.6, levels: 4,
    ratio: 0.052, ratioPower: 0.9, flare: 1.6, attractionUp: 0.34,
    lobes: 6, lobeDepth: 0.13, barkNoise: 0.1,
    levelsData: [
      { length: 1.0, taper: 0.45, curveRes: 8, curve: 10, curveV: 55,
        segSplits: 0.3, splitAngle: 28, splitAngleV: 12 },
      { length: 0.60, lengthV: 0.24, branches: 8, downAngle: 55, downAngleV: -44,
        rotate: 137.5, rotateV: 24, curveRes: 7, curve: -24, curveBack: 30,
        curveV: 70, taper: 0.80, segSplits: 0.12, splitAngle: 24 },
      { length: 0.48, lengthV: 0.3, branches: 5, downAngle: 48, downAngleV: 26,
        rotate: 137.5, rotateV: 32, curveRes: 5, curve: 16, curveV: 80, taper: 0.85 },
      { length: 0.44, lengthV: 0.3, branches: 3, downAngle: 44, downAngleV: 24,
        rotate: 137.5, rotateV: 34, curveRes: 3, curveV: 70, taper: 0.95 }
    ],
    foliage: { startLevel: 3, clumpsPerStem: 1.0, tipBias: 0.45, radius: 0.72,
               radiusV: 0.24, elongation: 0.70, droop: 0.22, density: 0.82,
               noiseScale: 0.3, sminK: 0.26, canopyShadeMix: 0.45, depthShade: 0.85 },
    placement: { biomes: { forest: 12, meadow: 5, pine: 0, desert: 0 },
                 maxY: 20.6, maxSlope: 340, sparsity: 1, shade: 255 }
  }),

  // Pine: a bare lower bole, WHORLED branches (the parameter vanilla
  // Weber-Penn cannot make a conifer without), branches that leave the trunk
  // downswept and curve their tips back up, and flattened needle plates rather
  // than round tufts.
  pine: preset({
    name: 'pine', displayName: 'Pine', variants: 3,
    shape: 0, baseSize: 0.42, scale: 12.0, scaleV: 2.4, levels: 3,
    ratio: 0.022, ratioPower: 1.5, flare: 0.5, attractionUp: 0.5,
    whorlCount: 5, lobes: 0, barkNoise: 0.09,
    levelsData: [
      { length: 1.0, taper: 0.92, curveRes: 12, curve: 0, curveV: 18, segSplits: 0 },
      { length: 0.34, lengthV: 0.14, branches: 24, downAngle: 78, downAngleV: -36,
        rotate: 74, rotateV: 14, curveRes: 5, curve: -28, curveV: 26, taper: 0.9 },
      { length: 0.44, lengthV: 0.2, branches: 6, downAngle: 52, downAngleV: 18,
        rotate: 137.5, rotateV: 24, curveRes: 3, curve: -14, curveV: 34, taper: 1.0 },
      { length: 0.4, branches: 4, downAngle: 45, rotate: 137.5, curveRes: 2, taper: 1.0 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.6, tipBias: 0.16, radius: 0.44,
               radiusV: 0.12, elongation: 1.9, droop: 0.05, density: 0.78,
               noiseScale: 0.22, sminK: 0.2, canopyShadeMix: 0.55, depthShade: 0.7 },
    bark: ['bark_dark', 'wood', 'bark_light'],
    leaf: ['pine_dark', 'pine_needles', 'pine_lit'],
    placement: { biomes: { forest: 14, meadow: 2, pine: 60, desert: 0 },
                 maxY: 22.6, maxSlope: 560, sparsity: 1, shade: 230 }
  }),

  // Spruce: the pine's colder sibling. Narrower envelope, shorter branches,
  // steeper droop, denser plates — the classic Christmas-tree silhouette that
  // `shape 0` plus a short `1Length` produces almost by itself.
  spruce: preset({
    name: 'spruce', displayName: 'Spruce', variants: 3,
    shape: 0, baseSize: 0.14, scale: 13.5, scaleV: 2.8, levels: 3,
    ratio: 0.019, ratioPower: 1.6, flare: 0.35, attractionUp: 0.25,
    whorlCount: 6, barkNoise: 0.07,
    levelsData: [
      { length: 1.0, taper: 0.96, curveRes: 14, curve: 0, curveV: 10 },
      { length: 0.28, lengthV: 0.1, branches: 34, downAngle: 92, downAngleV: -30,
        rotate: 62, rotateV: 10, curveRes: 5, curve: -34, curveV: 18, taper: 0.9 },
      { length: 0.5, lengthV: 0.16, branches: 7, downAngle: 48, downAngleV: 14,
        rotate: 137.5, rotateV: 20, curveRes: 3, curve: -10, curveV: 24, taper: 1.0 },
      { length: 0.4, branches: 3, downAngle: 45, rotate: 137.5, curveRes: 2 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.5, tipBias: 0.10, radius: 0.40,
               radiusV: 0.1, elongation: 2.3, droop: 0.02, density: 0.84,
               noiseScale: 0.2, sminK: 0.18, canopyShadeMix: 0.6, depthShade: 0.72 },
    bark: ['bark_dark', 'wood', 'bark_light'],
    leaf: ['pine_dark', 'pine_needles', 'pine_lit'],
    placement: { biomes: { forest: 4, meadow: 0, pine: 34, desert: 0 },
                 minY: 19, maxY: 23.6, maxSlope: 640, sparsity: 1, shade: 240 }
  }),

  // Birch: slender, flame-shaped, with drooping twig ends (the NEGATIVE
  // attractionUp at the last level is the whole trick) and airy clumps. The
  // white bark is legible only if the structure stays visible, so density is
  // low and the clumps stay small.
  birch: preset({
    name: 'birch', autumnChance: 4, displayName: 'Birch', variants: 3,
    shape: 5, baseSize: 0.34, scale: 9.0, scaleV: 1.6, levels: 3,
    ratio: 0.017, ratioPower: 1.35, flare: 0.4, attractionUp: -0.18,
    barkNoise: 0.04,
    levelsData: [
      { length: 1.0, taper: 0.85, curveRes: 10, curve: 4, curveV: 40, segSplits: 0.15,
        splitAngle: 14 },
      { length: 0.52, lengthV: 0.24, branches: 11, downAngle: 44, downAngleV: -32,
        rotate: 137.5, rotateV: 26, curveRes: 6, curve: 22, curveV: 60, taper: 0.90 },
      { length: 0.52, lengthV: 0.3, branches: 5, downAngle: 36, downAngleV: 26,
        rotate: 137.5, rotateV: 34, curveRes: 4, curve: 38, curveV: 75, taper: 0.95 },
      { length: 0.4, branches: 4, downAngle: 40, rotate: 137.5, curveRes: 3, curveV: 60 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.2, tipBias: 0.45, radius: 0.40,
               radiusV: 0.14, elongation: 0.85, droop: 0.35, density: 0.68,
               noiseScale: 0.24, sminK: 0.16, canopyShadeMix: 0.4, depthShade: 0.7 },
    bark: ['wood', 'birch_wood', 'birch_wood'],
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    placement: { biomes: { forest: 18, meadow: 30, pine: 8, desert: 0 },
                 maxY: 22, maxSlope: 460, sparsity: 1, shade: 95 }
  }),

  // Redwood: a COLUMN, not a cone. `shape 4` (tapered cylindrical) plus very
  // short level-1 branches is the entire recipe; the deep flare and the huge
  // scale do the rest. Many small dense tufts rather than a few big lobes.
  redwood: preset({
    name: 'redwood', displayName: 'Redwood', variants: 2,
    shape: 4, baseSize: 0.55, scale: 22.0, scaleV: 4.0, levels: 3,
    ratio: 0.028, ratioPower: 1.7, flare: 2.2, attractionUp: 0.55,
    whorlCount: 4, lobes: 7, lobeDepth: 0.10, barkNoise: 0.12,
    levelsData: [
      { length: 1.0, taper: 0.86, curveRes: 16, curve: 0, curveV: 12 },
      { length: 0.15, lengthV: 0.06, branches: 40, downAngle: 84, downAngleV: -28,
        rotate: 88, rotateV: 12, curveRes: 4, curve: -22, curveV: 20, taper: 0.9 },
      { length: 0.55, lengthV: 0.2, branches: 6, downAngle: 46, downAngleV: 16,
        rotate: 137.5, rotateV: 24, curveRes: 3, curveV: 26, taper: 1.0 },
      { length: 0.4, branches: 3, downAngle: 45, rotate: 137.5, curveRes: 2 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.6, tipBias: 0.12, radius: 0.36,
               radiusV: 0.1, elongation: 1.35, droop: 0.1, density: 0.86,
               noiseScale: 0.2, sminK: 0.16, canopyShadeMix: 0.6, depthShade: 0.75 },
    bark: ['bark_dark', 'wood', 'bark_light'],
    leaf: ['pine_dark', 'pine_needles', 'pine_lit'],
    placement: { biomes: { forest: 3, meadow: 0, pine: 10, desert: 0 },
                 maxY: 21.8, maxSlope: 300, sparsity: 2, shade: 235 }
  }),

  // Eucalypt: the GAPS are the identity. Sparse branches, foliage hanging
  // BELOW the branch (high droop) in heavily eroded hanging curtains, and a
  // pale smooth bark with almost no relief.
  eucalyptus: preset({
    name: 'eucalyptus', displayName: 'Eucalyptus', variants: 3,
    shape: 6, baseSize: 0.44, scale: 14.0, scaleV: 3.0, levels: 3,
    ratio: 0.021, ratioPower: 1.4, flare: 0.6, attractionUp: -0.1,
    barkNoise: 0.03,
    levelsData: [
      { length: 1.0, taper: 0.8, curveRes: 10, curve: 14, curveV: 60, segSplits: 0.3,
        splitAngle: 18 },
      { length: 0.48, lengthV: 0.3, branches: 8, downAngle: 40, downAngleV: -30,
        rotate: 137.5, rotateV: 40, curveRes: 6, curve: 26, curveV: 85, taper: 0.95 },
      { length: 0.45, lengthV: 0.34, branches: 5, downAngle: 34, downAngleV: 30,
        rotate: 137.5, rotateV: 46, curveRes: 4, curve: 30, curveV: 100, taper: 1.0 },
      { length: 0.4, branches: 4, downAngle: 40, rotate: 137.5, curveRes: 2 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.2, tipBias: 0.55, radius: 0.5,
               radiusV: 0.2, elongation: 1.5, droop: 0.75, density: 0.52,
               noiseScale: 0.28, sminK: 0.14, canopyShadeMix: 0.35, depthShade: 0.65 },
    bark: ['wood', 'birch_wood', 'birch_wood'],
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    placement: { biomes: { forest: 5, meadow: 12, pine: 0, desert: 8 },
                 maxY: 21.2, maxSlope: 500, sparsity: 1, shade: 110 }
  }),

  // Willow: negative attractionUp all the way down, an inverse-conical
  // envelope and long thin last-level stems. Everything hangs.
  willow: preset({
    name: 'willow', displayName: 'Willow', variants: 2,
    shape: 6, baseSize: 0.2, scale: 8.0, scaleV: 1.4, levels: 3,
    ratio: 0.032, ratioPower: 1.25, flare: 1.1, attractionUp: -0.85,
    lobes: 4, lobeDepth: 0.1, barkNoise: 0.09,
    levelsData: [
      { length: 1.0, taper: 0.6, curveRes: 8, curve: 6, curveV: 60, segSplits: 0.4,
        splitAngle: 28 },
      { length: 0.72, lengthV: 0.2, branches: 10, downAngle: 34, downAngleV: -28,
        rotate: 137.5, rotateV: 26, curveRes: 7, curve: 55, curveV: 65, taper: 0.88 },
      { length: 0.78, lengthV: 0.24, branches: 6, downAngle: 22, downAngleV: 20,
        rotate: 137.5, rotateV: 30, curveRes: 6, curve: 85, curveV: 50, taper: 0.95 },
      { length: 0.4, branches: 4, downAngle: 30, rotate: 137.5, curveRes: 3 }
    ],
    foliage: { startLevel: 2, clumpsPerStem: 1.4, tipBias: 0.28, radius: 0.38,
               radiusV: 0.12, elongation: 1.6, droop: 0.6, density: 0.66,
               noiseScale: 0.24, sminK: 0.15, canopyShadeMix: 0.4, depthShade: 0.7 },
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    placement: { biomes: { forest: 6, meadow: 14, pine: 0, desert: 0 },
                 maxY: 20.4, maxSlope: 260, sparsity: 1, shade: 190 }
  }),

  // Bush: one level, a stub of a stem, and clumps doing all the work. It is a
  // TREE by the same machinery, which is the point — the shrub layer and the
  // canopy layer are the same authoring surface.
  bush: preset({
    name: 'bush', displayName: 'Bush', variants: 3,
    shape: 1, baseSize: 0.05, scale: 1.1, scaleV: 0.35, levels: 3,
    ratio: 0.055, ratioPower: 1.0, flare: 0.3, attractionUp: 0.2, barkNoise: 0.025,
    levelsData: [
      { length: 1.0, taper: 0.7, curveRes: 4, curveV: 60, segSplits: 0.8,
        splitAngle: 42, splitAngleV: 16 },
      { length: 0.85, lengthV: 0.3, branches: 6, downAngle: 42, downAngleV: -26,
        rotate: 137.5, rotateV: 40, curveRes: 3, curve: 20, curveV: 70, taper: 0.9 },
      { length: 0.7, lengthV: 0.35, branches: 4, downAngle: 40, downAngleV: 30,
        rotate: 137.5, rotateV: 46, curveRes: 2, curve: 24, curveV: 80, taper: 1.0 },
      { length: 0.4, branches: 2, downAngle: 40, rotate: 137.5, curveRes: 2 }
    ],
    // Small clumps and a low density, because a shrub is the one plant the
    // player stands NEXT TO: at 2 m away a solid dome of leaves is a green
    // wall, and the gaps are the only thing that says "bush" rather than
    // "hedge block".
    foliage: { startLevel: 2, clumpsPerStem: 1.4, tipBias: 0.25, radius: 0.22,
               radiusV: 0.08, elongation: 0.85, droop: 0.05, density: 0.7,
               noiseScale: 0.18, sminK: 0.12, canopyShadeMix: 0.5, depthShade: 0.6 },
    leaf: ['leaves_dark', 'leaves', 'leaves_lit'],
    placement: { biomes: { forest: 16, meadow: 40, pine: 12, desert: 26 },
                 maxY: 23.2, maxSlope: 700, sparsity: 1, shade: 0 }
  }),

  // Dead tree: bare structure, no foliage at all. Cheap to place, and the one
  // thing a forest needs to stop reading as a nursery.
  dead: preset({
    name: 'dead', displayName: 'Dead tree', variants: 2,
    shape: 2, baseSize: 0.3, scale: 6.5, scaleV: 1.6, levels: 3,
    ratio: 0.03, ratioPower: 1.3, flare: 0.9, attractionUp: 0.5,
    lobes: 4, lobeDepth: 0.12, barkNoise: 0.11,
    levelsData: [
      { length: 1.0, taper: 0.75, curveRes: 7, curve: 12, curveV: 90,
        segSplits: 0.4, splitAngle: 30 },
      { length: 0.5, lengthV: 0.34, branches: 8, downAngle: 52, downAngleV: -40,
        rotate: 137.5, rotateV: 40, curveRes: 5, curve: -20, curveV: 110, taper: 1.0 },
      { length: 0.42, lengthV: 0.4, branches: 5, downAngle: 46, downAngleV: 30,
        rotate: 137.5, rotateV: 50, curveRes: 3, curveV: 120, taper: 1.0 },
      { length: 0.4, branches: 3, downAngle: 45, rotate: 137.5, curveRes: 2 }
    ],
    foliage: { startLevel: 9, clumpsPerStem: 0, radius: 0.1 },
    bark: ['bark_dark', 'wood', 'bark_light'],
    placement: { biomes: { forest: 4, meadow: 3, pine: 4, desert: 10 },
                 maxSlope: 700, sparsity: 1, shade: 0 }
  })
};

export const PRESET_ORDER = ['oak', 'great_oak', 'birch', 'pine', 'spruce',
                             'redwood', 'eucalyptus', 'willow', 'bush', 'dead'];
