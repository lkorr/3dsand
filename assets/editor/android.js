/* android.js — THE ANDROID RACE: synthetic people on the human rig.
 *
 * WHAT AN ANDROID IS, TO THE GENERATOR. The human body -- the SAME fifteen
 * limb boxes, anchors, sockets, chains and clips, so every piece of armour
 * and every weapon fits (test_mobgen section O) -- shelled in smooth panels
 * with fine seams, accent light strips and circuit traces, a power core
 * glowing in the chest, bare frame at the joints, and a face that is a visor,
 * a dark faceplate, a single scanning eye, a speaker grille, or an uncanny
 * near-human one. Off the body: antennae, swept head fins, power packs on the
 * back, a halo.
 *
 * WHAT IT IS MADE OF (materials.json, the robot rows). Panels are
 * `synth_shell` (hardness 45, `shell`); under them `circuitry` (soft, bleeds
 * COOLANT and ARCS -- almost every blow that reaches it throws a spark), an
 * `alloy` endoskeleton (hardness 180: a limb does not come off to a sword),
 * a `power_cell` in the chest and a `neural_core` (brainHp) in the head. The
 * lights are `gem_arcane` (emission; the art colour is the light's colour),
 * the visor `glass`, the joints `steel`. A breached power cell, or a dead
 * android, DISCHARGES: a spray of sparks along jagged arcs that lights what
 * burns and pops what explodes. It bleeds coolant -- cyan, faintly glowing,
 * and an extinguisher, where the automaton's oil is a fuel.
 *
 * synth_shell / alloy / circuitry / power_cell / neural_core are ids > 127,
 * painted as STAND-INS and rewritten at load (anatomy.becomes, mob.cpp).
 *
 * HUMANS ARE UNTOUCHED: every entry point is reached only for
 * `genome.body.race === 'android'`, and every gene here is race-gated.
 * PURE AND NODE-RUNNABLE; imports only robotkit.js.
 */

'use strict';

import { rnd, strHash, clamp, grid, disc, keepLargest, readFace, line6,
         cylinder, sceneCells, extent } from './robotkit.js';

export const RACE = 'android';
export const FOLDER = 'android';
export const NEW_NAME = 'unit';
export const BLURB = 'an android: smooth synthetic panels over circuitry and ' +
  'an alloy frame, light strips, a glowing power core. Bleeds coolant, arcs ' +
  'when struck, discharges when it dies; its frame shrugs off a blade. Same ' +
  'limbs and proportions, so armour and weapons still fit.';

// =============================================================================
// art slots and materials
// =============================================================================

/** The android's own art slots, below the automaton's (228). */
export const SLOTS = {
  LIGHT: 227,   // accent light strips, the visor slit, antenna tips (glow)
  SEAM: 226,    // panel lines
  FRAME: 225,   // bare frame at the joints (steel)
  CORE: 224,    // the power core in the chest (glow)
  VISOR: 223,   // dark faceplate and visor glass
  TRACE: 222,   // circuit traces: a dimmer light
};

export const COLOR_SLOTS = {
  light: SLOTS.LIGHT, seam: SLOTS.SEAM, frame: SLOTS.FRAME,
  core: SLOTS.CORE, visor: SLOTS.VISOR, trace: SLOTS.TRACE,
};

export const MATS = {
  shell: 'synth_shell',
  glow: 'gem_arcane',
  frame: 'steel',
  visor: 'glass',
};
export const DEFAULT_MAT = MATS.shell;

export const STAND_INS = {
  synth_shell: 'mushroom_stem',
  alloy: 'mushroom_gill',
  circuitry: 'petal_blue',
  power_cell: 'petal_pink',
  neural_core: 'petal_red',
};

/** Coolant: cyan, faintly lit, an extinguisher. */
export const BLEED = { material: 'coolant', perDamage: 1.2 };

export const ANATOMY = {
  layers: [
    { material: 'synth_shell', depth: 1, keep: true },
    { material: 'circuitry', depth: 1 },
    { material: 'alloy' },
  ],
  garments: [],
  limbs: {
    head: {
      layers: [
        { material: 'synth_shell', depth: 1, keep: true },
        { material: 'alloy', depth: 1 },
        { material: 'circuitry', depth: 1 },
        { material: 'neural_core' },
      ],
    },
    torso: {
      carve: [
        { material: 'power_cell', where: 'circuitry',
          box: { x: [0.32, 0.68], y: [0.48, 0.8], z: [0.45, 1] } },
        { material: 'power_cell', where: 'alloy',
          box: { x: [0.32, 0.68], y: [0.48, 0.8], z: [0.45, 1] } },
      ],
    },
  },
};

export const ANATOMY_NOTES = {
  '//': 'AN ANDROID: synthetic shell panels over a layer of circuitry over an ' +
        'alloy endoskeleton, a power cell in the chest behind the core window ' +
        'and a neural core (brainHp) in the head. The surface is synth_shell, ' +
        'gem_arcane lights, steel joints and a glass visor -- all KEPT; none ' +
        'of them appears below. synth_shell, circuitry, alloy, power_cell and ' +
        'neural_core are material ids above 127, which a .vox cannot paint, so ' +
        'the recipe names their stand-ins and `becomes` rewrites them at load ' +
        '(mob.cpp, as snake.json\'s venom gland).',
};

