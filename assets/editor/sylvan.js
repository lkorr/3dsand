/* sylvan.js — THE SYLVAN RACE: wood spirits built on the human rig.
 *
 * WHAT A SYLVAN IS, TO THE GENERATOR. A human body — the SAME archetype, the
 * same fifteen limb boxes, the same anchors, sockets, chains and clips — whose
 * surface is bark instead of skin, wound with roots, crowned with leaves,
 * branches and flowers, with a warped wooden face and eyes that glow from
 * deep sockets. Nothing about the rig is racial, and that is the point: every
 * helm, cuirass, greave and weapon in the game is FIT to the human's boxes
 * (mob.cpp cover.fitBox), so a sylvan wears and wields all of it unchanged.
 * Everything here happens INSIDE a limb's own box, or in the bloodless `hair`
 * parts that already exist for things that stand off the body (hairMass).
 *
 * WHAT CHANGES, and where:
 *   - MATERIALS. A human is one material (`skin`) with colour in the art
 *     layer. A sylvan is several, picked BY ART SLOT at write time
 *     (slotMaterials): bark, darker root bark, leaves, branch wood and — for
 *     the eyes — `gem_arcane`, whose emission is what makes them glow (the
 *     art colour is the glow's colour; microbody.wgsl shades emission off the
 *     material). Every one is an existing material with id <= 127, so the
 *     .vox can paint it and no engine code learned anything.
 *   - THE INSIDE. anatomy recipe ANATOMY below: pale sapwood under the bark,
 *     heartwood at the core, and a glowing `crystal` heart in the chest and
 *     behind the eyes. It BLEEDS SAP (`syrup`).
 *   - THE SURFACE. barkPass: grain, knots, moss, and roots wound round every
 *     limb that stand in RELIEF — the bark beside each strand is carved one
 *     cell down, so the silhouette never leaves the box an armour piece is
 *     fit to. On the head: deep eye sockets with the glow at the bottom, a
 *     heavy brow, a carved mouth, a warped and dented skull.
 *   - THE CROWN. crownExtras, called from inside hairMass: a wreath, leaf
 *     clumps, antler-like branches that fork, flowers on all of it, leaves
 *     hanging over the face (see-through, so the eyes show), and a deku-style
 *     O-mouth snout. All of it is bloodless hair-part geometry: burning or
 *     cutting it is a haircut, never a wound.
 *
 * PURE AND NODE-RUNNABLE, like mobgen.js, and imports nothing from it (mobgen
 * imports this). Every random choice is a counter hash of the genome's own
 * numbers, so a body is a pure function of its genome.
 *
 * HUMANS ARE UNTOUCHED. Every entry point here is only reached when
 * `genome.body.race === 'sylvan'`, and every sylvan gene is skipped by the
 * mutation / roll / cross loops of a human genome WITHOUT consuming a random
 * draw — so every seeded human litter, the pinned cross output and every
 * saved character build exactly as they did before this file existed.
 */

'use strict';

// ---- hashing (the same mix as mobgen.js hashN; duplicated so this module
//      has no import cycle) -------------------------------------------------
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x >>> 0;
}
function h32(...vals) {
  let h = 0x9e3779b9;
  for (let i = 0; i < vals.length; i++) {
    h = mix32((h ^ (vals[i] | 0)) >>> 0);
    h = (h + 0x9e3779b9) >>> 0;
  }
  return mix32(h);
}
const rnd = (...v) => h32(...v) / 4294967296;
const strHash = s => { let h = 7; for (const ch of String(s)) h = h32(h, ch.charCodeAt(0)); return h; };
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

export const RACES = ['human', 'sylvan'];

// =============================================================================
// art slots and materials
// =============================================================================

/** The sylvan's own art slots, continuing mobgen's ART table DOWNWARD past the
 *  wisps (244). A human never paints them and never writes them into its
 *  palette (mobgen paletteBytes), so they cost the engine's shared art palette
 *  nothing until a sylvan is loaded. */
export const SLOTS = {
  ROOT: 243,         // root strands, socket walls, knots, the mouth's hollow
  MOSS: 242,
  BRANCH: 241,
  FLOWER: 240,
  FLOWER_EYE: 239,   // a flower's centre
  // THREE MORE LEAF COLOURS. Real foliage is never one green: fresh growth,
  // old dark leaves, dry brown ones. Mixed into every leaf the crown has
  // (leafVariant) in patches or speckle, and whole clumps take one of them.
  LEAF2: 238,
  LEAF3: 237,
  LEAF4: 236,
};

/** genome.colors key -> slot, for the five colours only a sylvan uses. */
export const COLOR_SLOTS = {
  root: SLOTS.ROOT, moss: SLOTS.MOSS, branch: SLOTS.BRANCH,
  flower: SLOTS.FLOWER, flowerEye: SLOTS.FLOWER_EYE,
  leaf2: SLOTS.LEAF2, leaf3: SLOTS.LEAF3, leaf4: SLOTS.LEAF4,
};

/** Every slot that IS a leaf on a sylvan (the hair slots plus the variants):
 *  what the crown roots on and what leafVariant may recolour. */
export const LEAF_SLOTS_OF = ART => [ART.HAIR, ART.HAIR_SHADE, SLOTS.LEAF2,
                                     SLOTS.LEAF3, SLOTS.LEAF4];

/** What each kind of surface is MADE of. By name, resolved at write time, for
 *  the reason mobgen's FLESH_ID is: an id is an index into materials.json.
 *  All flammable wood and foliage except the eyes, which are the one mineral
 *  on the body. Burning a sylvan runs the world's own wood chain (ember,
 *  then ash and smoke) through the body evaluator, the same table the grid
 *  uses; leaves go up far faster than bark, which is right. */
export const MATS = {
  bark: 'bark_light',
  root: 'bark_dark',
  leaf: 'leaves',
  branch: 'staff_wood',
  glow: 'gem_arcane',
};

/** What runs out of a cut: sap. `syrup` is the amber liquid already in the
 *  table; a sylvan's cut wood soaks it (mob.cpp bleed.woundMaterial defaults
 *  to the bleed material). */
export const SAP = 'syrup';

/** The anatomy recipe (anatomy.js format). DISJOINT FROM THE SURFACE
 *  MATERIALS ON PURPOSE: anatomy.js rewrites a kept surface voxel whose
 *  material the recipe also puts underneath (it reads such a voxel as a
 *  stale interior face), so bark / roots / leaves / branch / glow never
 *  appear below. */
export const ANATOMY = {
  layers: [
    { material: 'bark_light', depth: 1, keep: true },
    { material: 'birch_wood', depth: 2 },
    { material: 'wood' },
  ],
  garments: [],
  limbs: {
    head: {
      layers: [
        { material: 'bark_light', depth: 1, keep: true },
        { material: 'birch_wood', depth: 1 },
        { material: 'wood', depth: 1 },
        { material: 'crystal' },
      ],
    },
    torso: {
      carve: [
        { material: 'crystal', where: 'wood',
          box: { x: [0.36, 0.64], y: [0.5, 0.74], z: [0.35, 0.78] } },
      ],
    },
  },
};

export const ANATOMY_NOTES = {
  '//': 'A SYLVAN: bark over pale sapwood over heartwood, no flesh anywhere. ' +
        'The surface is several materials (bark, root bark, leaves, branch ' +
        'wood, and gem_arcane in the eyes, which is what makes them glow) and ' +
        'every one of them is KEPT; the recipe below them uses none of them. ' +
        'A glowing crystal heart sits in the chest and behind the eyes.',
};

/** Art slot -> material name, for a sylvan body. Anything not listed keeps the
 *  caller's default (bark). */
