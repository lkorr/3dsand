/* naga.js — THE NAGA RACE: a person above the hips, a serpent below.
 *
 * WHAT A NAGA IS, TO THE GENERATOR. The human's upper body -- the same head,
 * torso, arms and hands, so every helm, breastplate, bracer and blade fits and
 * every system that binds by limb name binds -- on the SERPENT lower-body plan
 * (serpent.js): no legs, a waist, a rise and a long tail lying behind, which
 * slithers on land and kicks like a mermaid's in water. `LOWER = 'serpent'` is
 * the whole of how the race asks for it (mobgen applyRace); the tail's own
 * genes and colours belong to the plan, not to the race, so another race could
 * wear the same tail.
 *
 * WHAT THE RACE ADDS is the look of the upper half: slit-pupilled eyes that may
 * give light, scales creeping up from the tail over the hips, the spine, the
 * flanks and the forearms, scaled cheekbones, fangs, facial markings, a
 * cobra's hood off the back of the neck, fin ears and a head crest for the
 * sea-born, and gold -- armbands, a collar, a circlet -- for the royal. A KIND
 * (cobra, viper, python, coral, sea, jungle, royal, abyssal) is a picker that
 * sets the tail and the hood together with the colours that go with them.
 *
 * IT IS FLESH, like the demon: the human's anatomy recipe (ANATOMY = null),
 * blood, hair. Only fangs (bone), gold (gold) and lit eyes (gem_arcane, the
 * art colour is the light's) name a material. Every gene here is race-gated:
 * no human roll, mutation or cross takes a draw for it. PURE AND
 * NODE-RUNNABLE; imports robotkit.js and serpent.js.
 */

'use strict';

import { rnd, strHash, clamp, grid, vnoise, keepLargest, sceneCells,
         extent } from './robotkit.js';
import * as SE from './serpent.js';

export const RACE = 'naga';
export const FOLDER = 'naga';
export const NEW_NAME = 'naga';
/** Not a machine (test_mobgen section P is the machines'). */
export const MACHINE = false;
/** The lower-body plan this race is born on (mobgen applyRace). */
export const LOWER = 'serpent';
export const BLURB = 'a naga: a person above the hips and a serpent below, ' +
  'slithering on land and swimming like a mermaid. Slit-eyed, scaled, ' +
  'fanged, sometimes hooded like a cobra or finned for the sea. The same ' +
  'arms, torso and head as a human, so every helm and blade fits.';

/** Upright height (crown above the ground, the tail lying behind). */
export const HEIGHT_BAND = [1.5, 2.4];

// =============================================================================
// art slots and materials
// =============================================================================

/** The naga's own slots, below the lips (217); the serpent's are 213..209. */
export const SLOTS = {
  PUPIL: 216,        // the slit
  FANG: 215,         // fangs
  GOLD: 214,         // armbands, collar, circlet
};
export const COLOR_SLOTS = { pupil: SLOTS.PUPIL, fang: SLOTS.FANG, gold: SLOTS.GOLD };
export const MATS = { fang: 'bone', gold: 'gold', glow: 'gem_arcane' };
/** null: every slot not named below keeps the human's material (skin). */
export const DEFAULT_MAT = null;
/** `gold` is material 128, past what a .vox can paint: it is painted as
 *  `iron` (nothing else on a naga is iron) and the sidecar's anatomy
 *  `becomes` turns every iron voxel back into gold at load (mob.cpp, after
 *  the recipe; mobgen anatomyRecipe adds the row). */
export const STAND_INS = { gold: 'iron' };
export const BLEED = { material: 'blood', perDamage: 2.5 };
export const ANATOMY = null;
export const ANATOMY_NOTES = {};