export function slotMaterialNames(g, ART) {
  const m = {};
  for (const s of [ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT, ART.NAIL,
                   ART.CLOTH, ART.CLOTH_SHADE, ART.HAIR, ART.HAIR_SHADE,
                   ART.HAIR_WISP1, ART.HAIR_WISP2, ART.HAIR_WISP3, SLOTS.SEAM])
    m[s] = MATS.shell;
  for (const s of [SLOTS.LIGHT, SLOTS.CORE, SLOTS.TRACE]) m[s] = MATS.glow;
  m[SLOTS.FRAME] = MATS.frame;
  m[SLOTS.VISOR] = MATS.visor;
  m[ART.EYE] = g.android && g.android.glow ? MATS.glow : MATS.visor;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export function defaultGenes() {
  return {
    face: 'visor',     // a face preset (FACES); the numbers below are the truth
    glow: true,        // the eyes are lit
    eyeSize: 1,        // 0 no eyes .. 2 large
    visor: 0.6,        // a visor band across the eyes: its height
    faceplate: 0,      // a dark glass plate over the whole face
    mono: false,       // one central scanning eye
    grille: 0,         // speaker slits at the mouth
    mouthLight: 0,     // a lit bar where the mouth is
    seams: 0.5,        // panel lines on the face
    human: 0,          // keep the human nose and mouth relief
    ears: 0.4,         // ear discs, lit round the rim
    panels: 0.5,       // panel lines on the body
    lights: 0.5,       // accent light strips
    traces: 0.25,      // circuit traces in the shell
    core: 0.6,         // the power core window in the chest
    joints: 0.6,       // bare frame at the elbows, knees, neck
    twoTone: 0.3,      // accent panels in the second colour
    sheen: 0.5,        // a specular streak down every panel
    gear: 'none',      // a fittings preset (GEARS); the numbers below rule
    antenna: 0,        // antennae: one, then two, and their length
    fins: 0,           // swept fins on the sides of the head
    packs: 0,          // power packs on the back
    halo: 0,           // a lit ring over the head
  };
}


/** Colour sets a roll picks WHOLE: a shell, a suit, an accent. The nail,
 *  frame and visor colours are the race's own and in no set, and the core
 *  burns the eye colour -- fewer colours each in the engine's shared art
 *  palette. */
export const SHELLS = {
  white:    { skin: '#e8ecf0', skinShade: '#b4bcc6', skinLight: '#ffffff', seam: '#5a6270', hair: '#b4bcc6', hairShade: '#8a929c' },
  obsidian: { skin: '#2a2e34', skinShade: '#1a1d22', skinLight: '#5a6470', seam: '#0a0c0e', hair: '#1a1d22', hairShade: '#101214' },
  chrome:   { skin: '#c8ccd2', skinShade: '#7a8088', skinLight: '#ffffff', seam: '#4a4e56', hair: '#7a8088', hairShade: '#5a5e66' },
  crimson:  { skin: '#b8303a', skinShade: '#7a1e26', skinLight: '#e8606a', seam: '#3a1218', hair: '#7a1e26', hairShade: '#521418' },
  sand:     { skin: '#d8c8a8', skinShade: '#a8987a', skinLight: '#f4e8cc', seam: '#6a5e48', hair: '#a8987a', hairShade: '#7e7058' },
  olive:    { skin: '#6a7250', skinShade: '#4a5036', skinLight: '#8e9870', seam: '#2e3222', hair: '#4a5036', hairShade: '#343824' },
  pearl:    { skin: '#f0e4ea', skinShade: '#c4b0bc', skinLight: '#ffffff', seam: '#8a7884', hair: '#c4b0bc', hairShade: '#9a8692' },
  navy:     { skin: '#2c3e60', skinShade: '#1c2a44', skinLight: '#4e6a96', seam: '#0e1626', hair: '#1c2a44', hairShade: '#121c30' },
};
export const SUITS = {
  black:    { cloth: '#2a2e36', clothShade: '#1a1d22' },
  graphite: { cloth: '#4a4e56', clothShade: '#33363c' },
  white:    { cloth: '#d8dce2', clothShade: '#a8aeb6' },
  orange:   { cloth: '#e07a2a', clothShade: '#a8561a' },
  teal:     { cloth: '#2a8a8a', clothShade: '#1c6262' },
  red:      { cloth: '#b8303a', clothShade: '#7a1e26' },
};
/** The accent sets: strips, eyes, core and traces lit in one colour. */
export const ACCENTS = {
  // The strips, the eyes and the core burn ONE colour (one art palette
  // entry), the sylvan's glow hexes where they match (sylvan.js GLOWS);
  // the traces are its dim twin.
  cyan:    { light: '#6ae8ff', eye: '#6ae8ff', core: '#6ae8ff', trace: '#2a8aa8' },
  red:     { light: '#ff3a3a', eye: '#ff3a3a', core: '#ff3a3a', trace: '#a82a2a' },
  amber:   { light: '#ffc24a', eye: '#ffc24a', core: '#ffc24a', trace: '#a8742a' },
  green:   { light: '#b6ff6a', eye: '#b6ff6a', core: '#b6ff6a', trace: '#5a9a2a' },
  violet:  { light: '#c08aff', eye: '#c08aff', core: '#c08aff', trace: '#6a4aa8' },
  white:   { light: '#cfe4ff', eye: '#cfe4ff', core: '#cfe4ff', trace: '#8a9aaa' },
  magenta: { light: '#ff7ac0', eye: '#ff7ac0', core: '#ff7ac0', trace: '#a82a7a' },
};
/** The race's default look: a white shell, a black suit, cyan lights, and
 *  the colours every android shares (SHARED_COLORS). */
export const SHARED_COLORS = {
  nail: '#3a4048', frame: '#6a727c', visor: '#141a22',
};
export const DEFAULT_COLORS = {
  ...SHELLS.white, ...SUITS.black, ...ACCENTS.cyan, ...SHARED_COLORS,
};
/** Eye swatches (the Characters page's stock glows). */
export const GLOWS = Object.fromEntries(Object.entries(ACCENTS)
  .map(([k, v]) => [k, v.eye]));
export const COLOR_SETS = [SHELLS, SUITS, ACCENTS];
export const STOCK = Object.entries(SHELLS)
  .sort((a, b) => lum(a[1].skin) - lum(b[1].skin));
export const STOCK_LABEL = 'shell';
function lum(hex) {
  const n = parseInt(hex.slice(1), 16);
  return ((n >> 16) & 255) * 0.3 + ((n >> 8) & 255) * 0.59 + (n & 255) * 0.11;
}

/** Within-box build offsets only. An android is a little leaner and finer
 *  boned than the human it was switched from. */
export const BUILD_DELTA = {
  'shape.waist': -0.06,
  'shape.seat': -0.04,
  'head.jaw': -0.05,
  'head.crown': 0.03,
};

const FACE_KEYS = ['eyeSize', 'visor', 'faceplate', 'mono', 'grille',
                   'mouthLight', 'seams', 'human'];
export const FACES = {
  visor:    { eyeSize: 1,   visor: 0.6, faceplate: 0,   mono: false, grille: 0,   mouthLight: 0,   seams: 0.5, human: 0 },
  synth:    { eyeSize: 0.7, visor: 0,   faceplate: 0,   mono: false, grille: 0,   mouthLight: 0,   seams: 0.6, human: 1 },
  faceless: { eyeSize: 0,   visor: 0,   faceplate: 0,   mono: false, grille: 0,   mouthLight: 0,   seams: 0.2, human: 0 },
  scanner:  { eyeSize: 1.4, visor: 0,   faceplate: 0,   mono: true,  grille: 0.5, mouthLight: 0,   seams: 0.5, human: 0 },
  mask:     { eyeSize: 0.8, visor: 0,   faceplate: 0.9, mono: false, grille: 0,   mouthLight: 0,   seams: 0.3, human: 0 },
  speaker:  { eyeSize: 1.1, visor: 0,   faceplate: 0,   mono: false, grille: 0.8, mouthLight: 0.7, seams: 0.5, human: 0 },
};
export const FACE_ORDER = Object.keys(FACES);

const GEAR_KEYS = ['antenna', 'fins', 'packs', 'halo'];
export const GEARS = {
  none:     { antenna: 0,   fins: 0,   packs: 0,   halo: 0 },
  antenna:  { antenna: 0.4, fins: 0,   packs: 0,   halo: 0 },
  fins:     { antenna: 0,   fins: 0.7, packs: 0,   halo: 0 },
  packs:    { antenna: 0,   fins: 0,   packs: 0.7, halo: 0 },
  sentinel: { antenna: 0.8, fins: 0.5, packs: 0.6, halo: 0 },
  seraph:   { antenna: 0,   fins: 0.4, packs: 0,   halo: 0.8 },
};
export const GEAR_ORDER = Object.keys(GEARS);

export function applyFace(genome, style) {
  const f = FACES[style];
  if (!f) return genome;
  genome.android.face = style;
  for (const k of FACE_KEYS) genome.android[k] = f[k];
  return genome;
}
export function applyGear(genome, style) {
  const f = GEARS[style];
  if (!f) return genome;
  genome.android.gear = style;
  for (const k of GEAR_KEYS) genome.android[k] = f[k];
  return genome;
}

export const PICKERS = [
  { key: 'face', path: 'android.face', label: 'face', order: FACE_ORDER,
    apply: applyFace,
    title: 'a starting face: a visor, a near-human synth, faceless, a single ' +
           'scanning eye, a dark faceplate, a speaker grille' },
  { key: 'gear', path: 'android.gear', label: 'fittings', order: GEAR_ORDER,
    apply: applyGear,
    title: 'what stands off the body: antennae, head fins, power packs on ' +
           'the back, a halo' },
];

export function onBecome(genome, h) {
  if (!h.isLocked('hair.style')) h.applyHairStyle(genome, 'bald');
  if (!h.isLocked('face.beardStyle')) h.applyBeardStyle(genome, 'clean');
  if (!h.isLocked('hair.fuzz')) genome.hair.fuzz = 0;
}

export function randomize(g, r, isLocked) {
  if (!isLocked('android.face')) applyFace(g, r.pick(FACE_ORDER));
  if (!isLocked('android.gear')) applyGear(g, r.pick(GEAR_ORDER));
  for (const spec of GENE_SPECS) {
    if (spec.kind || isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    const v = g.android[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    g.android[k] = clamp(v, spec.min, spec.max);
  }
  if (!isLocked('android.glow')) g.android.glow = r() < 0.92;
  return g;
}

export const GENE_GROUPS = [
  { key: 'andface', title: 'android face', race: 'android',
    note: 'A visor, a faceplate, one eye or two, a speaker grille, or a face ' +
          'near enough human to unsettle. The eyes and every strip give ' +
          'light; their colours are the eye and light colours.' },
  { key: 'andbody', title: 'shell and lights', race: 'android',
    note: 'Panels over circuitry over an alloy frame. Seams and the joint ' +
          'gaps are cut into the shell, never out of it, so armour still ' +
          'fits. The core window is where a blade finds the power cell.' },
  { key: 'andgear', title: 'fittings', race: 'android',
    note: 'Antennae, head fins, power packs, a halo: bloodless parts riding ' +
          'the head or the torso.' },
];

const S = (path, label, group, hint, extra = {}) => ({
  path: 'android.' + path, label, group, min: 0, max: 1, step: 0.05,
  sigma: 0.2, race: 'android', hint, ...extra });

export const GENE_SPECS = [
  { path: 'android.face', label: 'face', group: 'andface', kind: 'enum',
    choices: FACE_ORDER, sigma: 0.15, race: 'android',
    hint: 'A starting face: visor, synth (near-human), faceless, scanner (one ' +
          'eye), mask (a dark faceplate), speaker.' },
  { path: 'android.glow', label: 'lit eyes', group: 'andface', kind: 'bool',
    sigma: 0, race: 'android',
    hint: 'The eyes are lit and give light. Off, they are dark glass.' },
  S('eyeSize', 'eye size', 'andface', 'No eyes at 0, large lenses at 2.',
    { max: 2, sigma: 0.3 }),
  S('visor', 'visor', 'andface', 'A dark visor band across the eyes, a lit ' +
    'slit through it. Its height.'),
  S('faceplate', 'faceplate', 'andface', 'A dark glass plate over the whole ' +
    'face, the eyes glowing through it.', { roll: 0.25 }),
  { path: 'android.mono', label: 'single eye', group: 'andface', kind: 'bool',
    sigma: 0, race: 'android', roll: 0.2,
    hint: 'One central scanning eye instead of two.' },
  S('grille', 'speaker grille', 'andface', 'Vertical slits where the mouth is.'),
  S('mouthLight', 'voice light', 'andface', 'A lit bar where the mouth is, ' +
    'that would flicker as it spoke.', { roll: 0.3 }),
  S('seams', 'face seams', 'andface', 'Panel lines round the face and across ' +
    'the cheeks and brow.'),
  S('human', 'human features', 'andface', 'Keep the human nose and mouth ' +
    'relief (1), or smooth them away (0).'),
  S('ears', 'ear discs', 'andface', 'A disc where each ear would be, lit ' +
    'round the rim.'),

  S('panels', 'panels', 'andbody', 'How finely the shell is divided into panels.'),
  S('lights', 'light strips', 'andbody', 'Accent strips down the limbs and ' +
    'the chest.'),
  S('traces', 'circuit traces', 'andbody', 'Faintly lit circuit traces running ' +
    'through the shell.'),
  S('core', 'power core', 'andbody', 'The power core glowing through a window ' +
    'in the chest. It is a lamp.'),
  S('joints', 'bare joints', 'andbody', 'The frame showing at the elbows, ' +
    'knees and neck, the shell stepped back from it.'),
  S('twoTone', 'two-tone', 'andbody', 'Accent panels in the second colour ' +
    'down the sides and the chest.'),
  S('sheen', 'sheen', 'andbody', 'A bright specular streak down each panel.'),

  { path: 'android.gear', label: 'fittings', group: 'andgear', kind: 'enum',
    choices: GEAR_ORDER, sigma: 0.15, race: 'android',
    hint: 'A starting set: none, antenna, fins, packs, sentinel (all three), ' +
          'seraph (fins and a halo).' },
  S('antenna', 'antennae', 'andgear', 'One antenna, then two, and how long.',
    { roll: 0.3 }),
  S('fins', 'head fins', 'andgear', 'Fins swept back from the sides of the head.',
    { roll: 0.3 }),
  S('packs', 'power packs', 'andgear', 'Twin power packs on the back, lit at ' +
    'the base.', { roll: 0.3 }),
  S('halo', 'halo', 'andgear', 'A lit ring over the head.', { roll: 0.1 }),
];

export const COLOR_SPECS = [
  ['light', 'light strips', 'Accent strips, the visor slit, antenna tips, the halo.'],
  ['seam', 'panel lines', 'The seams between panels.'],
  ['frame', 'frame', 'The bare frame at the joints, antennae.'],
  ['core', 'power core', 'The core glowing in the chest. It gives light.'],
  ['visor', 'visor', 'Dark visor and faceplate glass, eye sockets.'],
  ['trace', 'circuit traces', 'The faint traces in the shell.'],
];
export const COLOR_LABELS = {
  skin: ['shell', 'The shell panels. Most of the body is this.'],
  skinShade: ['shell, shaded', 'Panels turned from the light.'],
  skinLight: ['shell, sheen', 'The specular streak.'],
  hair: ['sculpted hair', 'Hair, if you give an android any: it is moulded.'],
  hairShade: ['sculpted hair, shaded', 'The same, turned from the light.'],
  eye: ['eyes', 'The eyes, and the colour of the light they give.'],
  cloth: ['accent', 'The second colour: the hip section (the human\'s ' +
          'shorts) and the two-tone panels.'],
  clothShade: ['accent, shaded', 'The second colour turned from the light.'],
  nail: ['fingertips', 'The finger and toe tips.'],
};

export const PRESETS = {
  android: { face: 'visor', gear: 'none' },
  courier: {
    face: 'visor', gear: 'antenna',
    body: { heightM: 1.74 },
    colors: { ...SHELLS.white, ...SUITS.orange, ...ACCENTS.cyan },
    android: { lights: 0.7, twoTone: 0.6 },
  },
  replicant: {
    face: 'synth', gear: 'none',
    body: { sex: 'female', heightM: 1.7 },
    colors: { ...SHELLS.pearl, ...SUITS.white, ...ACCENTS.violet },
    android: { panels: 0.3, lights: 0.2, traces: 0.1, sheen: 0.7, core: 0.4 },
  },
  sentinel: {
    face: 'scanner', gear: 'sentinel',
    body: { heightM: 1.9, shoulderWidth: 14, bodyDepth: 10, limbWidth: 6 },
    shape: { chest: 1.12, shoulder: 1.12, armGirth: 1.05 },
    colors: { ...SHELLS.obsidian, ...SUITS.red, ...ACCENTS.red },
    android: { panels: 0.75, lights: 0.8, traces: 0.5, core: 0.9 },
  },
  seraph: {
    face: 'faceless', gear: 'seraph',
    body: { heightM: 1.8 },
    colors: { ...SHELLS.chrome, ...SUITS.graphite, ...ACCENTS.white },
    android: { sheen: 0.9, lights: 0.6, panels: 0.4 },
  },
  operator: {
    face: 'mask', gear: 'packs',
    body: { heightM: 1.78 },
    colors: { ...SHELLS.olive, ...SUITS.black, ...ACCENTS.amber },
    android: { panels: 0.65, traces: 0.4, twoTone: 0.2 },
  },
};
export const PRESET_ORDER = Object.keys(PRESETS);

// =============================================================================
// the surface: panels, seams, lights, traces, the core, bare joints, sheen,
// the face. Part-local, front LOW y, z up; never grows a box.
// =============================================================================

export function surfacePass(g, role, name, cells, size, info) {
  const s = g.android, A = info.ART;
  const G = grid(cells, size);
  const { sx, sy, sz, cx, cy } = G;
  const seed = strHash(name) ^ 0xa4d2;
  const SHELL = new Set([A.SKIN_BASE, A.SKIN_SHADE, A.SKIN_LIGHT]);
  const plateOf = v => (v[1] + 0.5 > cy ? A.SKIN_SHADE : A.SKIN_BASE);
  const owned = new Set();
  const own = (x, y, z) => owned.add(G.idx(x, y, z));
  const isOwned = (x, y, z) => owned.has(G.idx(x, y, z));
  const paintOwn = (x, y, z, c) => { if (G.paint(x, y, z, c)) own(x, y, z); };
  const free = v => SHELL.has(v[3]) && !isOwned(v[0], v[1], v[2]);
  const frontOf = (() => {
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y < m.get(k)) m.set(k, y);
    }
    return (x, z) => m.get(x * 4096 + z);
  })();
  const perim = Math.PI * (sx + sy) / 2;
  const tDist = (x, y, t0) => {
    const d = Math.abs(G.turn(x, y) - t0);
    return Math.min(d, 1 - d) * perim;
  };

  // ---- the face -----------------------------------------------------------
  let face = null;
  if (role === 'head') {
    face = carveFace(g, A, G, info, seed, paintOwn, plateOf);
    for (const [x, y, z] of G.list())
      if (y + 0.5 < cy && z + 0.5 > face.chinZ && z + 0.5 < face.browZ + 2 &&
          Math.abs(x + 0.5 - cx) < face.halfW) own(x, y, z);
  }

  // ---- the power core (the chest) -------------------------------------------
  if (role === 'trunk' && name === 'torso' && s.core > 0.05) {
    const R = Math.min(1.4 + s.core * 2.8, Math.min(sx, sz) * 0.26);
    const pz = sz * 0.66, px = cx;
    disc(G, frontOf, -1, px, pz, R, R, 1,
         d => (d < 0.7 ? SLOTS.CORE : d < 0.86 ? SLOTS.LIGHT : SLOTS.SEAM),
         (d, a) => (Math.abs(Math.sin(a * 3)) < 0.25 ? SLOTS.LIGHT : SLOTS.SEAM),
         1.2);
    for (const v of G.list())
      if (Math.hypot(v[0] + 0.5 - px, v[2] + 0.5 - pz) < R + 1.4 &&
          v[1] + 0.5 < cy) own(v[0], v[1], v[2]);
  }

  // ---- bare joints: the shell steps back and the frame shows ----------------
  if (s.joints > 0.05 && (role === 'limb' || role === 'head')) {
    const n = Math.max(1, Math.round(s.joints * 2.5));
    const inBand = z => role === 'head' ? z <= (info.neckTop || 0) + 1
                                        : z < n || z >= sz - n;
    const band = G.list().filter(v => inBand(v[2]) &&
                                      G.lateral(v[0], v[1], v[2]) &&
                                      !isOwned(v[0], v[1], v[2]));
    for (const [x, y, z] of band) {
      if (!G.has(x, y, z)) continue;
      const [ix, iy] = G.inward(x, y);
      if (G.has(ix, iy, z)) {
        G.del(x, y, z);
        paintOwn(ix, iy, z, SLOTS.FRAME);
      } else paintOwn(x, y, z, SLOTS.FRAME);
    }
    // A lit ring round the middle of each knee and elbow band.
    if (s.lights > 0.45 && role === 'limb')
      for (const v of G.list()) {
        const [x, y, z] = v;
        if ((z === 0 || z === sz - 1) && v[3] === SLOTS.FRAME &&
            G.lateral(x, y, z)) v[3] = SLOTS.LIGHT;
      }
  }

  // ---- two-tone accent panels -------------------------------------------------
  if (s.twoTone > 0.05 && role !== 'hand' && role !== 'head') {
    const w = s.twoTone * (role === 'trunk' ? 0.16 : 0.22) * perim;
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!free(v) || !G.lateral(x, y, z)) continue;
      // The outside of each limb (turn 0 / 0.5 are the +x / -x sides); the
      // trunk's flanks.
      const d = Math.min(tDist(x, y, 0), tDist(x, y, 0.5));
      if (d < w) v[3] = y + 0.5 > cy ? A.CLOTH_SHADE : A.CLOTH;
    }
  }

  // ---- panel lines --------------------------------------------------------------
  if (s.panels > 0.05 && role !== 'hand') {
    const N = Math.round(20 - s.panels * 12);
    const off = Math.floor(rnd(seed, 3) * N);
    const vT = role === 'trunk' ? (s.panels > 0.55 ? [0.75, 0.25, 0.1, 0.4, 0.6, 0.9]
                                                   : [0.75, 0.25])
             : role === 'head' ? [0.25] : [0.12, 0.38, 0.62, 0.88].slice(0,
                 s.panels > 0.5 ? 4 : 2);
    const onH = z => role !== 'head' && (z + off) % N === 0 && z > 1 && z < sz - 2;
    const cut = s.panels > 0.4;
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!(free(v) || v[3] === A.CLOTH || v[3] === A.CLOTH_SHADE) ||
          !G.lateral(x, y, z)) continue;
      if (!(onH(z) || vT.some(t0 => tDist(x, y, t0) < 0.5))) continue;
      if (cut && G.groove(x, y, z, SLOTS.SEAM)) {
        const [ix, iy] = G.inward(x, y);
        own(ix, iy, z);
        continue;
      }
      v[3] = SLOTS.SEAM;
      own(x, y, z);
    }
  }

  // ---- light strips -------------------------------------------------------------
  if (s.lights > 0.05 && role !== 'hand' && role !== 'head') {
    const dash = Math.round(3 + (1 - s.lights) * 6);
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!(free(v) || v[3] === A.CLOTH || v[3] === A.CLOTH_SHADE) ||
          !G.lateral(x, y, z)) continue;
      let on = false;
      if (role === 'trunk' && name === 'torso') {
        // Two lines down the chest from the collar, either side of the core.
        on = y + 0.5 < cy && Math.abs(Math.abs(x + 0.5 - cx) - sx * 0.3) < 0.55 &&
             z < sz * 0.86 && z > sz * 0.2;
      } else if (role === 'limb' || role === 'foot') {
        on = Math.min(tDist(x, y, 0), tDist(x, y, 0.5)) < 0.55 &&
             z > 1 && z < sz - 2;
      }
      if (on && (z % (dash + 2)) < dash) {
        v[3] = SLOTS.LIGHT;
        own(x, y, z);
      }
    }
  }

  // ---- circuit traces: manhattan runs through the shell -----------------------
  if (s.traces > 0.05 && role !== 'hand') {
    const byUZ = new Map();
    for (const v of G.list()) {
      if (!free(v) || !G.lateral(v[0], v[1], v[2])) continue;
      const u = Math.round(G.turn(v[0], v[1]) * perim);
      const k = u * 4096 + v[2];
      if (!byUZ.has(k)) byUZ.set(k, []);
      byUZ.get(k).push(v);
    }
    const keys = [...byUZ.keys()];
    const n = Math.floor(s.traces * keys.length / 120 + rnd(seed, 7));
    const P = Math.max(1, Math.round(perim));
    for (let i = 0; i < n && keys.length; i++) {
      let k = keys[Math.floor(rnd(seed, i, 11) * keys.length)];
      let u = Math.floor(k / 4096), z = k % 4096;
      let alongZ = rnd(seed, i, 13) < 0.5;
      const legs = 2 + Math.floor(rnd(seed, i, 17) * 3);
      let last = null;
      for (let l = 0; l < legs; l++) {
        const len = 2 + Math.floor(rnd(seed, i, l, 19) * 6);
        const dir = rnd(seed, i, l, 23) < 0.5 ? -1 : 1;
        for (let st = 0; st < len; st++) {
          const cell = byUZ.get(((u % P) + P) % P * 4096 + z);
          if (cell) for (const v of cell) if (free(v)) { v[3] = SLOTS.TRACE; last = v; }
          if (alongZ) z += dir; else u += dir;
        }
        alongZ = !alongZ;
      }
      if (last) last[3] = SLOTS.LIGHT;                // the pad at the end
    }
  }

  // ---- ear discs (head) ------------------------------------------------------
  if (role === 'head' && s.ears > 0.05) {
    const ez = (info.eyeRow ?? sz * 0.6) - 0.5;
    const r = 1.2 + s.ears * 1.8;
    const xs = G.list().map(v => v[0]);
    const xMin = Math.min(...xs), xMax = Math.max(...xs);
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (x > xMin + 1 && x < xMax - 1) continue;
      if (G.has(x + (x <= xMin + 1 ? -1 : 1), y, z)) continue;   // not the side face
      const d = Math.hypot(y + 0.5 - (cy + 0.5), z + 0.5 - ez);
      if (d < r * 0.5) paintOwn(x, y, z, SLOTS.FRAME);
      else if (d < r * 0.8) paintOwn(x, y, z, SLOTS.SEAM);
      else if (d < r) paintOwn(x, y, z, SLOTS.LIGHT);
    }
  }

  // ---- sheen: a specular streak down each panel -------------------------------
  if (s.sheen > 0.05) {
    // A light up and to the front-left: the cells whose outward direction
    // points at it most squarely take the highlight.
    const L = [-0.55, -0.83];
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!free(v) || !G.lateral(x, y, z)) continue;
      const ox = x + 0.5 - cx, oy = y + 0.5 - cy;
      const n = Math.hypot(ox, oy) || 1;
      const dot = (ox * L[0] + oy * L[1]) / n;
      if (dot > 1 - s.sheen * 0.12) v[3] = A.SKIN_LIGHT;
    }
  }

  // ---- fingertips and soles ----------------------------------------------------
  if (role === 'hand')
    for (const v of G.list()) if (v[2] <= 1 && SHELL.has(v[3])) v[3] = A.NAIL;
  if (role === 'foot')
    for (const v of G.list()) if (v[2] === 0 && SHELL.has(v[3])) v[3] = A.NAIL;

  const out = keepLargest(G.list(), size);
  if (face) info.face = face;
  return out;
}