export function slotMaterialNames(g, ART) {
  const m = {};
  for (const s of [ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT, ART.NAIL])
    m[s] = MATS.bark;
  m[SLOTS.ROOT] = MATS.root;
  for (const s of [ART.HAIR, ART.HAIR_SHADE, ART.HAIR_WISP1, ART.HAIR_WISP2,
                   ART.HAIR_WISP3, ART.CLOTH, ART.CLOTH_SHADE, SLOTS.MOSS,
                   SLOTS.FLOWER, SLOTS.FLOWER_EYE, SLOTS.LEAF2, SLOTS.LEAF3,
                   SLOTS.LEAF4])
    m[s] = MATS.leaf;
  m[SLOTS.BRANCH] = MATS.branch;
  m[ART.EYE] = g.sylvan && g.sylvan.glow ? MATS.glow : MATS.root;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export function defaultSylvan() {
  return {
    face: 'hollow',   // a face preset (FACES); the numbers below are the truth
    crown: 'leafy',   // a crown preset (CROWNS)
    glow: true,       // eyes are gem_arcane: they give light
    sockets: 0.65,    // how deep the eyes sit: 0 painted on .. 1 three cells
    eyeSize: 1,       // 0 pinpoints .. 2 wide
    brow: 0.5,        // a heavy bark ridge over the eyes
    mouthHole: 0.3,   // a carved mouth: 0 none .. 1 a wide dark hollow
    snout: 0,         // the deku O-mouth: a tube standing off the face
    warp: 0.35,       // dents and knobs on the skull
    grain: 0.6,       // streaks along the grain
    knots: 0.3,
    roots: 0.55,      // root strands wound round the limbs
    rootTwist: 0.5,   // how tightly they spiral (crossing strands)
    relief: 0.7,      // how much the bark beside a root is carved away
    moss: 0.2,        // moss on everything that faces up
    wrap: 0.45,       // the leaf wrap at the hips: 0 bare bark .. 1 whole
    wreath: 0.3,      // a circlet of leaves round the brow
    leaves: 0.55,     // leaf clumps on the crown
    leafSize: 0.5,
    branches: 0.2,    // twigs growing off the head
    branchLen: 0.4,
    branchFork: 0.5,
    branchStyle: 'sprigs', // a branch preset (BRANCH_STYLES); the numbers rule
    shoulderBranches: 0,   // boughs out of each shoulder
    backBranches: 0,       // boughs out of the upper back
    boughLen: 0.5,         // their length
    tufts: 0.6,            // share of branch tips carrying a tuft of leaves
    flowers: 0.15,
    flowerSize: 0,    // 0 a five-petal cross .. 1 a full bloom
    leafMix: 0.35,    // share of the foliage in the three other leaf colours
    leafPatch: 0.5,   // 0 leaf-by-leaf speckle .. 1 big patches of one colour
    veil: 0,          // leaves hanging over the face
  };
}

/** The sylvan's default colours: grey-brown ash bark (the reference), spring
 *  leaves, green glow, white flowers. */
export const DEFAULT_COLORS = {
  skin: '#8b7b66', skinShade: '#5f5344', skinLight: '#a8987f', nail: '#4a3f33',
  hair: '#6fa843', hairShade: '#3f7327',
  eye: '#b6ff6a',
  cloth: '#5d7a34', clothShade: '#3f5623',
  root: '#4a3a2b', moss: '#7f9a3e', branch: '#6b5540',
  flower: '#f2eee2', flowerEye: '#e8c34a',
  leaf2: '#a6c94a', leaf3: '#2f5a24', leaf4: '#7a5a2e',
};

/** Colour sets a roll picks WHOLE (mobgen.rollColors' argument for humans,
 *  applied to wood). */
export const BARKS = {
  ash:    { skin: '#8b7b66', skinShade: '#5f5344', skinLight: '#a8987f', nail: '#4a3f33', root: '#4a3a2b', branch: '#6b5540' },
  oak:    { skin: '#6e5236', skinShade: '#4e3a26', skinLight: '#8a6a48', nail: '#3a2a1a', root: '#33251a', branch: '#5a4128' },
  birch:  { skin: '#cfc6b4', skinShade: '#a39a88', skinLight: '#e6dfd0', nail: '#5a5248', root: '#3e3a34', branch: '#8a7d68' },
  dark:   { skin: '#4a3a2c', skinShade: '#32271e', skinLight: '#5f4b3a', nail: '#221a13', root: '#1e1712', branch: '#3e3024' },
  cedar:  { skin: '#8a4e34', skinShade: '#643824', skinLight: '#a8664a', nail: '#40241a', root: '#3a2016', branch: '#6e3e28' },
  drift:  { skin: '#b8ab94', skinShade: '#8e8370', skinLight: '#d2c7b0', nail: '#5e5546', root: '#5a4e40', branch: '#9a8c74' },
  mossy:  { skin: '#6d6a4c', skinShade: '#4b4934', skinLight: '#878463', nail: '#2e2c1e', root: '#3a3826', branch: '#5a5238' },
};
/** Each foliage set is FIVE leaf colours that belong together: the main one
 *  and its shade, then leaf2 / leaf3 / leaf4 for the mix (fresh or light,
 *  dark or old, dry or turned). An autumn set is mostly turning leaves with
 *  a little late green; a spring set is greens with one brown. */
export const LEAVES = {
  spring:  { hair: '#6fa843', hairShade: '#3f7327', leaf2: '#a6c94a', leaf3: '#2f5a24', leaf4: '#7a5a2e', cloth: '#5d7a34', clothShade: '#3f5623', moss: '#7f9a3e' },
  forest:  { hair: '#3c7a34', hairShade: '#24521f', leaf2: '#5e9a3a', leaf3: '#1c3a18', leaf4: '#5a4424', cloth: '#3f6a30', clothShade: '#2a4a20', moss: '#5c7a36' },
  autumn:  { hair: '#c8742a', hairShade: '#8e4a1a', leaf2: '#d8b440', leaf3: '#a8322a', leaf4: '#6a4424', cloth: '#9a5a26', clothShade: '#6e3e1a', moss: '#8a8a3a' },
  crimson: { hair: '#a8322a', hairShade: '#6e1e1a', leaf2: '#c8742a', leaf3: '#5e1a2a', leaf4: '#5a3a22', cloth: '#7a2a24', clothShade: '#521a16', moss: '#6a7a3a' },
  gold:    { hair: '#d8b440', hairShade: '#9c7c22', leaf2: '#e8d070', leaf3: '#8aa03a', leaf4: '#9a6a2a', cloth: '#a88a34', clothShade: '#7a6224', moss: '#8a9a40' },
  silver:  { hair: '#9fb89a', hairShade: '#6f8a6c', leaf2: '#c8d8c0', leaf3: '#5a7058', leaf4: '#8a8270', cloth: '#7a8e74', clothShade: '#586a54', moss: '#8aa07a' },
  blossom: { hair: '#e8a0c0', hairShade: '#b06a8e', leaf2: '#f4d0e0', leaf3: '#6fa843', leaf4: '#c07090', cloth: '#6a8a44', clothShade: '#4a6230', moss: '#7f9a3e' },
  frost:   { hair: '#b8d8d0', hairShade: '#7ea49c', leaf2: '#e0f0ec', leaf3: '#6a8c8a', leaf4: '#9a9488', cloth: '#8aa8a0', clothShade: '#62807a', moss: '#9ab8a8' },
  turning: { hair: '#8aa03a', hairShade: '#5a6e24', leaf2: '#d8b440', leaf3: '#c8742a', leaf4: '#7a4a24', cloth: '#6a7a30', clothShade: '#4a5622', moss: '#7f9a3e' },
  withered:{ hair: '#8a6a3e', hairShade: '#5e4628', leaf2: '#a88a5a', leaf3: '#5a4a2a', leaf4: '#6e7a3a', cloth: '#6e5634', clothShade: '#4a3a22', moss: '#6a7040' },
};

/**
 * THE LEAF MIX. Which leaf colour a leaf cell takes: its own (main / shade)
 * or one of leaf2..4. A coarse value noise decides WHETHER (so the mix comes
 * in patches the leafPatch gene sizes, or speckle at 0), a hash over coarser
 * cells decides WHICH, weighted so the dry colour is the rarest. In SCENE
 * coordinates, so the painted cap on the skull and the mass above it agree
 * where they meet. Returns the slot unchanged for anything not a main leaf.
 */
export function leafVariant(g, ART, X, Y, Z, slot) {
  const s = g.sylvan;
  // The heaviest wisp (the broken outer surface of a leafy crown) mixes too,
  // or the whole OUTSIDE of a crown stays the main colour; it goes opaque,
  // which at 190/255 coverage is barely a change. The sheer tiers stay.
  if (!(s.leafMix > 0) || (slot !== ART.HAIR && slot !== ART.HAIR_SHADE &&
                           slot !== ART.HAIR_WISP1))
    return slot;
  const scale = 1 + s.leafPatch * 5;
  const n = s.leafPatch < 0.05 ? rnd(0x1eaf, X, Y, Z)
                               : vnoise(0x1eaf, X, Y, Z, scale);
  if (n >= s.leafMix * 0.95) return slot;
  const q = Math.max(1, Math.round(scale * 1.5));
  const w = rnd(0x1eb0, Math.floor(X / q), Math.floor(Y / q), Math.floor(Z / q));
  return w < 0.45 ? SLOTS.LEAF2 : w < 0.8 ? SLOTS.LEAF3 : SLOTS.LEAF4;
}
/** Stock eye glows, the Characters page's swatch row and what a roll picks
 *  from. The swatch still takes any colour: the glow IS the eye colour. */
export const GLOWS = {
  green: '#b6ff6a', lime: '#e2ff5a', gold: '#ffe066', amber: '#ffc24a',
  ember: '#ff6a3a', red: '#ff3a3a', rose: '#ff7ac0', violet: '#c08aff',
  blue: '#6a9cff', cyan: '#6ae8ff', teal: '#4affc4', white: '#f4ffd8',
  moon: '#cfe4ff',
};
export const FLOWERS = {
  white:  { flower: '#f2eee2', flowerEye: '#e8c34a' },
  pink:   { flower: '#f08ab8', flowerEye: '#f4e070' },
  yellow: { flower: '#f2d440', flowerEye: '#b8642a' },
  blue:   { flower: '#7aa0f0', flowerEye: '#f4f0c0' },
  red:    { flower: '#d83a3a', flowerEye: '#2a1a10' },
  violet: { flower: '#a878e0', flowerEye: '#f4d860' },
  orange: { flower: '#f09040', flowerEye: '#6a2a10' },
};
/** Leaf colours for the Characters page's stock stepper (the hair stepper,
 *  on a sylvan), darkest to lightest. */
export const LEAF_STOCK = [
  ['deep forest', { hair: '#24521f', hairShade: '#153312' }],
  ['forest',      LEAVES.forest],
  ['moss',        { hair: '#5c7a36', hairShade: '#3c5222' }],
  ['spring',      LEAVES.spring],
  ['lime',        { hair: '#9ccc4a', hairShade: '#68922a' }],
  ['silver',      LEAVES.silver],
  ['frost',       LEAVES.frost],
  ['gold',        LEAVES.gold],
  ['autumn',      LEAVES.autumn],
  ['rust',        { hair: '#9a4a1e', hairShade: '#663012' }],
  ['crimson',     LEAVES.crimson],
  ['blossom',     LEAVES.blossom],
  ['plum',        { hair: '#6a3a5e', hairShade: '#44223c' }],
];

/** NOT the limb girths: past 1.0 they widen an arm's BOX (mobgen armSection),
 *  and the box is what armour is fit to -- a sylvan must have exactly the
 *  boxes of the human it was switched from (test_mobgen section O).
 *
 *  A sylvan is SLIGHTER than the human it is built on — the reference is a
 *  sapling given a body — as OFFSETS, the way mobgen's SEX_FEMALE_DELTA is,
 *  so a switch back to human undoes exactly this and keeps your own work. */
export const BUILD_DELTA = {
  'shape.waist': -0.14,
  'shape.chest': -0.07,
  'shape.chestDepth': -0.06,
  'shape.seat': -0.06,
  'head.jaw': -0.06,
  'head.crown': 0.05,
};

/** FACE PRESETS over the face genes (a preset is data, never a code path, the
 *  hairstyle contract). Every preset states every face gene. */
const FACE_KEYS = ['sockets', 'eyeSize', 'brow', 'mouthHole', 'snout', 'warp', 'veil'];
export const FACES = {
  hollow:  { sockets: 0.8, eyeSize: 0.6, brow: 0.6, mouthHole: 0.35, snout: 0,   warp: 0.35, veil: 0 },
  mask:    { sockets: 0.35, eyeSize: 1.8, brow: 0.1, mouthHole: 0.9, snout: 0.8, warp: 0.1, veil: 0 },
  gnarled: { sockets: 0.65, eyeSize: 1, brow: 1, mouthHole: 0.5, snout: 0.15, warp: 0.85, veil: 0 },
  veiled:  { sockets: 0.5, eyeSize: 1, brow: 0.3, mouthHole: 0.2, snout: 0,   warp: 0.25, veil: 0.8 },
  serene:  { sockets: 0.25, eyeSize: 0.8, brow: 0.15, mouthHole: 0,  snout: 0,   warp: 0.1, veil: 0 },
  howler:  { sockets: 0.9, eyeSize: 1.4, brow: 0.7, mouthHole: 1,  snout: 0.35, warp: 0.5, veil: 0 },
};
export const FACE_ORDER = Object.keys(FACES);

/** CROWN PRESETS: the crown genes plus the HAIR style they sit on (the hair
 *  mass is the leaf mass on a sylvan: hair slots are leaves). */
const CROWN_KEYS = ['wreath', 'leaves', 'leafSize', 'flowers', 'flowerSize',
                    'moss'];
export const CROWNS = {
  bare:    { hair: 'bald',     wreath: 0,    leaves: 0,    leafSize: 0.5, branches: 0,    branchLen: 0.4, branchFork: 0.5, flowers: 0,    flowerSize: 0,   moss: 0.1 },
  sprout:  { hair: 'cropped',  wreath: 0,    leaves: 0.2,  leafSize: 0.2, branches: 0.35, branchLen: 0.15, branchFork: 0.2, flowers: 0.1, flowerSize: 0,   moss: 0.1 },
  leafy:   { hair: 'crew', hairOver: { volume: 2 }, wreath: 0.3,  leaves: 0.75, leafSize: 0.55, branches: 0.15, branchLen: 0.3, branchFork: 0.4, flowers: 0.1, flowerSize: 0,   moss: 0.2 },
  blossom: { hair: 'swept',    wreath: 0.9,  leaves: 0.45, leafSize: 0.35, branches: 0,    branchLen: 0.3, branchFork: 0.3, flowers: 0.9, flowerSize: 0.6, moss: 0.1 },
  sparse:  { hair: 'cropped',  wreath: 0.15, leaves: 0.2,  leafSize: 0.25, flowers: 0,  flowerSize: 0,   moss: 0.25 },
  willow:  { hair: 'flowing',  wreath: 0.4,  leaves: 0.3,  leafSize: 0.3, branches: 0,    branchLen: 0.3, branchFork: 0.3, flowers: 0.2, flowerSize: 0,   moss: 0.2 },
  thicket: { hair: 'wild',     wreath: 0.2,  leaves: 0.85, leafSize: 0.7, branches: 0.6,  branchLen: 0.6, branchFork: 0.6, flowers: 0.35, flowerSize: 0.3, moss: 0.35 },
  mossy:   { hair: 'mane',     wreath: 0,    leaves: 0.25, leafSize: 0.4, branches: 0.1,  branchLen: 0.25, branchFork: 0.2, flowers: 0.05, flowerSize: 0, moss: 0.85 },
};
export const CROWN_ORDER = Object.keys(CROWNS);

/** BRANCH PRESETS, independent of the crown: a crown is the leaf hair and
 *  what sits in it, these are the wood that grows out of the head, the
 *  shoulders and the back. Every style states every branch gene. */
const BRANCH_KEYS = ['branches', 'branchLen', 'branchFork', 'shoulderBranches',
                     'backBranches', 'boughLen', 'tufts'];
export const BRANCH_STYLES = {
  none:      { branches: 0,    branchLen: 0.4,  branchFork: 0.5, shoulderBranches: 0,   backBranches: 0,   boughLen: 0.5, tufts: 0.6 },
  sprigs:    { branches: 0.2,  branchLen: 0.4,  branchFork: 0.5, shoulderBranches: 0,   backBranches: 0,   boughLen: 0.5, tufts: 0.6 },
  antlers:   { branches: 0.75, branchLen: 0.95, branchFork: 0.85, shoulderBranches: 0,  backBranches: 0,   boughLen: 0.5, tufts: 0.25 },
  stag:      { branches: 0.35, branchLen: 1,    branchFork: 1,   shoulderBranches: 0,   backBranches: 0,   boughLen: 0.5, tufts: 0 },
  shoulders: { branches: 0,    branchLen: 0.4,  branchFork: 0.6, shoulderBranches: 0.7, backBranches: 0,   boughLen: 0.6, tufts: 0.7 },
  spines:    { branches: 0,    branchLen: 0.4,  branchFork: 0.2, shoulderBranches: 0.35, backBranches: 0.8, boughLen: 0.35, tufts: 0.2 },
  wildwood:  { branches: 0.6,  branchLen: 0.7,  branchFork: 0.7, shoulderBranches: 0.6, backBranches: 0.6, boughLen: 0.7, tufts: 0.6 },
};
export const BRANCH_ORDER = Object.keys(BRANCH_STYLES);

export function applyBranches(genome, style) {
  const b = BRANCH_STYLES[style];
  if (!b) return genome;
  genome.sylvan.branchStyle = style;
  for (const k of BRANCH_KEYS) genome.sylvan[k] = b[k];
  return genome;
}

export function applyFace(genome, style) {
  const f = FACES[style];
  if (!f) return genome;
  genome.sylvan.face = style;
  for (const k of FACE_KEYS) genome.sylvan[k] = f[k];
  return genome;
}

/** A crown sets its genes; the hairstyle underneath is applied by the caller
 *  (mobgen.applyHairStyle), which owns the hair genes. Returns the style. */
export function applyCrownGenes(genome, style) {
  const c = CROWNS[style];
  if (!c) return null;
  genome.sylvan.crown = style;
  for (const k of CROWN_KEYS) genome.sylvan[k] = c[k];
  return c.hair;
}

/** What a crown changes about the hairstyle under it, once that style is
 *  applied: leaves are never a neat helmet, so every leafy crown breaks its
 *  outer surface into see-through leaf edges and raggeds its ends. */
export function crownHairOverrides(style) {
  const c = CROWNS[style];
  if (!c || c.hair === 'bald') return {};
  return { fluff: 0.45, ragged: 0.7, hairlineSoft: 0.5, ...(c.hairOver || {}) };
}

/** THE GENE TABLE ROWS for the race (mobgen appends them to GENE_SPECS).
 *  `race: 'sylvan'` is what keeps them out of a human's mutation, roll and
 *  cross loops — without a random draw — and off a human's page. */
export const GENE_GROUPS = [
  { key: 'sylface', title: 'sylvan face', race: 'sylvan',
    note: 'The wooden face. The eyes glow from the bottom of their sockets ' +
          '(the glow is the eye colour); the brow, mouth and snout are carved ' +
          'or grown bark. Pick a face to set them all, then drag.' },
  { key: 'sylbark', title: 'bark and roots', race: 'sylvan',
    note: 'The body is bark over pale sapwood. Roots wind round every limb ' +
          'and stand proud of it -- the bark beside them is carved away, so ' +
          'the outline never grows and armour still fits.' },
  { key: 'sylcrown', title: 'crown', race: 'sylvan',
    note: 'What grows in the leaf hair: a wreath, clumps of leaves, flowers. ' +
          'Pick a crown to set them all, then drag. Branches are their own ' +
          'group, below.' },
  { key: 'sylbranch', title: 'branches and antlers', race: 'sylvan',
    note: 'Wood growing out of the head (antlers), the shoulders and the ' +
          'upper back, independent of the crown. The shoulder and back ' +
          'boughs ride the torso, so they stay put when the head turns.' },
];

export const GENE_SPECS = [
  { path: 'sylvan.face', label: 'face', group: 'sylface', kind: 'enum',
    choices: FACE_ORDER, sigma: 0.15, race: 'sylvan',
    hint: 'A starting face: hollow (deep sockets, gaunt), mask (round eyes and ' +
          'a pipe mouth), gnarled, veiled (leaves over the face), serene, howler.' },
  { path: 'sylvan.glow', label: 'glowing eyes', group: 'sylface', kind: 'bool',
    sigma: 0, race: 'sylvan',
    hint: 'The eyes are a glowing crystal and give light in the dark. Off, ' +
          'they are dark wood painted in the eye colour.' },
  { path: 'sylvan.sockets', label: 'eye depth', group: 'sylface', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How deep the eyes sit. At 0 they are on the surface; at 1 they ' +
          'glow from three cells down, under the brow.' },
  { path: 'sylvan.eyeSize', label: 'eye size', group: 'sylface', min: 0, max: 2,
    step: 0.05, sigma: 0.3, race: 'sylvan',
    hint: 'Pinpoints of light to wide round eyes.' },
  { path: 'sylvan.brow', label: 'brow', group: 'sylface', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'A dark ridge of bark over the eyes; high values stand it proud.' },
  { path: 'sylvan.mouthHole', label: 'mouth hollow', group: 'sylface', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'A carved, dark mouth: nothing, a slit, a wide hollow.' },
  { path: 'sylvan.snout', label: 'pipe mouth', group: 'sylface', min: 0, max: 1,
    step: 0.05, sigma: 0.15, race: 'sylvan', roll: 0.3,
    hint: 'The deku mouth: a round wooden tube standing off the face.' },
  { path: 'sylvan.warp', label: 'warp', group: 'sylface', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Dents and knobs over the skull, so the head reads as grown ' +
          'wood rather than a carved ball.' },
  { path: 'sylvan.veil', label: 'leaf veil', group: 'sylface', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan', roll: 0.3,
    hint: 'Leaves hanging over the face from the brow. They thin to ' +
          'see-through over the eyes, so the glow still shows.' },

  { path: 'sylvan.grain', label: 'grain', group: 'sylbark', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Light and dark streaks running along each limb.' },
  { path: 'sylvan.knots', label: 'knots', group: 'sylbark', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Dark knot rings in the bark.' },
  { path: 'sylvan.roots', label: 'roots', group: 'sylbark', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How many root strands wind round the body, and how thick.' },
  { path: 'sylvan.rootTwist', label: 'root twist', group: 'sylbark', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Straight up the limb at 0; a tight spiral at 1. Every other strand ' +
          'turns the other way, so they cross like woven roots.' },
  { path: 'sylvan.relief', label: 'root relief', group: 'sylbark', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How much of the bark beside each root is carved away, so the ' +
          'root stands proud. 0 is paint only.' },
  { path: 'sylvan.moss', label: 'moss', group: 'sylbark', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Moss on the shoulders, the crown and everything else that faces up.' },
  { path: 'sylvan.wrap', label: 'leaf wrap', group: 'sylbark', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'What covers the hips (and chest): bare bark at 0, a ragged ' +
          'hanging fringe of leaves in the middle, a whole wrap at 1.' },

  { path: 'sylvan.crown', label: 'crown', group: 'sylcrown', kind: 'enum',
    choices: CROWN_ORDER, sigma: 0.15, race: 'sylvan',
    hint: 'A starting crown: bare, sprout, leafy, blossom, sparse, willow, ' +
          'thicket, mossy. Sets the hairstyle under it too; leaves the ' +
          'branches alone.' },
  { path: 'sylvan.wreath', label: 'wreath', group: 'sylcrown', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'A circlet of leaves round the head at the brow; wider with more.' },
  { path: 'sylvan.leaves', label: 'leaf clumps', group: 'sylcrown', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How many clumps of leaves grow on the crown.' },
  { path: 'sylvan.leafSize', label: 'clump size', group: 'sylcrown', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Small tufts to big bushy clumps; also the tufts on branch tips.' },
  { path: 'sylvan.flowers', label: 'flowers', group: 'sylcrown', min: 0, max: 1,
    step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Flowers on the leaves, the wreath and the branch tips.' },
  { path: 'sylvan.flowerSize', label: 'flower size', group: 'sylcrown', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'A five-petal cross, or a full bloom past one half.' },
  { path: 'sylvan.leafMix', label: 'leaf mix', group: 'sylcrown', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How much of the foliage is the second, third and dry leaf colours ' +
          '(set in the colours below) instead of the main one. 0 is one ' +
          'green; 1 is mostly the others.' },
  { path: 'sylvan.leafPatch', label: 'leaf patches', group: 'sylcrown', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How the colours mix: leaf by leaf at 0, in big patches of one ' +
          'colour at 1.' },
  { path: 'sylvan.branchStyle', label: 'branches', group: 'sylbranch',
    kind: 'enum', choices: BRANCH_ORDER, sigma: 0.15, race: 'sylvan',
    hint: 'A starting set of branches: none, sprigs, antlers, stag, ' +
          'shoulders, spines (down the back), wildwood (everywhere).' },
  { path: 'sylvan.branches', label: 'head branches', group: 'sylbranch',
    min: 0, max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How many branches grow from the head. Long and forked, they are ' +
          'antlers.' },
  { path: 'sylvan.branchLen', label: 'head branch length', group: 'sylbranch',
    min: 0, max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'Stubs to long antlers.' },
  { path: 'sylvan.branchFork', label: 'forks', group: 'sylbranch', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How often a branch splits -- on the head and the body alike.' },
  { path: 'sylvan.shoulderBranches', label: 'shoulder boughs',
    group: 'sylbranch', min: 0, max: 1, step: 0.05, sigma: 0.2,
    race: 'sylvan', roll: 0.35,
    hint: 'Branches out of the top of each shoulder, up and outward.' },
  { path: 'sylvan.backBranches', label: 'back boughs', group: 'sylbranch',
    min: 0, max: 1, step: 0.05, sigma: 0.2, race: 'sylvan', roll: 0.35,
    hint: 'Branches out of the upper back, backward and up -- a row of ' +
          'spines at low length, a thicket at high.' },
  { path: 'sylvan.boughLen', label: 'bough length', group: 'sylbranch',
    min: 0, max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How long the shoulder and back boughs grow.' },
  { path: 'sylvan.tufts', label: 'leaf tufts', group: 'sylbranch', min: 0,
    max: 1, step: 0.05, sigma: 0.2, race: 'sylvan',
    hint: 'How many branch tips carry a tuft of leaves (clump size sets how ' +
          'big). 0 is bare wood.' },
];

/** Colour rows only a sylvan has, and the wording every shared row takes on a
 *  sylvan (the page shows these labels in place of "skin", "hair"...). */
export const COLOR_SPECS = [
  ['root', 'roots', 'The root strands, knots, eye sockets and the mouth\'s hollow.'],
  ['moss', 'moss', 'Moss on everything that faces up.'],
  ['branch', 'branches', 'Branches growing from the head.'],
  ['flower', 'flowers', 'The petals of every flower.'],
  ['flowerEye', 'flower centres', 'The middle of each flower.'],
  ['leaf2', 'leaves, second', 'A second leaf colour mixed through the foliage ' +
   '(fresh growth, say). How much is the leaf mix slider.'],
  ['leaf3', 'leaves, third', 'A third leaf colour (old, dark leaves).'],
  ['leaf4', 'leaves, dry', 'A fourth leaf colour -- by default the brown of ' +
   'dry, dead leaves.'],
];
export const COLOR_LABELS = {
  skin: ['bark', 'The base bark. Most of the body is this.'],
  skinShade: ['bark, shaded', 'Bark on faces turned away from the light, and ' +
              'the dark streaks of the grain.'],
  skinLight: ['bark, lit', 'The light streaks of the grain.'],
  hair: ['leaves', 'Every leaf: the hair, the clumps, the wreath, the veil.'],
  hairShade: ['leaves, shaded', 'Leaves on shaded faces.'],
  eye: ['eye glow', 'The eyes, and the colour of the light they give.'],
  cloth: ['leaf wrap', 'The wrap of leaves round the hips (and chest).'],
  clothShade: ['leaf wrap, shaded', 'The wrap where it turns from the light.'],
  nail: ['fingertips', 'Dark wood at the finger and toe tips.'],
};

// =============================================================================
// the surface: bark, grain, knots, moss, roots, and the face
//
// On the SHIPPED lattice, part-local ([x, y, z, slot], front is LOW y after
// mobgen's flipY, z up), after the upscale and the shoulder round. It may
// REMOVE cells and it may ADD cells inside the part's own box; it never
// grows the box, because the box is what armour is fit to.
// =============================================================================

const BARKISH = ART => new Set([ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT]);

/** Coarse value noise, 0..1, cell spacing `s`. */
function vnoise(seed, x, y, z, s) {
  const fx = x / s, fy = y / s, fz = z / s;
  const x0 = Math.floor(fx), y0 = Math.floor(fy), z0 = Math.floor(fz);
  const tx = fx - x0, ty = fy - y0, tz = fz - z0;
  const sm = t => t * t * (3 - 2 * t);
  const a = sm(tx), b = sm(ty), c = sm(tz);
  const v = (i, j, k) => rnd(seed, x0 + i, y0 + j, z0 + k);
  const l = (p, q, t) => p + (q - p) * t;
  return l(l(l(v(0, 0, 0), v(1, 0, 0), a), l(v(0, 1, 0), v(1, 1, 0), a), b),
           l(l(v(0, 0, 1), v(1, 0, 1), a), l(v(0, 1, 1), v(1, 1, 1), a), b), c);
}

/**
 * @param g      normalised genome (race sylvan)
 * @param role   'trunk' | 'head' | 'limb' | 'hand' | 'foot'
 * @param name   the part's name: a hash seed only, never compared
 * @param cells  [[x,y,z,slot]] shipped, part-local
 * @param size   the part's shipped box
 * @param info   { ART, eyeRow (head: shipped local eye row), mouthRow,
 *                 mouthDx } — the head also gets `info.face` back
 * @returns new cell list
 */
export function barkPass(g, role, name, cells, size, info) {
  const s = g.sylvan, A = info.ART;
  const [sx, sy, sz] = size;
  const seed = strHash(name) ^ 0x51f15;
  const idx = (x, y, z) => (z * sy + y) * sx + x;
  const inBox = (x, y, z) => x >= 0 && y >= 0 && z >= 0 &&
                             x < sx && y < sy && z < sz;
  const cellAt = new Map();
  for (const [x, y, z, c] of cells) cellAt.set(idx(x, y, z), [x, y, z, c]);
  const has = (x, y, z) => inBox(x, y, z) && cellAt.has(idx(x, y, z));
  const get = (x, y, z) => cellAt.get(idx(x, y, z));
  const paint = (x, y, z, c) => { const v = get(x, y, z); if (v) v[3] = c; };
  const del = (x, y, z) => cellAt.delete(idx(x, y, z));
  const put = (x, y, z, c) => {
    if (!inBox(x, y, z) || has(x, y, z)) return false;
    cellAt.set(idx(x, y, z), [x, y, z, c]);
    return true;
  };
  const BARK = BARKISH(A);
  const lateral = (x, y, z) => !has(x - 1, y, z) || !has(x + 1, y, z) ||
                               !has(x, y - 1, z) || !has(x, y + 1, z);
  const cx = sx / 2, cy = sy / 2;

  // ---- the face first (head): it decides what the rest must leave alone ----
  let face = null;
  if (role === 'head') face = carveFace(g, A, cells, size, info, { has, get,
    paint, del, put, inBox, seed });
  const nearFace = (x, y, z) => {
    if (!face) return false;
    if (y + 0.5 > cy) return false;                     // the back of the head
    for (const e of face.eyes)
      if ((x + 0.5 - e.x) ** 2 + (z + 0.5 - e.z) ** 2 < (e.rs + 2.5) ** 2)
        return true;
    const m = face.mouth;
    if (m && (x + 0.5 - m.x) ** 2 / 2 + (z + 0.5 - m.z) ** 2 < (m.r + 2) ** 2)
      return true;
    return z + 0.5 < face.browZ + 2 && z + 0.5 > face.chinZ &&
           Math.abs(x + 0.5 - cx) < face.halfW;
  };

  const list = () => [...cellAt.values()];

  // ---- warp: dents and knobs on the skull ----------------------------------
  if (role === 'head' && s.warp > 0) {
    const neck = info.neckTop || 0;
    const dent = [], knob = [];
    for (const [x, y, z, c] of list()) {
      if (z <= neck + 2 || nearFace(x, y, z)) continue;
      if (!lateral(x, y, z) && has(x, y, z + 1)) continue;
      const n = vnoise(seed ^ 0x77, x, y, z, 4.5);
      if (n < s.warp * 0.42) dent.push([x, y, z]);
      else if (n > 1 - s.warp * 0.3) knob.push([x, y, z, c]);
    }
    for (const [x, y, z] of dent) del(x, y, z);
    for (const [x, y, z, c] of knob) {
      const ox = x + 0.5 - cx, oy = y + 0.5 - cy;
      const dx = Math.abs(ox) > Math.abs(oy) ? Math.sign(ox) : 0;
      const dy = dx ? 0 : Math.sign(oy) || 1;
      put(x + dx, y + dy, z, c === A.HAIR || c === A.HAIR_SHADE ? c : A.SKIN_SHADE);
    }
  }

  // ---- roots: helical strands, with the bark beside them carved away --------
  const nR = s.roots > 0.02
    ? Math.max(1, Math.round(s.roots * (role === 'trunk' ? 6 : role === 'head'
                                        ? 3 : role === 'hand' ? 1.5 : 4)))
    : 0;
  if (nR) {
    const perim = Math.PI * (sx + sy) / 2;
    const w = 0.55 + s.roots * 0.85;                   // half-width, cells
    const twist = (0.1 + s.rootTwist * 1.5) / Math.max(sz, 12);
    const phase0 = rnd(seed, 3);
    const tOf = (x, y) => (Math.atan2(y + 0.5 - cy, x + 0.5 - cx) /
                           (2 * Math.PI) + 1) % 1;
    const distTo = (t, z) => {
      let best = 9;
      for (let k = 0; k < nR; k++) {
        const dir = k % 2 ? -1 : 1;
        const ph = (phase0 + k / nR + dir * twist * z +
                    0.035 * Math.sin(z * 0.3 + k * 1.7 + phase0 * 6)) % 1;
        let d = Math.abs(t - ((ph + 1) % 1));
        d = Math.min(d, 1 - d) * perim;
        if (d < best) best = d;
      }
      return best;
    };
    const groove = [];
    const carve = role !== 'hand' && role !== 'foot' && s.relief > 0;
    for (const [x, y, z, c] of list()) {
      if (!BARK.has(c) || !lateral(x, y, z)) continue;
      if (role === 'head' && (y + 0.5 < cy + 1 || z <= (info.neckTop || 0)))
        continue;                                      // roots stay off the face
      const d = distTo(tOf(x, y), z);
      if (d < w) paint(x, y, z, SLOTS.ROOT);
      else if (carve && d < w + 1.3 && rnd(seed, x, y, z, 5) < s.relief * 1.15)
        groove.push([x, y, z]);
    }
    // A groove cell goes only if the cell under it (toward the axis) stays:
    // relief is ONE cell deep, never a hole.
    for (const [x, y, z] of groove) {
      const ox = x + 0.5 - cx, oy = y + 0.5 - cy;
      const ix = Math.abs(ox) >= Math.abs(oy) ? x - Math.sign(ox) : x;
      const iy = Math.abs(ox) >= Math.abs(oy) ? y : y - Math.sign(oy);
      if (has(ix, iy, z)) del(x, y, z);
    }
    // The new floor of each groove is in shadow.
    for (const [x, y, z] of groove) {
      const ox = x + 0.5 - cx, oy = y + 0.5 - cy;
      const ix = Math.abs(ox) >= Math.abs(oy) ? x - Math.sign(ox) : x;
      const iy = Math.abs(ox) >= Math.abs(oy) ? y : y - Math.sign(oy);
      const v = get(ix, iy, z);
      if (v && BARK.has(v[3])) v[3] = A.SKIN_SHADE;
    }
  }

  // ---- grain: streaks along the limb -----------------------------------------
  if (s.grain > 0) {
    const perim = Math.PI * (sx + sy) / 2;
    for (const v of list()) {
      const [x, y, z, c] = v;
      if (!BARK.has(c) || !lateral(x, y, z) || nearFace(x, y, z)) continue;
      const col = Math.round(((Math.atan2(y + 0.5 - cy, x + 0.5 - cx) /
                               (2 * Math.PI) + 1) % 1) * perim);
      const run = 5 + Math.floor(rnd(seed, col, 5) * 7);
      const seg = Math.floor((z + rnd(seed, col, 3) * 9) / run);
      const r = rnd(seed, col, seg, 7);
      if (r < s.grain * 0.3) v[3] = A.SKIN_SHADE;
      else if (r > 1 - s.grain * 0.14 && c !== A.SKIN_SHADE) v[3] = A.SKIN_LIGHT;
    }
  }

  // ---- knots -----------------------------------------------------------------
  if (s.knots > 0 && role !== 'hand') {
    const surf = list().filter(([x, y, z, c]) => BARK.has(c) && lateral(x, y, z) &&
                                                !nearFace(x, y, z));
    const n = Math.floor(s.knots * surf.length / 420 + rnd(seed, 17));
    for (let i = 0; i < n && surf.length; i++) {
      const [kx, ky, kz] = surf[Math.floor(rnd(seed, i, 19) * surf.length)];
      const r = 1.4 + rnd(seed, i, 23) * 1.4 * (0.5 + s.knots);
      for (const v of list()) {
        const d = Math.hypot(v[0] - kx, v[1] - ky, v[2] - kz);
        if (d > r + 0.2 || !lateral(v[0], v[1], v[2])) continue;
        if (!BARK.has(v[3]) && v[3] !== SLOTS.ROOT) continue;
        v[3] = d < r * 0.45 ? SLOTS.ROOT : d < r * 0.8 ? A.SKIN_SHADE : SLOTS.ROOT;
      }
    }
  }

  // ---- moss: on whatever faces up --------------------------------------------
  if (s.moss > 0) {
    for (const v of list()) {
      const [x, y, z, c] = v;
      if (!BARK.has(c) && c !== SLOTS.ROOT) continue;
      if (has(x, y, z + 1)) continue;
      // A limb's own top row is a joint face under its parent; the trunk's
      // and the head's are shoulders and crown, which are exactly where
      // moss grows.
      if (z === sz - 1 && role !== 'trunk' && role !== 'head') continue;
      const blob = vnoise(seed ^ 0x3131, x, y, z, 3.5);
      if (blob < s.moss * 1.05 && rnd(seed, x, y, z, 29) < 0.35 + s.moss * 0.6)
        v[3] = SLOTS.MOSS;
    }
  }

  // ---- the leaf wrap: whole, ragged, or gone -----------------------------------
  // The human's shorts (and a woman's top) are CLOTH paint; on a sylvan they
  // are leaves. Below 1 they fray from the edges in: a ragged fringe that
  // hangs longer in places, then patches, then bare bark.
  if (s.wrap < 1) {
    for (const v of list()) {
      const [x, y, z, c] = v;
      if (c !== A.CLOTH && c !== A.CLOTH_SHADE) continue;
      const n = vnoise(seed ^ 0x9a9a, x * 2.2, y, z, 3);
      if (n > s.wrap * 1.2 - 0.1)
        v[3] = y + 0.5 < cy - 1.8 ? A.SKIN_BASE : A.SKIN_SHADE;
    }
  }

  // ---- the leaf mix on the painted cap (scene coordinates, so it lines up
  //      with the hair mass above it: crownExtras applies the same function)
  if (info.mn)
    for (const v of list())
      v[3] = leafVariant(g, A, v[0] + info.mn[0], v[1] + info.mn[1],
                         v[2] + info.mn[2], v[3]);

  // ---- fingertips and toes ----------------------------------------------------
  if (role === 'hand')
    for (const v of list()) if (v[2] <= 1 && BARK.has(v[3])) v[3] = A.NAIL;

  // ---- ONE PIECE: a carve can orphan a cell (a knob off a dent, a nose tip);
  //      the engine splits a disconnected limb on its first carve, so anything
  //      not joined to the largest piece goes. ------------------------------
  const out = keepLargest(list(), idx, sx, sy, sz);
  if (face) info.face = face;
  return out;
}

function keepLargest(cells, idx, sx, sy, sz) {
  const at = new Map(cells.map((c, i) => [idx(c[0], c[1], c[2]), i]));
  const comp = new Int32Array(cells.length).fill(-1);
  let best = -1, bestN = 0, nComp = 0;
  const sizes = [];
  for (let i = 0; i < cells.length; i++) {
    if (comp[i] >= 0) continue;
    const q = [i]; comp[i] = nComp;
    for (let qi = 0; qi < q.length; qi++) {
      const [x, y, z] = cells[q[qi]];
      for (const [dx, dy, dz] of D6) {
        const X = x + dx, Y = y + dy, Z = z + dz;
        if (X < 0 || Y < 0 || Z < 0 || X >= sx || Y >= sy || Z >= sz) continue;
        const j = at.get(idx(X, Y, Z));
        if (j === undefined || comp[j] >= 0) continue;
        comp[j] = nComp; q.push(j);
      }
    }
    sizes.push(q.length);
    if (q.length > bestN) { bestN = q.length; best = nComp; }
    nComp++;
  }
  return nComp <= 1 ? cells : cells.filter((_, i) => comp[i] === best);
}
const D6 = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];

/** The wooden face: sockets with the glow at the bottom, a brow, a mouth.
 *  Returns what the crown needs to know (eyes, mouth, brow row), part-local. */
function carveFace(g, A, cells, size, info, op) {
  const s = g.sylvan;
  const [sx, sy, sz] = size;
  const cx = sx / 2;
  const eyeCells = cells.filter(c => c[3] === A.EYE);
  // The FRONT of every (x, z) column: the lowest y there. Measured before any
  // carving, so a socket and the mouth are cut from the same face.
  const front = new Map();
  for (const [x, y, z] of cells) {
    const k = x * 4096 + z;
    if (!front.has(k) || y < front.get(k)) front.set(k, y);
  }
  const fy = (x, z) => front.get(x * 4096 + z);
  const R = 1.0 + s.eyeSize * 0.85;                   // the glow's radius
  const Rs = R + 0.7 + s.sockets * 0.5;               // the socket's
  const D = Math.round(s.sockets * 3);
  const groups = [eyeCells.filter(c => c[0] + 0.5 < cx),
                  eyeCells.filter(c => c[0] + 0.5 >= cx)];
  const eyes = [];
  for (const grp of groups) {
    if (!grp.length) continue;
    const ex = grp.reduce((a, c) => a + c[0] + 0.5, 0) / grp.length;
    const ez = grp.reduce((a, c) => a + c[2] + 0.5, 0) / grp.length;
    eyes.push({ x: ex, z: ez, r: R, rs: Rs,
                y: Math.min(...grp.map(c => c[1])) });
  }
  // The painted pupils go; the socket repaints what is left of them.
  for (const [x, y, z] of eyeCells) op.paint(x, y, z, A.SKIN_SHADE);

  const hollow = (px, pz, rx, rz, depth, inner, floorSlot, rimSlot, rimW) => {
    for (let z = Math.floor(pz - rz - rimW - 1); z <= pz + rz + rimW + 1; z++)
      for (let x = Math.floor(px - rx - rimW - 1); x <= px + rx + rimW + 1; x++) {
        const f = fy(x, z);
        if (f === undefined) continue;
        const d = Math.sqrt(((x + 0.5 - px) / rx) ** 2 + ((z + 0.5 - pz) / rz) ** 2);
        if (d <= 1) {
          for (let k = 0; k < depth; k++) op.del(x, f + k, z);
          const bottom = f + depth;
          if (op.has(x, bottom, z))
            op.paint(x, bottom, z, d <= inner ? floorSlot : A.SKIN_SHADE);
          // The socket's own walls are in shadow.
          for (let k = 0; k < depth; k++)
            for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
              const v = op.get(x + dx, f + k, z + dz);
              if (v) v[3] = SLOTS.ROOT;
            }
        } else if (d <= 1 + rimW / Math.max(rx, rz)) {
          if (op.has(x, f, z)) op.paint(x, f, z, rimSlot);
        }
      }
  };
  for (const e of eyes)
    hollow(e.x, e.z, e.rs, e.rs, D, R / e.rs, A.EYE, SLOTS.ROOT, 0.9);

  // THE BROW: rows over the sockets, dark, standing one cell proud where
  // the box has the room (it usually does: the skull sits set back).
  const top = eyes.length ? Math.max(...eyes.map(e => e.z + e.rs)) : sz * 0.55;
  const browZ = top + 0.5;
  if (eyes.length && s.brow > 0.05) {
    const x0 = Math.floor(Math.min(...eyes.map(e => e.x - e.rs)) - 1);
    const x1 = Math.ceil(Math.max(...eyes.map(e => e.x + e.rs)) + 1);
    const rows = 1 + Math.round(s.brow * 1.6);
    for (let z = Math.floor(browZ); z < Math.floor(browZ) + rows; z++)
      for (let x = x0; x <= x1; x++) {
        const f = fy(x, z);
        if (f === undefined) continue;
        if (op.has(x, f, z)) op.paint(x, f, z, SLOTS.ROOT);
        if (s.brow > 0.45 && z === Math.floor(browZ) && x > x0 && x < x1)
          op.put(x, f - 1, z, SLOTS.ROOT);
      }
  }

  // THE MOUTH, from the human face's own mouth row.
  let mouth = null;
  const mz = info.mouthRow + 0.5, mx = cx + (info.mouthDx || 0);
  if (s.mouthHole > 0.05 || s.snout > 0.02) {
    const r = 0.7 + s.mouthHole * 1.4 + s.snout * 0.6;
    const rx = r * (s.snout > 0.3 ? 1 : 1.35);
    if (s.mouthHole > 0.05)
      hollow(mx, mz, rx, r, 1 + Math.round(s.mouthHole * 1.5), 1.01,
             SLOTS.ROOT, A.SKIN_SHADE, 0.8);
    let f = Infinity;
    for (let z = Math.floor(mz - r - 1); z <= mz + r + 1; z++)
      for (let x = Math.floor(mx - rx - 1); x <= mx + rx + 1; x++)
        if (fy(x, z) !== undefined) f = Math.min(f, fy(x, z));
    mouth = { x: mx, z: mz, r, rx, y: Number.isFinite(f) ? f : 0 };
  }
  const chinZ = (info.neckTop || 0) + 1;
  const halfW = eyes.length
    ? Math.max(...eyes.map(e => Math.abs(e.x - cx) + e.rs)) + 1 : sx * 0.3;
  return { eyes, mouth, browZ, chinZ, halfW, frontOf: fy };
}

// =============================================================================
// the crown: wreath, leaf clumps, branches, flowers, veil, snout
//
// Called from inside mobgen's hairMass, after the hair mass is built and
// thinned and before it is split into parts, on the SHIPPED lattice in SCENE
// coordinates. `ctx.add(X,Y,Z,slot)` refuses body cells and cells already
// taken. Everything added rides the head (it is all above the neck), and the
// snout is returned separately: it is its own small part, so the size filter
// that drops hair specks never drops it.
// =============================================================================

export function crownWants(g) {
  const s = g.sylvan;
  return s.wreath > 0.02 || s.leaves > 0.02 || s.branches > 0.02 ||
         s.flowers > 0.02 || s.veil > 0.02 || s.snout > 0.02 ||
         s.shoulderBranches > 0.02 || s.backBranches > 0.02;
}

export function crownExtras(g, ctx) {
  const s = g.sylvan, A = ctx.ART;
  const { K, add, C, halfW, halfD, skullH, head, mass, body } = ctx;
  const seed = 0x5117a ^ Math.round(s.leaves * 97 + s.branches * 131 +
                                    s.flowers * 173 + s.leafSize * 7 +
                                    s.branchLen * 11);
  const face = ctx.face;
  const headCells = head.cells.map(([x, y, z]) =>
    [head.mn[0] + x, head.mn[1] + y, head.mn[2] + z]);
  const headSet = new Set(headCells.map(([x, y, z]) => K(x, y, z)));
  // BOUGHS (shoulder and back branches) are collected apart from the head's
  // mass: they ride the TORSO, and grow up through the neck line that splits
  // hair from mane, so they cannot go in `mass`. `emit` is where the branch
  // and tuft builders write: the mass for the head, the boughs for the body.
  const bough = new Map();
  // A bough grows THROUGH hanging leaf hair (a mane lies over the very
  // shoulders and back it roots on): in bough mode a mass cell is passable,
  // and the bough takes it over.
  let throughMass = false;
  const free = (X, Y, Z) => !body.has(K(X, Y, Z)) &&
                            (throughMass || !mass.has(K(X, Y, Z))) &&
                            !bough.has(K(X, Y, Z));
  const addBough = (X, Y, Z, slot) => {
    const k = K(X, Y, Z);
    if (!free(X, Y, Z)) return false;
    mass.delete(k);
    bough.set(k, [X, Y, Z, slot]);
    return true;
  };
  let emit = add;
  const leaf = (Y, X, Z, n) => (Y > C[1] + 1 || rnd(seed, X, Y, Z, n) < 0.3
    ? A.HAIR_SHADE : A.HAIR);
  // The face was measured head-local; everything here is scene.
  const browZ = face ? face.browZ + head.mn[2] : C[2] + skullH * 0.1;
  const frontLow = (X, Y, Z) => Y < C[1] - halfD * 0.3 && Z < browZ + 1;
  const up = [0, 0, 1];
  const norm = v => { const n = Math.hypot(...v) || 1; return v.map(a => a / n); };
  // A 6-connected line: a diagonal step is broken into axis steps, so a thin
  // strand is ONE piece the way the engine counts pieces.
  const line = (a, b, slot) => {
    let [x, y, z] = a;
    const tgt = b;
    let guard = 0;
    while ((x !== tgt[0] || y !== tgt[1] || z !== tgt[2]) && guard++ < 8) {
      if (x !== tgt[0]) x += Math.sign(tgt[0] - x);
      else if (y !== tgt[1]) y += Math.sign(tgt[1] - y);
      else z += Math.sign(tgt[2] - z);
      if (free(x, y, z)) emit(x, y, z, slot);
    }
  };
  // A WHOLE CLUMP in one of the other leaf colours, as often as the mix says:
  // a brown clump among green ones reads as a dying branch, which patches of
  // noise alone never do.
  const clumpSlot = n => {
    if (!(s.leafMix > 0) || rnd(seed, n, 331) >= s.leafMix * 0.6) return null;
    const w = rnd(seed, n, 337);
    return w < 0.45 ? SLOTS.LEAF2 : w < 0.8 ? SLOTS.LEAF3 : SLOTS.LEAF4;
  };
  const blob = (P, rx, rz, n, edge = 0.35) => {
    const whole = clumpSlot(n);
    for (let z = Math.floor(P[2] - rz - 1); z <= P[2] + rz + 1; z++)
      for (let y = Math.floor(P[1] - rx - 1); y <= P[1] + rx + 1; y++)
        for (let x = Math.floor(P[0] - rx - 1); x <= P[0] + rx + 1; x++) {
          const d = Math.sqrt(((x + 0.5 - P[0]) / rx) ** 2 +
                              ((y + 0.5 - P[1]) / rx) ** 2 +
                              ((z + 0.5 - P[2]) / rz) ** 2);
          if (d > 1 || !free(x, y, z)) continue;
          // Ragged: the outer shell is a broken, partly see-through edge.
          if (d > 0.72 && rnd(seed, x, y, z, n, 3) < 0.3) continue;
          emit(x, y, z, d > 0.72 && rnd(seed, x, y, z, n) < edge
            ? A.HAIR_WISP1 : (whole || leaf(y, x, z, n)));
        }
  };
  // Where the skull surface is along a direction from its centre: an
  // ellipsoid estimate, refined by walking out until the head is left behind.
  const surfaceAlong = v => {
    let t = 0;
    for (; t < 60; t += 0.5) {
      const X = Math.floor(C[0] + v[0] * t), Y = Math.floor(C[1] + v[1] * t);
      const Z = Math.floor(C[2] + v[2] * t);
      if (!headSet.has(K(X, Y, Z)) && t > 2) break;
    }
    return t;
  };

  // ---- the cap: one layer over the upper skull, so everything rooted on the
  //      crown is ONE piece. Leaves if the crown is leafy, else a burl of bark.
  const any = s.leaves > 0.02 || s.branches > 0.02 || s.flowers > 0.02 ||
              s.wreath > 0.02;
  const capSlot = (X, Y, Z) => s.leaves > 0.15 || s.flowers > 0.3
    ? leaf(Y, X, Z, 1) : A.SKIN_SHADE;
  // Only where no hair mass already covers the crown: the mass IS a cap.
  let massAbove = 0;
  for (const v of mass.values()) if (v[2] > C[2]) massAbove++;
  if (any && massAbove < 40) {
    const zCap = C[2] + skullH * 0.02;
    for (const [X, Y, Z] of headCells) {
      if (Z < zCap) continue;
      // A ragged lower edge, not a helmet's rim.
      if (Z < zCap + 3 && rnd(seed, X, Y, Z, 91) < 0.55) continue;
      for (const [dx, dy, dz] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0],
                                  [0, 0, 1]]) {
        const x = X + dx, y = Y + dy, z = Z + dz;
        if (!free(x, y, z) || frontLow(x, y, z)) continue;
        // The cap faces AWAY from the head's centre.
        if ((x - C[0]) * dx + (y - C[1]) * dy + (z - C[2]) * dz <= 0) continue;
        add(x, y, z, capSlot(x, y, z));
      }
    }
  }

  // ---- the wreath: a band round the head at the brow --------------------------
  if (s.wreath > 0.02) {
    const z0 = Math.floor(browZ + 1), z1 = z0 + 1 + Math.round(s.wreath * 2.5);
    const thick = s.wreath > 0.55 ? 2 : 1;
    for (let pass = 0; pass < thick; pass++) {
      const ring = [];
      for (const [k, v] of mass) {
        if (v[2] < z0 || v[2] >= z1) continue;
        ring.push(v);
      }
      const src = pass === 0 ? headCells.filter(c => c[2] >= z0 && c[2] < z1)
                             : ring;
      for (const [X, Y, Z] of src)
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const x = X + dx, y = Y + dy;
          if (!free(x, y, Z)) continue;
          if ((x - C[0]) * dx + (y - C[1]) * dy <= 0) continue;
          const fl = s.flowers > 0.02 &&
                     rnd(seed, x, y, Z, 41) < s.flowers * 0.22;
          add(x, y, Z, fl ? (rnd(seed, x, y, Z, 43) < 0.3 ? SLOTS.FLOWER_EYE
                                                          : SLOTS.FLOWER)
                          : leaf(y, x, Z, 2));
        }
    }
  }

  // ---- branches: antler-like, forking, bending upward --------------------------
  const tips = [];
  const grow = (P, d, len, thick, depth, n) => {
    let p = P.slice(), prev = P.map(Math.floor);
    let dir = norm(d);
    // The root cell itself, or a bough started just off the shoulder never
    // touches the body it grows from.
    if (free(prev[0], prev[1], prev[2])) emit(prev[0], prev[1], prev[2], SLOTS.BRANCH);
    for (let t = 0; t < len; t++) {
      if (t % 3 === 2) {
        dir = norm([dir[0] + (rnd(seed, n, t, 1) - 0.5) * 0.5,
                    dir[1] + (rnd(seed, n, t, 2) - 0.5) * 0.5,
                    dir[2] + 0.12 + (rnd(seed, n, t, 3) - 0.5) * 0.3]);
      }
      p = [p[0] + dir[0], p[1] + dir[1], p[2] + dir[2]];
      const c = p.map(Math.floor);
      line(prev, c, SLOTS.BRANCH);
      if (free(c[0], c[1], c[2])) emit(c[0], c[1], c[2], SLOTS.BRANCH);
      if (thick && t < len * 0.4) {
        // A 2x2 core near the root: a limb, not a wire.
        for (const [a, b] of [[1, 0], [0, 1], [1, 1]]) {
          const x = c[0] + a, y = c[1] + (Math.abs(dir[2]) > 0.6 ? b : 0);
          const z = c[2] + (Math.abs(dir[2]) > 0.6 ? 0 : b);
          if (free(x, y, z)) emit(x, y, z, SLOTS.BRANCH);
        }
      }
      prev = c;
      if (depth < 2 && (t === Math.round(len * 0.42) || t === Math.round(len * 0.72)) &&
          rnd(seed, n, t, 9) < s.branchFork) {
        const side = rnd(seed, n, t, 11) < 0.5 ? -1 : 1;
        // Rotate about the vertical-ish axis perpendicular to dir.
        const perp = norm([-dir[1], dir[0], 0.0001]);
        const a = 0.55 + rnd(seed, n, t, 13) * 0.35;
        const nd = norm([dir[0] * Math.cos(a) + perp[0] * side * Math.sin(a),
                         dir[1] * Math.cos(a) + perp[1] * side * Math.sin(a),
                         dir[2] * Math.cos(a) + 0.35]);
        grow(p, nd, Math.max(3, Math.round((len - t) * 0.7)), false, depth + 1,
             n * 7 + t);
      }
    }
    tips.push(p.map(Math.floor));
  };
  const nb = Math.round(s.branches * 6);
  for (let i = 0; i < nb; i++) {
    let phi = 2 * Math.PI * (i + 0.5 + (rnd(seed, i, 51) - 0.5) * 0.5) / nb +
              rnd(seed, 53) * 6.283;
    let v = [Math.cos(phi), Math.sin(phi), 0];
    // Antlers grow from the sides and back; a branch aimed at the face is
    // turned aside.
    if (v[1] < -0.55) v = norm([Math.sign(v[0]) || 1, -0.2, 0]);
    const el = 0.65 + rnd(seed, i, 57) * 0.55;
    const dir = norm([v[0] * Math.cos(el), v[1] * Math.cos(el), Math.sin(el)]);
    const t0 = surfaceAlong(dir);
    const P = [C[0] + dir[0] * (t0 - 1.5), C[1] + dir[1] * (t0 - 1.5),
               C[2] + dir[2] * (t0 - 1.5)];
    const len = Math.round(6 + s.branchLen * 26 * (0.8 + rnd(seed, i, 59) * 0.4));
    grow(P, norm([dir[0], dir[1], dir[2] + 0.5]), len, s.branchLen > 0.3, 0, i + 1);
  }
  // Tufts on the tips.
  const tuft = (T, i) => {
    if (rnd(seed, i, 79) < s.tufts)
      blob(T, 1.1 + s.leafSize * 1.6, 0.9 + s.leafSize * 1.2, 100 + i);
  };
  tips.forEach(tuft);

  // ---- boughs: branches out of the shoulders and the upper back -------------
  // Rooted on the TORSO's own surface, grown with the same builder as the
  // head's branches, and written to `bough` (they become torso-riding parts).
  if ((s.shoulderBranches > 0.02 || s.backBranches > 0.02) && ctx.torso) {
    emit = addBough;
    throughMass = true;
    tips.length = 0;
    const T = ctx.torso;
    const tc = T.cells.map(([x, y, z]) => [T.mn[0] + x, T.mn[1] + y, T.mn[2] + z]);
    const tx0 = Math.min(...tc.map(c => c[0])), tx1 = Math.max(...tc.map(c => c[0]));
    const tz0 = Math.min(...tc.map(c => c[2])), tz1 = Math.max(...tc.map(c => c[2]));
    const ty0 = Math.min(...tc.map(c => c[1])), ty1 = Math.max(...tc.map(c => c[1]));
    const tcx = (tx0 + tx1 + 1) / 2, tcy = (ty0 + ty1 + 1) / 2;
    const halfTW = (tx1 - tx0 + 1) / 2;
    const bLen = s.boughLen;
    const pickSpaced = (cands, n, minD, salt) => {
      cands.sort((a, b) => rnd(seed, a[0], a[1], a[2], salt) -
                           rnd(seed, b[0], b[1], b[2], salt));
      const out = [];
      for (const c of cands) {
        if (out.length >= n) break;
        if (out.some(o => Math.abs(o[0] - c[0]) + Math.abs(o[1] - c[1]) +
                          Math.abs(o[2] - c[2]) < minD)) continue;
        out.push(c);
      }
      return out;
    };
    let n = 500;
    // Shoulders: the top rows, out toward each side, the up-facing surface.
    const perSide = Math.round(s.shoulderBranches * 3);
    for (const side of [-1, 1]) {
      if (!perSide) break;
      const cands = tc.filter(([X, Y, Z]) => Z >= tz1 - 3 &&
        side * (X + 0.5 - tcx) > halfTW * 0.6 && !body.has(K(X, Y, Z + 1)));
      for (const [X, Y, Z] of pickSpaced(cands, perSide, 4, 11 + side)) {
        // Out over the arm and up, like a pauldron grown wild.
        const d = norm([side * (0.9 + rnd(seed, X, Z, 3) * 0.6),
                        (rnd(seed, X, Z, 5) - 0.3) * 0.7, 0.85]);
        const len = Math.round(5 + bLen * 22 * (0.75 + rnd(seed, X, Z, 7) * 0.5));
        grow([X + 0.5, Y + 0.5, Z + 1.5], d, len, bLen > 0.3, 0, n++);
      }
    }
    // The upper back: the back-facing surface (+y is the back), upper half.
    const nBack = Math.round(s.backBranches * 6);
    if (nBack) {
      const cands = tc.filter(([X, Y, Z]) => Z > tz0 + (tz1 - tz0) * 0.4 &&
        !body.has(K(X, Y + 1, Z)) && Y + 0.5 > tcy);
      for (const [X, Y, Z] of pickSpaced(cands, nBack, 5, 23)) {
        const d = norm([(X + 0.5 - tcx) / Math.max(halfTW, 1) * 0.6,
                        1, 0.7 + rnd(seed, X, Z, 9) * 0.5]);
        const len = Math.round(4 + bLen * 20 * (0.75 + rnd(seed, X, Z, 13) * 0.5));
        grow([X + 0.5, Y + 1.5, Z + 0.5], d, len, bLen > 0.35, 0, n++);
      }
    }
    tips.forEach((t, i) => tuft(t, 700 + i));
    emit = add;
    throughMass = false;
  }

  // ---- leaf clumps ---------------------------------------------------------------
  const nl = Math.round(s.leaves * 8);
  for (let i = 0; i < nl; i++) {
    const phi = 2 * Math.PI * (i + rnd(seed, i, 61)) / Math.max(nl, 1) +
                rnd(seed, 67) * 6.283;
    const el = 0.35 + rnd(seed, i, 71) * 0.95;
    let v = [Math.cos(phi) * Math.cos(el), Math.sin(phi) * Math.cos(el), Math.sin(el)];
    if (v[1] < -0.5 && el < 0.9) v = norm([v[0], 0.1, v[2] + 0.4]);
    v = norm(v);
    const rc = 2 + s.leafSize * 3.2 + rnd(seed, i, 73) * 1.2;
    const t0 = surfaceAlong(v);
    const out = t0 + rc * 0.55 + (massAbove ? 1.5 : 0);
    blob([C[0] + v[0] * out, C[1] + v[1] * out, C[2] + v[2] * out],
         rc, rc * 0.8, 200 + i);
  }

  // ---- flowers: on the outside of whatever grew ---------------------------------
  const nf = Math.round(s.flowers * 12);
  if (nf > 0) {
    const cand = [];
    for (const v of mass.values()) {
      const [X, Y, Z, c] = v;
      if (Z < browZ - 1) continue;
      if (c !== A.HAIR && c !== A.HAIR_SHADE && c !== SLOTS.BRANCH) continue;
      const o = [X + 0.5 - C[0], Y + 0.5 - C[1], Z + 0.5 - C[2]];
      const ax = [0, 1, 2].reduce((b, i) => Math.abs(o[i]) > Math.abs(o[b]) ? i : b, 0);
      const n = [0, 0, 0]; n[ax] = Math.sign(o[ax]) || 1;
      if (!free(X + n[0], Y + n[1], Z + n[2])) continue;
      if (frontLow(X, Y, Z)) continue;
      cand.push([X, Y, Z, n, rnd(seed, X, Y, Z, 81)]);
    }
    cand.sort((a, b) => a[4] - b[4]);
    const placed = [];
    for (const [X, Y, Z, n] of cand) {
      if (placed.length >= nf) break;
      if (placed.some(p => Math.abs(p[0] - X) + Math.abs(p[1] - Y) +
                           Math.abs(p[2] - Z) < 5)) continue;
      const F = [X + n[0], Y + n[1], Z + n[2]];
      add(F[0], F[1], F[2], SLOTS.FLOWER_EYE);
      const ax = n.findIndex(a => a !== 0);
      const tang = [0, 1, 2].filter(i => i !== ax);
      const big = s.flowerSize > 0.5;
      const petals = big
        ? [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [-1, 1], [1, -1], [-1, -1],
           [2, 0], [-2, 0], [0, 2], [0, -2]]
        : [[1, 0], [-1, 0], [0, 1], [0, -1]];
      for (const [a, b] of petals) {
        const q = F.slice();
        q[tang[0]] += a; q[tang[1]] += b;
        if (free(q[0], q[1], q[2])) add(q[0], q[1], q[2], SLOTS.FLOWER);
      }
      placed.push(F);
    }
  }

  // ---- the veil: leaves hanging over the face -------------------------------------
  if (s.veil > 0.02 && face && face.eyes.length) {
    const hx = head.mn[0], hy = head.mn[1], hz = head.mn[2];
    const eyes = face.eyes.map(e => ({ x: e.x + hx, z: e.z + hz, r: e.r }));
    const frontAt = (X, Z) => {
      const f = face.frontOf(X - hx, Z - hz);
      return f === undefined ? undefined : f + hy;
    };
    const x0 = Math.floor(Math.min(...eyes.map(e => e.x)) - 5);
    const x1 = Math.ceil(Math.max(...eyes.map(e => e.x)) + 5);
    const zTop = Math.floor(face.browZ + hz + 1);
    const mouthZ = face.mouth ? face.mouth.z + hz : zTop - 10;
    const span = Math.max(3, Math.round(s.veil * (zTop - mouthZ + 3)));
    let lastY = null;
    for (let X = x0; X <= x1; X++) {
      const f = frontAt(X, zTop);
      if (f !== undefined) lastY = f - 1;
      // The band the strands hang from: every column, no gaps, so the veil is
      // one piece.
      for (let Y = (lastY ?? C[1] - halfD) ; Y >= (lastY ?? C[1] - halfD) - 1; Y--)
        if (free(X, Y, zTop)) { add(X, Y, zTop, leaf(Y, X, zTop, 5)); break; }
      if (rnd(seed, X, 401) > 0.3 + s.veil * 0.7) continue;
      const len = Math.round(span * (0.6 + rnd(seed, X, 403) * 0.5));
      let py = lastY;
      for (let d = 1; d <= len; d++) {
        const Z = zTop - d;
        const ff = frontAt(X, Z);
        const Y = ff !== undefined ? ff - 1 : py;
        if (Y === null || Y === undefined) break;
        if (!free(X, Y, Z)) { py = Y; continue; }
        if (py !== null && py !== Y) line([X, py, Z + 1], [X, Y, Z + 1], A.HAIR_WISP1);
        py = Y;
        const overEye = eyes.some(e => (X + 0.5 - e.x) ** 2 + (Z + 0.5 - e.z) ** 2 <
                                       (e.r + 1.6) ** 2);
        const f = d / Math.max(len, 1);
        const slot = overEye ? (f > 0.5 ? A.HAIR_WISP3 : A.HAIR_WISP2)
                   : f < 0.45 ? leaf(Y, X, Z, 6)
                   : f < 0.75 ? A.HAIR_WISP1 : A.HAIR_WISP2;
        add(X, Y, Z, slot);
      }
    }
  }

  // ---- the snout: the deku pipe mouth -----------------------------------------------
  const snout = [];
  if (s.snout > 0.02 && face && face.mouth) {
    const m = face.mouth;
    const X0 = m.x + head.mn[0], Z0 = m.z + head.mn[2];
    const yf = m.y + head.mn[1];
    const Ls = 1 + Math.round(s.snout * 3);
    const Ro = m.r + 1.3, Ri = Math.max(0.6, m.r - 0.1);
    const taken = new Set();
    for (let l = 1; l <= Ls; l++)
      for (let Z = Math.floor(Z0 - Ro - 1); Z <= Z0 + Ro + 1; Z++)
        for (let X = Math.floor(X0 - Ro - 1); X <= X0 + Ro + 1; X++) {
          const d = Math.hypot(X + 0.5 - X0, Z + 0.5 - Z0);
          if (d > Ro || d < Ri) continue;
          const Y = yf - l;
          const k = K(X, Y, Z);
          if (body.has(k) || mass.has(k) || taken.has(k)) continue;
          taken.add(k);
          snout.push([X, Y, Z, l === Ls ? SLOTS.ROOT
                                        : d > Ro - 0.9 ? A.SKIN_BASE : A.SKIN_SHADE]);
        }
  }
  // ---- the leaf mix, over every main-colour leaf the crown has ----------------
  // Last, so it reaches the hair mass, the cap, the wreath, the clumps, the
  // tufts and the veil alike. A clump already given a whole colour keeps it.
  for (const v of mass.values())
    v[3] = leafVariant(g, A, v[0], v[1], v[2], v[3]);
  for (const v of bough.values())
    v[3] = leafVariant(g, A, v[0], v[1], v[2], v[3]);
  return { snout, boughs: [...bough.values()] };
}