export function slotMaterialNames(g, ART) {
  const m = {};
  // The scales and fins are SKIN, the tail's and the creep up the body alike,
  // so they bleed, burn and cut like the rest of it. (A hood is hung as a
  // hair-mass part, whose own material is hair; its slots say skin here.)
  for (const sl of Object.values(SE.SLOTS)) m[sl] = 'skin';
  m[SLOTS.PUPIL] = 'skin';
  m[SLOTS.FANG] = MATS.fang;
  m[SLOTS.GOLD] = MATS.gold;
  if (g.naga && g.naga.glow) m[ART.EYE] = MATS.glow;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export const EYE_STYLES = ['slit', 'round', 'black'];
export const MARKINGS = ['none', 'tear', 'mask', 'stripe', 'chevron'];
export const HOOD_MARKS = ['none', 'spectacle', 'eye', 'bands'];
export const EARS = ['human', 'fins'];
export const REGALIA = ['none', 'bands', 'collar', 'circlet', 'royal'];

export function defaultGenes() {
  return {
    kind: 'cobra',       // a KIND (KINDS): sets the tail, hood and colours
    eyes: 'slit',
    glow: false,         // the eyes give light
    markings: 'none',
    scaleCreep: 0.5,     // scales up from the tail over the hips and spine
    faceScales: 0.35,    // scaled cheekbones and temples
    fangs: 0.5,
    venom: true,         // the bite envenoms
    hood: 0.6,           // a cobra's hood, its height; 0 is none
    hoodWidth: 0.6,
    hoodMark: 'spectacle',
    ears: 'human',
    crest: 0,            // a fin along the top of the skull
    regalia: 'none',
  };
}

/** THE KINDS: the tail, the hood and the scale colours, picked together.
 *  Every key of `serpent` and `naga` named is set; the rest are left. */
export const KINDS = {
  cobra: {
    serpent: { pattern: 'banded', tip: 'point', crest: 0, frills: 0,
               girth: 0.5, length: 1.7, swim: 'undulate', patternScale: 0.55 },
    naga: { hood: 0.7, hoodWidth: 0.65, hoodMark: 'spectacle', ears: 'human',
            crest: 0 },
    colors: { scale: '#3e3426', scaleShade: '#2a2318', pattern: '#c8a85a',
              belly: '#dccb8e', fin: '#6a5a3a' },
  },
  viper: {
    serpent: { pattern: 'diamond', tip: 'rattle', crest: 0, frills: 0,
               girth: 0.45, length: 1.8, swim: 'undulate', patternScale: 0.6 },
    naga: { hood: 0, ears: 'human', crest: 0 },
    colors: { scale: '#8e7c58', scaleShade: '#6a5a3e', pattern: '#3a2e1e',
              belly: '#dcd0ae', fin: '#7a6a4a' },
  },
  python: {
    serpent: { pattern: 'blotched', tip: 'point', crest: 0, frills: 0,
               girth: 0.85, length: 2.4, swim: 'undulate', patternScale: 0.7 },
    naga: { hood: 0, ears: 'human', crest: 0 },
    colors: { scale: '#7a6a3a', scaleShade: '#584a26', pattern: '#33261a',
              belly: '#e2d6b2', fin: '#6a5a30' },
  },
  coral: {
    serpent: { pattern: 'ringed', tip: 'point', crest: 0, frills: 0,
               girth: 0.35, length: 1.6, swim: 'undulate', patternScale: 0.45 },
    naga: { hood: 0, ears: 'human', crest: 0 },
    colors: { scale: '#c4322a', scaleShade: '#922218', pattern: '#1a1414',
              belly: '#e6c84a', fin: '#c4322a' },
  },
  sea: {
    serpent: { pattern: 'speckled', tip: 'fluke', crest: 0.5, frills: 0.5,
               girth: 0.55, length: 1.6, swim: 'kick', patternScale: 0.4,
               tipSize: 0.65 },
    naga: { hood: 0, ears: 'fins', crest: 0.4 },
    colors: { scale: '#2a6a7a', scaleShade: '#1a4a5a', pattern: '#8ad8d4',
              belly: '#d0e8e0', fin: '#5ab8c8' },
  },
  jungle: {
    serpent: { pattern: 'striped', tip: 'point', crest: 0, frills: 0,
               girth: 0.45, length: 2.0, swim: 'undulate', patternScale: 0.5 },
    naga: { hood: 0.3, hoodWidth: 0.4, hoodMark: 'none', ears: 'human',
            crest: 0 },
    colors: { scale: '#3a8a3a', scaleShade: '#286028', pattern: '#c8d84a',
              belly: '#dcecaa', fin: '#5aa848' },
  },
  royal: {
    serpent: { pattern: 'diamond', tip: 'spade', crest: 0.2, frills: 0,
               girth: 0.55, length: 2.0, swim: 'kick', patternScale: 0.5 },
    naga: { hood: 0.9, hoodWidth: 0.8, hoodMark: 'eye', ears: 'human',
            crest: 0 },
    colors: { scale: '#e6d8b4', scaleShade: '#bca878', pattern: '#c89a38',
              belly: '#f6eedc', fin: '#e0c070' },
  },
  abyssal: {
    serpent: { pattern: 'speckled', tip: 'fan', crest: 0.6, frills: 0.3,
               girth: 0.5, length: 2.1, swim: 'kick', patternScale: 0.35 },
    naga: { hood: 0, ears: 'fins', crest: 0.5, glow: true },
    colors: { scale: '#1c1c2c', scaleShade: '#121220', pattern: '#5a48b0',
              belly: '#3a3a4c', fin: '#6a4ac8' },
  },
};
export const KIND_ORDER = Object.keys(KINDS);

export function applyKind(genome, kind) {
  const k = KINDS[kind];
  if (!k) return genome;
  genome.naga.kind = kind;
  Object.assign(genome.serpent, k.serpent);
  Object.assign(genome.naga, k.naga);
  Object.assign(genome.colors, k.colors);
  return genome;
}

export const PICKERS = [
  { key: 'kind', path: 'naga.kind', label: 'kind', order: KIND_ORDER,
    apply: applyKind,
    title: 'a kind of serpent: sets the tail (pattern, tip, fins, girth, ' +
           'length, how it swims), the hood and the scale colours together' },
];

/** Skin a roll picks whole: the human's own tones and a few that are not. */
export const SKINS = {
  fair:   { skin: '#e2b48e', skinShade: '#b88664', skinLight: '#f2cfae', nail: '#d9ae90', lip: '#a8564a' },
  bronze: { skin: '#b07a4e', skinShade: '#86562f', skinLight: '#cf9a6c', nail: '#a87a58', lip: '#7a3a2a' },
  umber:  { skin: '#6e4a32', skinShade: '#4e321e', skinLight: '#8e664a', nail: '#6a4a38', lip: '#4a2418' },
  jade:   { skin: '#8aa880', skinShade: '#647e5a', skinLight: '#a8c49e', nail: '#5a7a50', lip: '#4a6a3e' },
  ashen:  { skin: '#9a9a94', skinShade: '#74746e', skinLight: '#b8b8b0', nail: '#6a6a64', lip: '#5a4a4a' },
  cobalt: { skin: '#6a84a8', skinShade: '#4a6080', skinLight: '#8aa2c4', nail: '#40587a', lip: '#3a3a6a' },
};
/** Irises: gold the most snake-like. Pupils always near-black. */
export const EYES = {
  gold:    { eye: '#e8b830', pupil: '#120c06' },
  amber:   { eye: '#d87a20', pupil: '#120c06' },
  emerald: { eye: '#3ac860', pupil: '#0a120a' },
  ice:     { eye: '#a8e0f0', pupil: '#0a1018' },
  blood:   { eye: '#e83a2a', pupil: '#140606' },
  violet:  { eye: '#a060e0', pupil: '#0e0818' },
};
export const METALS = {
  gold:   { gold: '#d8a838', fang: '#f0ead8' },
  bronze: { gold: '#b07838', fang: '#ece2c8' },
  silver: { gold: '#c8ccd0', fang: '#f4f0e6' },
};
export const DEFAULT_COLORS = {
  ...EYES.gold, ...METALS.gold,
};
export const GLOWS = Object.fromEntries(Object.entries(EYES).map(([k, v]) => [k, v.eye]));
export const COLOR_SETS = [SKINS, EYES, METALS];
function lum(hex) {
  const n = parseInt(hex.slice(1), 16);
  return ((n >> 16) & 255) * 0.3 + ((n >> 8) & 255) * 0.59 + (n & 255) * 0.11;
}
export const STOCK = Object.entries(SKINS).sort((a, b) => lum(a[1].skin) - lum(b[1].skin));
export const STOCK_LABEL = 'skin';

export const BUILD_DELTA = { 'shape.waist': -0.04 };

export function onBecome(genome, h) {
  if (!h.isLocked('face.beardStyle')) h.applyBeardStyle(genome, 'clean');
}

export function randomize(g, r, isLocked) {
  if (!isLocked('naga.kind')) applyKind(g, r.pick(KIND_ORDER));
  SE.randomize(g, r, isLocked);
  for (const spec of GENE_SPECS) {
    if (spec.kind || isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    const v = g.naga[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    g.naga[k] = clamp(v, spec.min, spec.max);
  }
  if (!isLocked('naga.eyes')) g.naga.eyes = r() < 0.7 ? 'slit' : r.pick(EYE_STYLES);
  if (!isLocked('naga.glow')) g.naga.glow = r() < 0.25;
  if (!isLocked('naga.markings')) g.naga.markings = r() < 0.5 ? 'none' : r.pick(MARKINGS);
  if (!isLocked('naga.regalia')) g.naga.regalia = r() < 0.6 ? 'none' : r.pick(REGALIA);
  if (!isLocked('naga.venom')) g.naga.venom = r() < 0.7;
  return g;
}

export const GENE_GROUPS = [
  { key: 'ngface', title: 'naga: eyes and face', race: 'naga',
    note: 'Eyes slit like a snake\'s (and lit, if you like), scaled ' +
          'cheekbones, fangs that envenom, and markings in the pattern ' +
          'colour. The face is otherwise the human one above.' },
  { key: 'ngbody', title: 'naga: hood, fins, scales, gold', race: 'naga',
    note: 'Pick a kind to set the tail, hood and scale colours together, ' +
          'then drag. Scales creep up from the tail over the hips, the ' +
          'spine and the forearms. The hood stands off the back of the neck.' },
];

const N = (path, label, group, hint, extra = {}) => ({
  path: 'naga.' + path, label, group, min: 0, max: 1, step: 0.05,
  sigma: 0.2, race: 'naga', hint, ...extra });
const E = (path, label, group, choices, hint) => ({
  path: 'naga.' + path, label, group, kind: 'enum', choices, sigma: 0.15,
  race: 'naga', hint });

export const GENE_SPECS = [
  E('kind', 'kind', 'ngbody', KIND_ORDER, 'A kind of serpent: cobra, viper ' +
    '(diamondback, rattle), python, coral (ringed), sea (fluke and fins, ' +
    'swims like a mermaid), jungle, royal, abyssal.'),
  E('eyes', 'eyes', 'ngface', EYE_STYLES, 'slit: a snake\'s pupil. round: ' +
    'an iris and a round pupil. black: all pupil, a beady snake eye.'),
  { path: 'naga.glow', label: 'lit eyes', group: 'ngface', kind: 'bool',
    sigma: 0, race: 'naga',
    hint: 'The irises give light in the eye colour.' },
  E('markings', 'markings', 'ngface', MARKINGS, 'In the pattern colour: a ' +
    'tear-line from each eye, a mask across them, a stripe up the brow, a ' +
    'chevron on the forehead.'),
  N('faceScales', 'face scales', 'ngface', 'Scales over the cheekbones and ' +
    'temples. 0 is none.'),
  N('fangs', 'fangs', 'ngface', 'How long the fangs under the upper lip are. ' +
    '0 is none; long fangs bite harder.'),
  { path: 'naga.venom', label: 'venom', group: 'ngface', kind: 'bool',
    sigma: 0, race: 'naga',
    hint: 'The bite envenoms the wound, as the snake\'s does.' },
  N('scaleCreep', 'scale creep', 'ngbody', 'How far the scales climb from ' +
    'the tail: just the hips at 0, up the spine, flanks and forearms at 1.'),
  N('hood', 'hood', 'ngbody', 'A cobra\'s hood off the back of the neck, its ' +
    'height. 0 is none.'),
  N('hoodWidth', 'hood width', 'ngbody', 'How far the hood spreads to the ' +
    'sides.'),
  E('hoodMark', 'hood mark', 'ngbody', HOOD_MARKS, 'On the back of the hood: ' +
    'a cobra\'s spectacles, one great eye, bands, or nothing.'),
  E('ears', 'ears', 'ngbody', EARS, 'Human ears, or fins swept back from ' +
    'the sides of the head (the sea-born).'),
  N('crest', 'head crest', 'ngbody', 'A fin along the top of the skull and ' +
    'down the nape. 0 is none.'),
  E('regalia', 'gold', 'ngbody', REGALIA, 'Gold worn on the body: armbands, ' +
    'a collar, a circlet, or all of it (royal).'),
];

export const COLOR_SPECS = [
  ['pupil', 'pupils', 'The slit, or the pupil, or the whole eye if black.'],
  ['fang', 'fangs', 'The fangs under the upper lip.'],
  ['gold', 'gold', 'Armbands, collar and circlet.'],
];
export const COLOR_LABELS = {
  eye: ['irises', 'The iris around the slit, and the colour of the light ' +
        'it gives when lit.'],
  cloth: ['loincloth', 'The human\'s shorts, worn at the waist above the ' +
          'tail.'],
};

export const PRESETS = {
  naga: { kind: 'cobra' },
  // THE NAGARAJA: a serpent king. Tall, broad, a great hood marked with one
  // eye, gold on every limb and a lit gaze.
  nagaraja: {
    kind: 'royal',
    body: { heightM: 2.15, shoulderWidth: 16, hipWidth: 12, bodyDepth: 10 },
    shape: { chest: 1.12, shoulder: 1.12, armGirth: 1.1 },
    hairStyle: 'bald',
    naga: { regalia: 'royal', glow: true, markings: 'stripe', fangs: 0.6,
            scaleCreep: 0.7, hood: 1, hoodWidth: 0.85 },
    serpent: { length: 2.0, girth: 0.6 },
    colors: { ...SKINS.bronze, ...EYES.gold, ...METALS.gold },
  },
  // THE MERROW: a sea naga, a mermaid in all but name. Fluked, finned and
  // frilled, slim and quick, and she kicks.
  merrow: {
    kind: 'sea',
    body: { sex: 'female', heightM: 1.65 },
    hairStyle: 'flowing',
    naga: { eyes: 'round', faceScales: 0.5, scaleCreep: 0.6, fangs: 0.2,
            venom: false, regalia: 'bands' },
    serpent: { length: 1.6, kick: 0.7, tipSize: 0.75, frills: 0.6 },
    colors: { ...SKINS.jade, ...EYES.ice, ...METALS.silver,
              hair: '#2a5a5a', hairShade: '#1a3e40' },
  },
  // THE SIDEWINDER: a desert viper, lean and long, rattle up, tear-marked.
  sidewinder: {
    kind: 'viper',
    body: { heightM: 1.75, shoulderWidth: 12, hipWidth: 10 },
    shape: { waist: 0.9 },
    hairStyle: 'cropped',
    naga: { markings: 'tear', faceScales: 0.6, fangs: 0.8, scaleCreep: 0.55 },
    serpent: { length: 2.1, girth: 0.35, speed: 1.1, tipSize: 0.6 },
    colors: { ...SKINS.bronze, ...EYES.amber, ...METALS.bronze },
  },
  // THE CONSTRICTOR: a python brute, huge and slow, no venom, all coil.
  constrictor: {
    kind: 'python',
    body: { heightM: 2.15, shoulderWidth: 18, hipWidth: 14, bodyDepth: 12,
            limbWidth: 6 },
    shape: { chest: 1.2, shoulder: 1.2, chestDepth: 1.15, armGirth: 1.25 },
    hairStyle: 'bald',
    naga: { venom: false, fangs: 0.2, scaleCreep: 0.8, faceScales: 0.5 },
    serpent: { length: 2.1, girth: 0.85, speed: 0.75 },
    colors: { ...SKINS.umber, ...EYES.emerald, ...METALS.gold },
  },
  // THE CORALLINE: small, quick, bright and deadly, masked.
  coralline: {
    kind: 'coral',
    body: { sex: 'female', heightM: 1.55 },
    hairStyle: 'bob',
    naga: { markings: 'mask', eyes: 'black', fangs: 0.7, scaleCreep: 0.4 },
    serpent: { speed: 1.15, length: 1.5 },
    colors: { ...SKINS.fair, ...EYES.blood, ...METALS.gold,
              hair: '#1a1414', hairShade: '#0e0a0a' },
  },
  // THE ABYSSAL: from the deep, finned and crested, eyes alight.
  abyssal: {
    kind: 'abyssal',
    body: { heightM: 1.95 },
    hairStyle: 'mohawk',
    naga: { glow: true, eyes: 'slit', markings: 'chevron', faceScales: 0.7,
            scaleCreep: 0.9 },
    colors: { ...SKINS.cobalt, ...EYES.violet, ...METALS.silver,
              hair: '#1a1430', hairShade: '#0e0a20' },
  },
};
export const PRESET_ORDER = Object.keys(PRESETS);

// =============================================================================
// the generator hooks only a naga has (mobgen buildSidecar)
// =============================================================================

/** Sidecar keys a naga adds. CLOSE QUARTERS ARE A BITE (mob.h
 *  MobDef::closeStyle): whatever its behaviour profile swings with, a target
 *  within 0.7 m gets the jaws -- as a STRIKE (`bite_lunge`: the body darts in
 *  and bites), because the standing `bite` reaches about 0.2 m past the face
 *  and a fight is held at arm's length. And the bite carries venom, the
 *  snake's way. */
export const BITE_RANGE = 7;
export function extraSidecar(g) {
  return {
    closeStyle: { style: 'bite_lunge', range: BITE_RANGE },
    ...(g.naga && g.naga.venom ? { bite: { infect: 'envenomed', stain: 'venom' } } : {}),
  };
}

/** Long fangs bite harder. The edge stays the human's jaws'. */
export function patchNatural(g, natural) {
  const f = g.naga ? g.naga.fangs : 0;
  for (const nw of natural)
    if (nw.name === 'jaws' && f > 0.05)
      nw.strike = { ...nw.strike,
                    bite: Math.round(nw.strike.bite * (1 + 0.5 * f) * 10) / 10 };
}

// =============================================================================
// THE SURFACE: eyes, markings, fangs, face scales, the scale creep, gold.
// Part-local at the shipped lattice, front LOW y, z up; never grows a box.
// =============================================================================

export function surfacePass(g, role, name, cells, size, info) {
  const s = g.naga, A = info.ART;
  // The tail paints itself (serpent.js); a part of it reaching here is left.
  if (SE.PARTS.includes(name)) return cells;
  const G = grid(cells, size);
  const { sx, sy, sz, cx, cy } = G;
  const seed = strHash(name) ^ 0x7a6a;
  const SKIN = new Set([A.SKIN_BASE, A.SKIN_SHADE, A.SKIN_LIGHT]);
  const scaleOf = v => (v[1] + 0.5 > cy ? SE.SLOTS.SCALE_SHADE : SE.SLOTS.SCALE);
  const frontOf = (() => {
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y < m.get(k)) m.set(k, y);
    }
    return (x, z) => m.get(x * 4096 + z);
  })();

  // ---- the scale creep: up from the tail -----------------------------------
  const creep = s.scaleCreep;
  if (name === 'hips') {
    // The hips ARE the join: scales everywhere the cloth is not, the belly
    // colour down the front.
    for (const v of G.list()) {
      if (!SKIN.has(v[3]) || !G.exposed(v[0], v[1], v[2])) continue;
      const front = v[1] + 0.5 < cy * 0.6 && Math.abs(v[0] + 0.5 - cx) < sx * 0.22;
      const edge = sz * (0.45 + 0.55 * creep) +
                   (vnoise(seed, v[0], v[1], 0, 3) - 0.5) * sz * 0.3;
      if (v[2] + 0.5 < edge) v[3] = front ? SE.SLOTS.BELLY : scaleOf(v);
    }
  } else if (name === 'torso' && creep > 0.05) {
    // Up the spine further than the flanks, with a ragged edge.
    for (const v of G.list()) {
      if (!SKIN.has(v[3]) || !G.exposed(v[0], v[1], v[2])) continue;
      const back = (v[1] + 0.5 - cy) / (sy / 2);               // -1 front .. 1 back
      const side = Math.abs(v[0] + 0.5 - cx) / (sx / 2);
      const reach = creep * (0.18 + 0.6 * Math.max(0, back) + 0.25 * side * side);
      const n = (vnoise(seed, v[0], v[1], v[2], 2.5) - 0.5) * 0.18;
      if (v[2] / sz < reach + n) v[3] = scaleOf(v);
      // A ridge of the pattern colour right down the spine.
      if (creep > 0.45 && back > 0.85 && Math.abs(v[0] + 0.5 - cx) < 1.1 &&
          v[2] / sz < reach + 0.1)
        v[3] = SE.SLOTS.PATTERN;
    }
  } else if ((name === 'armL.L' || name === 'armL.R') && creep > 0.3) {
    // Scales down the OUTSIDE of the forearm.
    const outer = name.endsWith('.L') ? 1 : -1;
    for (const v of G.list()) {
      if (!SKIN.has(v[3]) || !G.exposed(v[0], v[1], v[2])) continue;
      const o = (v[0] + 0.5 - cx) * outer / (sx / 2);
      if (o > 0.15 + (1 - creep) * 0.5 &&
          vnoise(seed, v[0], v[1], v[2], 2) > 0.3) v[3] = scaleOf(v);
    }
  }

  // ---- gold -------------------------------------------------------------------
  const R = s.regalia;
  const ring = (z0, z1) => {
    for (const v of G.list())
      if (v[2] >= z0 && v[2] < z1 && G.lateral(v[0], v[1], v[2]))
        v[3] = SLOTS.GOLD;
  };
  if ((R === 'bands' || R === 'royal') && (name === 'armU.L' || name === 'armU.R'))
    ring(Math.round(sz * 0.55), Math.round(sz * 0.55) + 3);
  if ((R === 'bands' || R === 'royal') && (name === 'armL.L' || name === 'armL.R'))
    ring(2, 4);
  if ((R === 'collar' || R === 'royal') && name === 'torso')
    ring(sz - 3, sz);

  if (role !== 'head') return keepLargest(G.list(), size);

  // ---- the face ----------------------------------------------------------------
  const eyeRow = info.eyeRow ?? sz * 0.6;
  // The eyes the head says it has (info.isEye, mobgen headMarks), not every
  // cell in the EYE slot: a dark or open mouth is painted in it too.
  const eyeCells = G.list().filter(c => c[3] === A.EYE &&
                                       (!info.isEye || info.isEye(c[0], c[1], c[2])) &&
                                       Math.abs(c[2] + 0.5 - eyeRow) < 5);
  const eyes = [eyeCells.filter(c => c[0] + 0.5 < cx),
                eyeCells.filter(c => c[0] + 0.5 >= cx)].filter(e => e.length);
  for (const grp of eyes) {
    const xs = grp.map(c => c[0]), zs = grp.map(c => c[2]);
    const x0 = Math.min(...xs), x1 = Math.max(...xs);
    const z0 = Math.min(...zs), z1 = Math.max(...zs);
    // A serpent's eye is bigger than a man's pupil: one cell more every way
    // over the front of the face.
    const iris = [];
    for (let z = z0 - 1; z <= z1 + 1; z++)
      for (let x = x0 - 1; x <= x1 + 1; x++) {
        const y = frontOf(x, z);
        if (y === undefined) continue;
        const v = G.get(x, y, z);
        if (!v || !(SKIN.has(v[3]) || v[3] === A.EYE)) continue;
        // Round off the corners of the enlarged eye.
        if ((x === x0 - 1 || x === x1 + 1) && (z === z0 - 1 || z === z1 + 1)) continue;
        iris.push(v);
      }
    const mx = (x0 + x1 + 1) / 2, mz = (z0 + z1 + 1) / 2;
    for (const v of iris) {
      let c = A.EYE;
      if (s.eyes === 'black') c = SLOTS.PUPIL;
      else if (s.eyes === 'slit' && Math.abs(v[0] + 0.5 - mx) < 0.75) c = SLOTS.PUPIL;
      else if (s.eyes === 'round' && Math.abs(v[0] + 0.5 - mx) < 1.1 &&
               Math.abs(v[2] + 0.5 - mz) < 1.1) c = SLOTS.PUPIL;
      v[3] = c;
    }
  }
  const eyeX = eyes.map(grp => grp.reduce((a, c) => a + c[0] + 0.5, 0) / grp.length);
  const eyeZ = eyes.length
    ? eyes[0].reduce((a, c) => a + c[2] + 0.5, 0) / eyes[0].length : eyeRow;
  const paintFront = (x, z, c) => {
    const y = frontOf(x, z);
    if (y === undefined) return;
    const v = G.get(x, y, z);
    if (v && SKIN.has(v[3])) v[3] = c;
  };

  // ---- face scales: cheekbones and temples ------------------------------------
  if (s.faceScales > 0.05) {
    for (const v of G.list()) {
      if (!SKIN.has(v[3]) || !G.exposed(v[0], v[1], v[2])) continue;
      const side = Math.abs(v[0] + 0.5 - cx) / (sx / 2);
      const dz = v[2] + 0.5 - eyeZ;
      if (side < 0.55 || dz < -sz * 0.18 || dz > sz * 0.22) continue;
      if (v[1] + 0.5 > cy * 1.25) continue;            // not round the back
      const t = s.faceScales * (side - 0.5) * 2.2;
      if (vnoise(seed, v[0], v[1], v[2], 1.6) < t) v[3] = SE.SLOTS.SCALE;
    }
  }

  // ---- markings ----------------------------------------------------------------
  const M = s.markings;
  if (M === 'tear')
    for (const ex of eyeX) {
      const outward = ex < cx ? -1 : 1;
      const x = Math.floor(ex + outward * 1.5);
      for (let z = Math.floor(eyeZ) - 2; z > Math.floor(eyeZ) - 9; z--)
        paintFront(x - (z < eyeZ - 5 ? outward : 0), z, SE.SLOTS.PATTERN);
    }
  else if (M === 'mask')
    for (let z = Math.floor(eyeZ) - 2; z <= Math.floor(eyeZ) + 2; z++)
      for (let x = 0; x < sx; x++) paintFront(x, z, SE.SLOTS.PATTERN);
  else if (M === 'stripe')
    for (let z = Math.floor(eyeZ) + 2; z < sz; z++)
      for (const x of [Math.floor(cx) - 1, Math.floor(cx)])
        paintFront(x, z, SE.SLOTS.PATTERN);
  else if (M === 'chevron')
    for (let i = 0; i < 6; i++)
      for (const side of [-1, 1]) {
        const x = Math.floor(cx + side * (i + 0.5) - 0.5);
        for (const dz of [0, 1])
          paintFront(x, Math.floor(eyeZ) + 6 - i + dz, SE.SLOTS.PATTERN);
      }

  // ---- fangs: under the upper lip, either side of the middle --------------------
  if (s.fangs > 0.05 && info.mouthRow !== undefined) {
    const n = 1 + Math.round(s.fangs * 2.5);
    const mx = cx + (info.mouthDx || 0);
    const top = Math.floor(info.mouthRow) - 1;
    for (const side of [-1, 1]) {
      const x = Math.floor(mx + side * 2 - (side < 0 ? 1 : 0));
      for (let z = top; z > top - n; z--) {
        const y = frontOf(x, z);
        if (y === undefined) continue;
        const v = G.get(x, y, z);
        if (v) v[3] = SLOTS.FANG;
      }
    }
  }

  // ---- the circlet ------------------------------------------------------------
  if (R === 'circlet' || R === 'royal') {
    const z0 = Math.round(eyeZ + (sz - eyeZ) * 0.4);
    for (const v of G.list())
      if (v[2] >= z0 && v[2] < z0 + 2 && G.lateral(v[0], v[1], v[2]))
        v[3] = SLOTS.GOLD;
    // A stone at the front, in the iris colour.
    const x = Math.floor(cx) - 1;
    for (const dx of [0, 1]) for (const dz of [0, 1, 2]) {
      const y = frontOf(x + dx, z0 - 1 + dz);
      if (y !== undefined) { const v = G.get(x + dx, y, z0 - 1 + dz); if (v) v[3] = A.EYE; }
    }
  }
  void rnd;
  return keepLargest(G.list(), size);
}

// =============================================================================
// the extras: the hood (rides the torso, fixed), fin ears and a crest (ride
// the head). SCENE coordinates at the shipped lattice (x right, y BACK, z up).
// =============================================================================

export function extrasWant(g) {
  const s = g.naga;
  return s.hood > 0.02 || s.ears === 'fins' || s.crest > 0.02;
}

export function extras(g, ctx) {
  const s = g.naga;
  const { K, body, head, torso, ART } = ctx;
  const headOut = new Map(), hoodOut = new Map();
  const free = (X, Y, Z) => !body.has(K(X, Y, Z)) && !headOut.has(K(X, Y, Z)) &&
                            !hoodOut.has(K(X, Y, Z));
  const addH = (X, Y, Z, c) => { if (free(X, Y, Z)) headOut.set(K(X, Y, Z), [X, Y, Z, c]); };
  const addD = (X, Y, Z, c) => { if (free(X, Y, Z)) hoodOut.set(K(X, Y, Z), [X, Y, Z, c]); };
  const H = extent(head);
  const hc = sceneCells(head);
  const eyeZs = hc.filter(c => c[3] === ART.EYE).map(c => c[2]);
  // The head's own eye line (ctx.eyeLine, mobgen headMarks) when it has one:
  // the mean of every EYE-slot cell moves with a dark or open mouth.
  const eyeZ = ctx.eyeLine ? (ctx.eyeLine[0] + ctx.eyeLine[1]) / 2
    : eyeZs.length ? eyeZs.reduce((a, b) => a + b, 0) / eyeZs.length
                   : H.z0 + (H.z1 - H.z0) * 0.6;
  const headH = H.z1 - H.z0 + 1, halfW = (H.x1 - H.x0 + 1) / 2;

  // ---- the hood: a teardrop fan behind the neck, cupped forward at the edges
  if (s.hood > 0.02) {
    const T = extent(torso);
    const tc = sceneCells(torso);
    // The torso's back in its top rows, and the head's: the hood stands just
    // behind whichever is further back, and a fold of neck joins it to the
    // torso (it has to TOUCH the torso to be kept).
    let tBack = -1e9;
    for (const [, y, z] of tc) if (z >= T.z1 - 3) tBack = Math.max(tBack, y);
    let hBack = -1e9;
    for (const [, y, z] of hc) if (z <= H.z0 + headH * 0.5) hBack = Math.max(hBack, y);
    const yb = Math.max(tBack, hBack) + 1;
    const zBot = T.z1 - Math.round((T.z1 - T.z0 + 1) * 0.22);
    const zTop = Math.round(H.z0 + headH * (0.45 + 0.6 * s.hood));
    const rxMax = halfW * (1.05 + 1.7 * s.hoodWidth);
    const xc = (H.x0 + H.x1 + 1) / 2;
    const zc = zBot + (zTop - zBot) * 0.62;
    const rz = (zTop - zBot) * 0.5;
    for (let z = zBot; z <= zTop; z++) {
      const t = (z - zBot) / Math.max(1, zTop - zBot);
      const half = rxMax * Math.pow(Math.sin(Math.PI * Math.min(1, t * 0.92 + 0.08)), 0.75);
      for (let x = Math.floor(xc - half); x < Math.ceil(xc + half); x++) {
        const u = (x + 0.5 - xc) / Math.max(1, half);
        const cup = Math.round(u * u * 4 * s.hoodWidth);
        const thick = Math.abs(u) < 0.6 ? 2 : 1;
        for (let k = 0; k < thick; k++) {
          const y = yb - cup + k;
          const backFace = k === thick - 1;
          let c = backFace ? SE.SLOTS.SCALE : SE.SLOTS.BELLY;
          if (backFace && Math.abs(u) > 0.75) c = SE.SLOTS.SCALE_SHADE;
          if (backFace) {
            const ex = Math.abs(x + 0.5 - xc), ez = z + 0.5 - (zc + rz * 0.1);
            if (s.hoodMark === 'spectacle') {
              const d = Math.hypot(ex - rxMax * 0.42, ez) / (rxMax * 0.22);
              if (d < 1 && d > 0.55) c = SE.SLOTS.PATTERN;
              else if (d <= 0.55) c = SE.SLOTS.BELLY;
              // ...joined by a bridge, the cobra's spectacles.
              if (ex < rxMax * 0.25 && Math.abs(ez + rxMax * 0.12) < 1.2)
                c = SE.SLOTS.PATTERN;
            } else if (s.hoodMark === 'eye') {
              const d = Math.hypot(ex, ez) / (rxMax * 0.38);
              if (d < 1 && d > 0.62) c = SE.SLOTS.PATTERN;
              else if (d <= 0.62 && d > 0.3) c = SE.SLOTS.BELLY;
              else if (d <= 0.3) c = SE.SLOTS.PATTERN;
            } else if (s.hoodMark === 'bands') {
              if (Math.floor((z - zBot) / 3) % 2 === 0) c = SE.SLOTS.PATTERN;
            }
          }
          addD(x, y, z, c);
        }
      }
    }
    // The neck fold that roots it.
    for (let z = T.z1 - 3; z <= T.z1; z++)
      for (let x = Math.floor(xc) - 2; x < Math.floor(xc) + 2; x++)
        for (let y = tBack + 1; y < yb; y++) addD(x, y, z, SE.SLOTS.SCALE);
  }

  // ---- fin ears: swept back and up from the sides of the head -----------------
  if (s.ears === 'fins') {
    const len = Math.max(4, Math.round(headH * 0.45));
    const tall = Math.max(3, Math.round(headH * 0.35));
    for (const side of [-1, 1]) {
      // The outermost head column at the eye row, on this side.
      let edge = side > 0 ? -1e9 : 1e9, ey = 0;
      for (const [x, y, z] of hc)
        if (Math.abs(z - eyeZ) < 2 && (side > 0 ? x > edge : x < edge)) {
          edge = x; ey = y;
        }
      if (!Number.isFinite(edge)) continue;
      // Two cells proud of the cheek and no more: a fin lies back along the
      // head, rising toward its trailing edge, with spines every third row.
      const x0 = edge + side;
      for (let a = 0; a < len; a++) {
        const t = a / len;
        const h = Math.round(tall * (0.35 + 0.65 * Math.sin(Math.PI * Math.min(1, t * 1.1))));
        const spine = a % 3 === 0;
        const z0 = Math.round(eyeZ) - 2 + Math.round(t * 2);
        for (let z = z0; z < z0 + h; z++)
          addH(x0 + (a > len * 0.6 ? side : 0), ey + a, z,
               spine ? SE.SLOTS.SCALE_SHADE : SE.SLOTS.FIN);
      }
    }
  }

  // ---- the crest: a fin along the top of the skull and down the nape ----------
  if (s.crest > 0.02) {
    const topAt = new Map();
    for (const [x, y, z] of hc) {
      const k = x * 4096 + y;
      if (!topAt.has(k) || z > topAt.get(k)) topAt.set(k, z);
    }
    const xm = Math.floor((H.x0 + H.x1 + 1) / 2);
    const depth = H.y1 - H.y0 + 1;
    for (let y = H.y0 + Math.round(depth * 0.3); y <= H.y1; y++) {
      const t = (y - H.y0) / depth;
      const h = Math.round(s.crest * headH * 0.4 * Math.sin(Math.PI * clamp((t - 0.25) / 0.8, 0, 1))) + 1;
      for (const x of [xm - 1, xm]) {
        const top = topAt.get(x * 4096 + y);
        if (top === undefined) continue;
        for (let z = top + 1; z <= top + h; z++)
          addH(x, y, z, y % 3 === 0 ? SE.SLOTS.SCALE_SHADE : SE.SLOTS.FIN);
      }
    }
  }

  return { head: [...headOut.values()], torso: [], hood: [...hoodOut.values()] };
}