/** The android's face. Returns what the extras need, head-local. */
function carveFace(g, A, G, info, seed, paintOwn, plateOf) {
  const s = g.android;
  const F = readFace(G, A, info, plateOf);
  const { cx, cy } = G;
  let front = F.front;
  const eyeZ = F.eyes.reduce((a, e) => a + e.z, 0) / F.eyes.length;
  const eyeHalf = Math.max(...F.eyes.map(e => Math.abs(e.x - cx)));

  // SMOOTH THE HUMAN AWAY: the nose and lips stand proud of the face plane;
  // below `human` 0.5 everything in front of the plane beside them goes.
  if (s.human < 0.5) {
    const z0 = Math.floor(F.mouthRow - 2), z1 = Math.ceil(F.eyeRow);
    for (let z = z0; z <= z1; z++) {
      const ref = [front(Math.floor(cx - eyeHalf - 1), z),
                   front(Math.ceil(cx + eyeHalf), z)].filter(v => v !== undefined);
      if (!ref.length) continue;
      const plane = Math.max(...ref);
      for (let x = Math.floor(cx - eyeHalf); x <= Math.ceil(cx + eyeHalf); x++)
        for (let y = front(x, z) ?? plane; y < plane; y++)
          if (G.has(x, y + 1, z)) G.del(x, y, z);
    }
    // ...and the front moved.
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y < m.get(k)) m.set(k, y);
    }
    front = (x, z) => m.get(x * 4096 + z);
  }

  const R = 0.7 + s.eyeSize * 0.8;
  const eyes = [];
  const halfW = eyeHalf + R + 1.5;
  const eyePaint = d => (d < 0.72 ? A.EYE : SLOTS.VISOR);
  if (s.faceplate > 0.05) {
    // THE FACEPLATE: an oval of dark glass, brow to chin, the eyes behind it.
    const rx = Math.min(halfW + s.faceplate, G.sx / 2 - 0.5);
    const top = eyeZ + R + 2, bot = F.mouthRow - 1.5;
    const pz = (top + bot) / 2, rz = (top - bot) / 2 + s.faceplate;
    disc(G, front, -1, cx, pz, rx, rz, 1,
         (d, a, x, z) => {
           if (s.eyeSize > 0.05)
             for (const e of (s.mono ? [{ x: cx, z: eyeZ }] : F.eyes))
               if (Math.hypot(x + 0.5 - e.x, z + 0.5 - e.z) < R * (s.mono ? 1.3 : 0.8))
                 return A.EYE;
           return SLOTS.VISOR;
         },
         () => SLOTS.SEAM, 0.8);
    for (const e of (s.mono ? [{ x: cx, z: eyeZ }] : F.eyes)) eyes.push({ ...e, r: R });
  } else if (s.visor > 0.05) {
    // THE VISOR: a dark band across the eyes, a lit slit through it.
    const hz = 0.8 + s.visor * 2.2;
    const hw = Math.min(halfW + 0.5, G.sx / 2 - 0.5);
    disc(G, front, -1, cx, eyeZ, hw, hz, 1,
         (d, a, x, z) => (Math.abs(z + 0.5 - eyeZ) < 0.6 && s.eyeSize > 0.05
           ? A.EYE : SLOTS.VISOR),
         () => SLOTS.SEAM, 0.7);
    eyes.push({ x: cx - eyeHalf, z: eyeZ, r: hz }, { x: cx + eyeHalf, z: eyeZ, r: hz });
  } else if (s.eyeSize > 0.05) {
    const list = s.mono ? [{ x: cx, z: eyeZ }] : F.eyes;
    for (const e of list) {
      const r = s.mono ? R * 1.4 : R;
      disc(G, front, -1, e.x, e.z, r, r, 1, eyePaint,
           s.mono ? () => SLOTS.LIGHT : () => SLOTS.SEAM, s.mono ? 0.9 : 0.6);
      eyes.push({ x: e.x, z: e.z, r });
    }
  } else {
    // FACELESS: a single lit point on the brow, nothing else.
    const z = Math.floor(eyeZ + 2), f = front(Math.floor(cx), z);
    if (f !== undefined) {
      paintOwn(Math.floor(cx), f, z, SLOTS.LIGHT);
      paintOwn(Math.floor(cx) - 1, front(Math.floor(cx) - 1, z) ?? f, z, SLOTS.LIGHT);
    }
    eyes.push({ x: cx, z: eyeZ, r: 1 });
  }
  const browZ = Math.max(...eyes.map(e => e.z + e.r)) + 1;

  // THE MOUTH: speaker slits and/or a lit bar.
  const mz = F.mouthRow + 0.5, mx = cx + F.mouthDx;
  if (s.grille > 0.05 && s.faceplate <= 0.05) {
    const k = 2 + Math.round(s.grille * 4);
    const h = 1 + Math.round(s.grille * 2);
    for (let i = 0; i < k; i++) {
      const x = Math.round(mx - k + 1 + i * 2);
      for (let z = Math.floor(mz) - h + 1; z <= Math.floor(mz); z++) {
        const f = front(x, z);
        if (f === undefined || !G.has(x, f + 1, z)) continue;
        G.del(x, f, z);
        paintOwn(x, f + 1, z, SLOTS.VISOR);
      }
    }
  }
  if (s.mouthLight > 0.05 && s.faceplate <= 0.05) {
    const hw = 1 + s.mouthLight * 2.5;
    const z = Math.floor(mz) - (s.grille > 0.05 ? 3 : 0);
    for (let x = Math.floor(mx - hw); x <= Math.ceil(mx + hw); x++) {
      const f = front(x, z);
      if (f !== undefined) paintOwn(x, f, z, SLOTS.LIGHT);
    }
  }

  // FACE SEAMS: round the face, across the brow, down each cheek.
  if (s.seams > 0.05) {
    const lines = [];
    const fw = Math.min(halfW + 1, G.sx / 2 - 1);
    const zTop = Math.floor(browZ + 1), zBot = Math.floor(F.neckTop + 2);
    for (let x = Math.floor(cx - fw); x <= Math.ceil(cx + fw); x++) lines.push([x, zTop]);
    if (s.seams > 0.3)
      for (let z = zBot; z <= zTop; z++) {
        lines.push([Math.floor(cx - fw), z], [Math.ceil(cx + fw) - 1, z]);
      }
    if (s.seams > 0.6 && s.faceplate <= 0.05 && s.visor <= 0.05)
      for (const side of [-1, 1])
        for (let z = Math.floor(F.mouthRow); z < Math.floor(eyeZ - R); z++)
          lines.push([Math.round(cx + side * (eyeHalf + 0.5)), z]);
    for (const [x, z] of lines) {
      const f = front(x, z);
      if (f === undefined) continue;
      const v = G.get(x, f, z);
      if (!v || v[3] === A.EYE || v[3] === SLOTS.VISOR || v[3] === SLOTS.LIGHT) continue;
      if (G.has(x, f + 1, z) && s.seams > 0.5) {
        G.del(x, f, z);
        paintOwn(x, f + 1, z, SLOTS.SEAM);
      } else paintOwn(x, f, z, SLOTS.SEAM);
    }
  }

  return { eyes, browZ, chinZ: F.neckTop + 1, halfW, mouth: { x: mx, z: mz },
           frontOf: front };
}

// =============================================================================
// the extras: antennae, head fins, power packs, halo. SCENE coordinates (x
// right, y BACK, z up); `head` cells ride the head, `torso` cells the torso.
// =============================================================================

export function extrasWant(g) {
  const s = g.android;
  return s.antenna > 0.02 || s.fins > 0.02 || s.packs > 0.02 || s.halo > 0.02;
}

export function extras(g, ctx) {
  const s = g.android;
  const { K, body, head, torso } = ctx;
  const A = ctx.ART;
  const headOut = new Map(), torsoOut = new Map();
  const free = (X, Y, Z) => !body.has(K(X, Y, Z)) && !headOut.has(K(X, Y, Z)) &&
                            !torsoOut.has(K(X, Y, Z));
  const addH = (X, Y, Z, c) => {
    if (free(X, Y, Z)) headOut.set(K(X, Y, Z), [X, Y, Z, c]);
  };
  const addT = (X, Y, Z, c) => {
    if (free(X, Y, Z)) torsoOut.set(K(X, Y, Z), [X, Y, Z, c]);
  };
  const H = extent(head);
  const hc = sceneCells(head);
  const topAt = new Map();
  for (const [x, y, z] of hc) {
    const k = x * 4096 + y;
    if (!topAt.has(k) || z > topAt.get(k)) topAt.set(k, z);
  }
  const top = (x, y) => topAt.get(Math.floor(x) * 4096 + Math.floor(y));
  const halfW = (H.x1 - H.x0 + 1) / 2, halfD = (H.y1 - H.y0 + 1) / 2;
  const crownZ = top(H.cx, H.cy) ?? H.z1;

  // ---- antennae: a rod off the crown, a lit bead at the tip ------------------
  if (s.antenna > 0.02) {
    const n = s.antenna > 0.5 ? 2 : 1;
    const len = Math.round(3 + s.antenna * 10);
    for (let i = 0; i < n; i++) {
      const side = n === 1 ? 1 : (i === 0 ? -1 : 1);
      const bx = Math.floor(H.cx + side * halfW * 0.55);
      const by = Math.floor(H.cy + 1);
      const bz = top(bx, by);
      if (bz === undefined) continue;
      const tip = [bx + side * Math.round(len * 0.25), by + Math.round(len * 0.15),
                   bz + len];
      addH(bx, by, bz + 1, SLOTS.FRAME);
      line6([bx, by, bz + 1], tip, (x, y, z) => addH(x, y, z, SLOTS.FRAME));
      for (let dz = 0; dz <= 1; dz++)
        for (let dy = 0; dy <= 1; dy++)
          for (let dx = 0; dx <= 1; dx++)
            addH(tip[0] + dx - (side < 0 ? 1 : 0), tip[1] + dy, tip[2] + 1 + dz,
                 SLOTS.LIGHT);
    }
  }

  // ---- head fins: swept back from above each ear, lit along the edge ---------
  if (s.fins > 0.02) {
    // ONE FLAT BLADE A SIDE, in the plane just outside the head's widest
    // column over the fin's rows, so each fin is one piece that touches the
    // head where the head is widest; swept back past the skull.
    const z0 = Math.round(H.z0 + (H.z1 - H.z0) * 0.5);
    const z1 = Math.round(H.z1 - 1);
    let xMin = 1e9, xMax = -1e9;
    for (const [x, , z] of hc)
      if (z >= z0 && z <= z1) { xMin = Math.min(xMin, x); xMax = Math.max(xMax, x); }
    const sweep = 2 + s.fins * 9;
    const thick = s.fins > 0.5 ? 2 : 1;
    for (const dir of [-1, 1]) {
      const x0 = dir < 0 ? xMin - 1 : xMax + 1;
      for (let z = z0; z <= z1; z++) {
        const t = (z - z0) / Math.max(1, z1 - z0);
        const y0 = Math.round(H.cy - halfD * 0.35 + t * halfD * 0.5);
        const y1 = Math.round(H.y1 + sweep * (0.25 + t * 0.75));
        for (let y = y0; y <= y1; y++) {
          const edge = y === y1 || z === z1;
          for (let k = 0; k < thick; k++)
            addH(x0 + dir * k, y, z, edge ? SLOTS.LIGHT
                                          : k ? A.SKIN_SHADE : A.SKIN_BASE);
        }
      }
    }
  }

  // ---- the halo: a lit ring floating over the crown ---------------------------
  if (s.halo > 0.02) {
    const R = Math.max(halfW, halfD) * (0.75 + s.halo * 0.35);
    const z = crownZ + 3 + Math.round(s.halo * 2);
    // THE EMITTER: a thin mast up the back of the skull that holds the ring
    // (a ring floating free would be a part touching nothing).
    {
      const my = Math.floor(H.cy + R);
      const mx = Math.floor(H.cx);
      let mz = null;
      for (let y = Math.floor(H.cy); y <= H.y1; y++) {
        const t = top(mx, y);
        if (t !== undefined) mz = { y, z: t };
      }
      if (mz) line6([mx, mz.y, mz.z + 1], [mx, my, z],
                    (x, y, zz) => { addH(x, y, zz, SLOTS.FRAME);
                                    addH(x + 1, y, zz, SLOTS.FRAME); });
      if (mz) { addH(mx, mz.y, mz.z + 1, SLOTS.FRAME);
                addH(mx + 1, mz.y, mz.z + 1, SLOTS.FRAME); }
    }
    for (let y = Math.floor(H.cy - R - 2); y <= H.cy + R + 2; y++)
      for (let x = Math.floor(H.cx - R - 2); x <= H.cx + R + 2; x++) {
        const d = Math.hypot(x + 0.5 - H.cx, y + 0.5 - H.cy);
        if (d <= R + 0.6 && d >= R - 0.8) {
          addH(x, y, z, SLOTS.LIGHT);
          addH(x, y, z + 1, SLOTS.LIGHT);
        }
      }
  }

  // ---- power packs on the back ------------------------------------------------
  if (s.packs > 0.02) {
    const T = extent(torso);
    const backAt = new Map();
    for (const [x, y, z] of sceneCells(torso)) {
      const k = x * 4096 + z;
      if (!backAt.has(k) || y > backAt.get(k)) backAt.set(k, y);
    }
    const tH = T.z1 - T.z0 + 1;
    const r = 1.4 + s.packs * 1.2;
    const zA = Math.round(T.z0 + tH * 0.4), zB = Math.round(T.z0 + tH * (0.8 + s.packs * 0.1));
    for (const dir of [-1, 1]) {
      const px = T.cx + dir * (T.x1 - T.x0 + 1) * 0.2;
      let yb = -1e9;
      for (let z = zA; z <= zB; z++) {
        const b = backAt.get(Math.floor(px) * 4096 + z);
        if (b !== undefined) yb = Math.max(yb, b);
      }
      if (yb < -1e8) continue;
      const py = yb + 1 + r;
      cylinder(px, py, zA, zB, r, (x, y, z, d) => {
        addT(x, y, z, z === zA ? SLOTS.LIGHT : z === zB ? SLOTS.SEAM
                      : (z - zA) % 5 === 4 ? SLOTS.SEAM : A.SKIN_BASE);
      });
      // The strap that bolts it to the back: a bar from pack to shell.
      for (let z = zA + 1; z < zB; z += Math.max(3, Math.round((zB - zA) / 2))) {
        line6([Math.floor(px), Math.floor(py), z], [Math.floor(px), yb, z],
              (x, y, zz) => addT(x, y, zz, SLOTS.FRAME));
      }
    }
  }

  return { head: [...headOut.values()], torso: [...torsoOut.values()] };
}
