/* goblin.js — THE GOBLIN RACE: small, quick, ugly people on the human rig.
 *
 * WHAT A GOBLIN IS, TO THE GENERATOR. The human body -- the SAME fifteen limb
 * boxes, anchors, sockets, chains and clips, so every helm and blade fits and
 * every system that binds by limb name binds -- made SMALLER and wrong in the
 * goblin way: a big head on short legs and long arms (BUILD_DELTA, offsets as
 * applySex uses, so a round trip through human lands where it started), a
 * green / grey / ruddy hide, and what sticks out of the head.
 *
 * WHAT STICKS OUT is the race's identity and is built as head EXTRAS (scene
 * coordinates, past the head's box, the demon horns' mechanism):
 *   - EARS, two flat blades off the sides of the skull. Length, width, rise,
 *     sweep back, a flop at the tip, how pointed, torn notches, lynx tufts,
 *     and iron rings up the lower edge. Six starting shapes (EAR_STYLES).
 *   - A NOSE swept forward off the bridge: length, hook (a beak down, a snout
 *     up), a bulb at the tip, a crooked bend, warts, and a ring, a bone or a
 *     stud through it. Six starting shapes (NOSE_STYLES).
 *   - TUSKS up from the lower jaw, set close or wide.
 *   - A HAT: a redcap (the folklore one, a floppy point with a bobble), a
 *     mushroom cap worn as a hat, an upturned cooking pot, or a leather band
 *     ringed with bone spikes. Size, a cocked tilt, a flop. Hair under a hat
 *     is flattened (the mass cells inside it are removed); hair below the
 *     brim still shows.
 * WHAT IS PAINTED (the surface pass, inside each part's own box): eyes from
 * beady to huge cave eyes, slit or squinting, that may give light; teeth --
 * a snaggle, fangs, a toothy grin, a saw-edge; warts (raised where the box has
 * room) and darker blotches; war paint on the face and, optionally, the body;
 * and the human's nails carved into claws (demon.js talons).
 *
 * A KIND (forest, cave, bog, hill, redcap, hob, frost, ash) is a picker that
 * sets the ears, the nose, the teeth, the eyes and the colours together.
 *
 * IT IS FLESH, like the demon and the naga: the human's anatomy recipe
 * (ANATOMY = null), blood, hair. Only tusks, teeth, claws and bone hats name
 * `bone`; rings and pots `iron`; a redcap is `cloth`, a mushroom hat really is
 * mushroom (`cap_red` / `mushroom_gill`, so it burns like one); lit eyes are
 * `gem_arcane`. Every id is <= 127 -- no stand-ins.
 *
 * HUMANS ARE UNTOUCHED: every entry point is reached only for
 * `genome.body.race === 'goblin'`, and every gene here is race-gated (no human
 * roll, mutation or cross takes a draw for it). PURE AND NODE-RUNNABLE;
 * imports robotkit.js and demon.js (the talons).
 */

'use strict';

import { rnd, strHash, clamp, grid, vnoise, keepLargest, sceneCells,
         extent } from './robotkit.js';
import { talons } from './demon.js';
import { HATS, buildHat } from './goblinhats.js';

export const RACE = 'goblin';
export const FOLDER = 'goblin';
export const NEW_NAME = 'goblin';
/** Not a machine (test_mobgen section P is the machines'). */
export const MACHINE = false;
export const BLURB = 'a goblin: small and quick, a big head on short legs ' +
  'and long arms, in a green, grey or ruddy hide -- great ears, a hooked or ' +
  'bulbous nose, tusks or a snaggle tooth, warts, war paint, and a redcap, a ' +
  'mushroom or a cooking pot for a hat. Flesh like a human on the same limbs, ' +
  'so every helm and blade fits.';

/** The race's own height band (metres). 0.9 keeps a margin over the smallest
 *  figure validateMob passes (demon.js's imp is 0.85); 1.6 is a big
 *  hobgoblin, still under a man. */
export const HEIGHT_BAND = [0.9, 1.6];

// =============================================================================
// art slots and materials
// =============================================================================

/** The goblin's own slots, below the serpent's (209). */
export const SLOTS = {
  WART: 208,       // warts, raised or painted
  TUSK: 207,       // tusks, teeth, bone spikes, a bone through the nose
  PAINT: 206,      // war paint
  RING: 205,       // ear rings, a nose ring, a stud, the cooking pot
  HAT: 204,        // a redcap, a mushroom cap, a bone crown's band
  HAT_TRIM: 203,   // a redcap's band and bobble, a mushroom's spots and gills
  PUPIL: 202,      // pupils, nostrils
};
export const COLOR_SLOTS = {
  wart: SLOTS.WART, tusk: SLOTS.TUSK, warpaint: SLOTS.PAINT, ring: SLOTS.RING,
  hat: SLOTS.HAT, hatTrim: SLOTS.HAT_TRIM, eyeDark: SLOTS.PUPIL,
};

/** What each hat is made of: its body (HAT) and its trim (HAT_TRIM). A pot
 *  is drawn in RING (iron) and a skull in TUSK (bone); their HAT is a
 *  saucepan's nothing and a skull's horns. */
export const HAT_MATS = {
  none:      { hat: 'cloth', trim: 'cloth' },
  redcap:    { hat: 'cloth', trim: 'cloth' },
  hood:      { hat: 'cloth', trim: 'leather' },
  mushroom:  { hat: 'cap_red', trim: 'mushroom_gill' },
  morel:     { hat: 'cap_tan', trim: 'mushroom_stem' },
  pot:       { hat: 'iron', trim: 'iron' },
  bonecrown: { hat: 'leather', trim: 'leather' },
  skull:     { hat: 'bone', trim: 'bone' },
  tophat:    { hat: 'cloth', trim: 'cloth' },
  witch:     { hat: 'cloth', trim: 'leather' },
};
export const MATS = { tusk: 'bone', ring: 'iron', glow: 'gem_arcane' };
/** null: every slot not named below keeps the human's material (skin). */
export const DEFAULT_MAT = null;
export const STAND_INS = {};
export const BLEED = { material: 'blood', perDamage: 2.5 };
/** null: the human's recipe. A goblin is flesh. */
export const ANATOMY = null;
export const ANATOMY_NOTES = {};

export function slotMaterialNames(g, ART) {
  const s = g.goblin || {};
  const m = {};
  // The hide is SKIN wherever it is painted -- including the ears and the
  // nose, which are extras (hair-mass parts, hair_white unless a slot says).
  for (const sl of [ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT, ART.LIP,
                    SLOTS.WART, SLOTS.PAINT, SLOTS.PUPIL])
    m[sl] = 'skin';
  // Claws are bone; with none, the nails are the human's (skin).
  m[ART.NAIL] = s.claws > 0.05 ? MATS.tusk : 'skin';
  m[SLOTS.TUSK] = MATS.tusk;
  m[SLOTS.RING] = MATS.ring;
  const hm = HAT_MATS[s.hat] || HAT_MATS.none;
  m[SLOTS.HAT] = hm.hat;
  m[SLOTS.HAT_TRIM] = hm.trim;
  if (s.glow) m[ART.EYE] = MATS.glow;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export const EYE_STYLES = ['beady', 'big', 'huge', 'slit', 'squint'];
export const TEETH = ['none', 'snaggle', 'fangs', 'grin', 'saw'];
export const WARPAINT = ['none', 'stripes', 'mask', 'skull', 'hand', 'dots', 'split'];
export const PIERCINGS = ['none', 'ring', 'bone', 'stud'];
export { HATS };
export const POSTURES = ['upright', 'hunched'];

export function defaultGenes() {
  return {
    kind: 'forest',      // a KIND (KINDS): ears, nose, teeth, eyes, colours
    // ---- ears (EAR_STYLES sets these; the numbers are the truth)
    ears: 'leaf',
    earLength: 0.6,      // how far they reach off the skull
    earWidth: 0.45,      // how broad the blade is
    earRise: 0.25,       // -1 hangs down .. 1 stands straight up
    earSweep: 0.35,      // 0 straight out to the sides .. 1 swept back
    earFlop: 0.05,       // how far the tip bends down
    earPoint: 0.75,      // round (0) .. a needle point (1)
    earNotch: 0,         // torn notches out of the edges
    earTuft: 0,          // a lynx tuft of hair at each tip
    earRings: 0,         // iron rings up the lower edge, 0..4
    // ---- nose (NOSE_STYLES)
    nose: 'hook',
    noseLength: 0.5,
    noseHook: 0.6,       // -1 turns up (a snout) .. 1 a beak bent down
    noseBulb: 0.15,      // a round bulb at the tip
    noseBend: 0,         // -1..1 crooked to one side (an old break)
    nosePierce: 'none',
    // ---- mouth
    teeth: 'snaggle',
    tusks: 0,            // tusks up from the lower jaw
    tuskSpread: 0.5,     // set close (0) .. out at the corners (1)
    // ---- eyes
    eyes: 'big',
    glow: false,
    // ---- hide
    warts: 0.2,
    blotch: 0.2,         // darker patches over the hide
    warpaint: 'none',
    bodyPaint: 0,        // how much of the war paint goes on the body too
    claws: 0.4,          // the nails, carved into claws
    posture: 'upright',
    // ---- hat
    hat: 'none',
    hatSize: 0.5,        // spread / brim / snout (goblinhats.js, per hat)
    hatHeight: 0.5,      // how tall or long its main part runs
    hatTilt: 0,          // -1..1 cocked, or flopping, to one side
    hatFlop: 0.5,        // how far it bends, droops, curls or is crushed
    hatDetail: 0.5,      // its extra: bobble, warts, handle, spikes, feather
    hatWear: 0,          // patches and holes, or dents and chips
  };
}

const EAR_KEYS = ['earLength', 'earWidth', 'earRise', 'earSweep', 'earFlop',
                  'earPoint'];
export const EAR_STYLES = {
  leaf:  { earLength: 0.6,  earWidth: 0.45, earRise: 0.25,  earSweep: 0.35, earFlop: 0.05, earPoint: 0.75 },
  bat:   { earLength: 0.6,  earWidth: 0.9,  earRise: 0.55,  earSweep: 0.15, earFlop: 0,    earPoint: 0.5 },
  droop: { earLength: 0.75, earWidth: 0.5,  earRise: -0.1,  earSweep: 0.3,  earFlop: 0.8,  earPoint: 0.45 },
  swept: { earLength: 0.8,  earWidth: 0.35, earRise: 0.15,  earSweep: 0.85, earFlop: 0,    earPoint: 0.85 },
  jug:   { earLength: 0.3,  earWidth: 0.85, earRise: 0.1,   earSweep: 0.05, earFlop: 0,    earPoint: 0.1 },
  spike: { earLength: 1,    earWidth: 0.3,  earRise: 0.45,  earSweep: 0.5,  earFlop: 0,    earPoint: 1 },
};
export const EAR_ORDER = Object.keys(EAR_STYLES);

export function applyEars(genome, style) {
  const f = EAR_STYLES[style];
  if (!f) return genome;
  genome.goblin.ears = style;
  for (const k of EAR_KEYS) genome.goblin[k] = f[k];
  return genome;
}

const NOSE_KEYS = ['noseLength', 'noseHook', 'noseBulb'];
export const NOSE_STYLES = {
  hook:   { noseLength: 0.5,  noseHook: 0.6,  noseBulb: 0.15 },
  long:   { noseLength: 0.95, noseHook: 0.1,  noseBulb: 0 },
  bulb:   { noseLength: 0.3,  noseHook: 0.2,  noseBulb: 0.9 },
  snub:   { noseLength: 0.15, noseHook: -0.8, noseBulb: 0.35 },
  beak:   { noseLength: 0.7,  noseHook: 1,    noseBulb: 0 },
  button: { noseLength: 0.05, noseHook: 0,    noseBulb: 0.5 },
};
export const NOSE_ORDER = Object.keys(NOSE_STYLES);

export function applyNose(genome, style) {
  const f = NOSE_STYLES[style];
  if (!f) return genome;
  genome.goblin.nose = style;
  for (const k of NOSE_KEYS) genome.goblin[k] = f[k];
  return genome;
}

// ---- colours: sets a roll picks WHOLE ----------------------------------------
// THE ENGINE HAS ONE 255-COLOUR ART PALETTE for every loaded body, and it was
// already full before the goblins came (microbody.cpp: an overflowing colour
// takes the NEAREST merged one). Goblins load early (alphabetically), so every
// fresh colour here pushed a later body -- a naga, a shark -- onto a nearest
// match. So nearly every hex below is one another body ALREADY paints (an
// exact match shares its slot): the colours were authored, then snapped to
// the nearest shipped colour within 20 RGB steps, three hides were taken whole
// from the naga and the demon, and every pupil is the demon's black. Only the
// moss hide is new: it is the goblin. Re-snap after authoring a new set, or it costs slots.
/** The hide: the skin family, its claws and its warts, picked together. */
export const SKINS = {
  moss:  { skin: '#6f8a3a', skinShade: '#4e6428', skinLight: '#8ea852', nail: '#8d7a48', wart: '#584a26' },
  olive: { skin: '#7c7746', skinShade: '#5c5a2a', skinLight: '#9a9a58', nail: '#7a6a3a', wart: '#4a3a22' },
  bog:   { skin: '#5a6a44', skinShade: '#414e31', skinLight: '#647e5a', nail: '#6a5a3e', wart: '#7a6a3a' },
  // The naga's jade, whole: a cave goblin's sunless pale green.
  pale:  { skin: '#8aa880', skinShade: '#647e5a', skinLight: '#a8c49e', nail: '#5a7a50', wart: '#8e8a83' },
  ruddy: { skin: '#9a633f', skinShade: '#6a4424', skinLight: '#b17957', nail: '#5b361f', wart: '#7d4e2e' },
  ochre: { skin: '#b07838', skinShade: '#86562f', skinLight: '#ca8d5d', nail: '#6a4424', wart: '#5b361f' },
  // The naga's cobalt, whole.
  frost: { skin: '#6a84a8', skinShade: '#4a6080', skinLight: '#8aa2c4', nail: '#40587a', wart: '#5d6e7c' },
  // The demon's soot hide, whole.
  soot:  { skin: '#2e2826', skinShade: '#1e1a18', skinLight: '#4a4240', nail: '#100e0e', wart: '#4d4d49' },
};
/** Irises and pupils. Red and ice are the naga's own hexes, so the shared
 *  palette spends nothing on them. */
export const EYES = {
  yellow: { eye: '#e8b830', eyeDark: '#1a1210' },
  amber:  { eye: '#e07a2a', eyeDark: '#1a1210' },
  red:    { eye: '#e83a2a', eyeDark: '#1a1210' },
  green:  { eye: '#a6c94a', eyeDark: '#1a1210' },
  ice:    { eye: '#a8e0f0', eyeDark: '#1a1210' },
  white:  { eye: '#e6e6d9', eyeDark: '#1a1210' },
};
/** Metal (rings, the pot) and the tusks' bone, picked together. The tusk
 *  hexes are the demon's horn colours. */
export const METALS = {
  iron:   { ring: '#6a6a66', tusk: '#e2d6b2' },
  brass:  { ring: '#a8823f', tusk: '#d8ccb0' },
  copper: { ring: '#b8673a', tusk: '#e6dfd0' },
};
export const HAT_COLORS = {
  red:   { hat: '#a82a2a', hatTrim: '#e6d8b4' },
  brown: { hat: '#6b4a2e', hatTrim: '#cfc39a' },
  moss:  { hat: '#445a26', hatTrim: '#cfc39a' },
  slate: { hat: '#37506b', hatTrim: '#d8ccb0' },
  black: { hat: '#1a1210', hatTrim: '#922218' },
  tan:   { hat: '#5c4f39', hatTrim: '#b07838' },
};
export const PAINTS = {
  white: { warpaint: '#e6e6d9' },
  ochre: { warpaint: '#c89a38' },
  blood: { warpaint: '#922218' },
  woad:  { warpaint: '#4e6a96' },
  black: { warpaint: '#1a1414' },
};
/** The loincloth (the human's shorts) -- the demon's and the human's rags. */
export const RAGS = {
  hide:  { cloth: '#3a2a20', clothShade: '#261a12' },
  sack:  { cloth: '#5c4f39', clothShade: '#433a2a' },
  moss:  { cloth: '#5a6a44', clothShade: '#414e31' },
  umber: { cloth: '#6b4f33', clothShade: '#4d3824' },
};
export const HAIRS = {
  black:  { hair: '#1a1210', hairShade: '#0e0a08' },
  grey:   { hair: '#787773', hairShade: '#5a5650' },
  ginger: { hair: '#7e4424', hairShade: '#52321e' },
  white:  { hair: '#d8d3cb', hairShade: '#a8a39b' },
};
export const DEFAULT_COLORS = {
  ...SKINS.moss, ...EYES.yellow, ...METALS.iron, ...HAT_COLORS.red,
  ...PAINTS.white, ...RAGS.sack, ...HAIRS.black,
};
export const GLOWS = Object.fromEntries(Object.entries(EYES).map(([k, v]) => [k, v.eye]));
export const COLOR_SETS = [SKINS, EYES, METALS, HAT_COLORS, PAINTS, RAGS, HAIRS];
function lum(hex) {
  const n = parseInt(hex.slice(1), 16);
  return ((n >> 16) & 255) * 0.3 + ((n >> 8) & 255) * 0.59 + (n & 255) * 0.11;
}
export const STOCK = Object.entries(SKINS).sort((a, b) => lum(a[1].skin) - lum(b[1].skin));
export const STOCK_LABEL = 'hide';

/** THE KINDS: ears, nose, teeth, eyes and colours, picked together. Every key
 *  named is set; the rest are left. `ears` / `nose` name a style. */
export const KINDS = {
  forest: { ears: 'leaf', nose: 'hook', goblin: { teeth: 'snaggle', eyes: 'big', tusks: 0, warts: 0.15 },
            colors: { ...SKINS.moss, ...EYES.yellow } },
  cave:   { ears: 'bat', nose: 'snub', goblin: { teeth: 'fangs', eyes: 'huge', tusks: 0, warts: 0.1, glow: true },
            colors: { ...SKINS.pale, ...EYES.white } },
  bog:    { ears: 'droop', nose: 'bulb', goblin: { teeth: 'saw', eyes: 'squint', tusks: 0, warts: 0.75 },
            colors: { ...SKINS.bog, ...EYES.green } },
  hill:   { ears: 'jug', nose: 'long', goblin: { teeth: 'none', eyes: 'beady', tusks: 0.6, warts: 0.25 },
            colors: { ...SKINS.olive, ...EYES.amber } },
  redcap: { ears: 'spike', nose: 'beak', goblin: { teeth: 'saw', eyes: 'beady', tusks: 0, warts: 0.2, hat: 'redcap' },
            colors: { ...SKINS.ruddy, ...EYES.red, ...HAT_COLORS.red } },
  hob:    { ears: 'swept', nose: 'hook', goblin: { teeth: 'none', eyes: 'slit', tusks: 0.45, warts: 0.1 },
            colors: { ...SKINS.ochre, ...EYES.amber } },
  frost:  { ears: 'spike', nose: 'long', goblin: { teeth: 'fangs', eyes: 'big', tusks: 0, warts: 0.05 },
            colors: { ...SKINS.frost, ...EYES.ice } },
  ash:    { ears: 'bat', nose: 'hook', goblin: { teeth: 'grin', eyes: 'slit', tusks: 0.2, warts: 0.3, glow: true },
            colors: { ...SKINS.soot, ...EYES.amber } },
};
export const KIND_ORDER = Object.keys(KINDS);

export function applyKind(genome, kind) {
  const k = KINDS[kind];
  if (!k) return genome;
  genome.goblin.kind = kind;
  applyEars(genome, k.ears);
  applyNose(genome, k.nose);
  Object.assign(genome.goblin, k.goblin);
  Object.assign(genome.colors, k.colors);
  return genome;
}

export const PICKERS = [
  { key: 'kind', path: 'goblin.kind', label: 'kind', order: KIND_ORDER,
    apply: applyKind,
    title: 'a kind of goblin: sets the ears, the nose, the teeth, the eyes ' +
           'and the colours together' },
  { key: 'ears', path: 'goblin.ears', label: 'ears', order: EAR_ORDER,
    apply: applyEars,
    title: 'a starting pair of ears: leaf, bat, droop, swept, jug, spike' },
  { key: 'nose', path: 'goblin.nose', label: 'nose', order: NOSE_ORDER,
    apply: applyNose,
    title: 'a starting nose: hook, long, bulb, snub, beak, button' },
];

/** Within-box build offsets, made on the way in and undone on the way out
 *  (mobgen applyRace): a goblin is a head shorter than the human it was, with
 *  a bigger, wider head, short legs, long arms and a fuller belly. */
export const BUILD_DELTA = {
  'body.heightM': -0.45,
  'body.headWidth': 2,
  // The eye row is DERIVED from the height (mobgen limbTable: the first
  // person camera's contract), so a taller skull puts the eyes nearer its
  // crown and the hairline over them. The big head is mostly WIDTH, and the
  // eyes come down the row the skull's extra height took.
  'body.stack.head': 1.5,
  'face.eyeRow': -1,
  // ...and a taller foot: under a metre the stock foot row is too thin to
  // hold its own ankle anchor (validateMob; demon.js's imp sets 6).
  'body.stack.foot': 1,
  'body.stack.shin': -1.5,
  'body.stack.thigh': -1.5,
  'body.armStack.fore': 1,
  'shape.waist': 0.06,
  'shape.legGirth': -0.08,
  'head.jaw': 0.1,
};

/** A goblin's face: no beard, a heavy brow. (Its lips stay the human's lit
 *  tone of the hide: a lip colour of its own would spend a palette slot.) */
export function onBecome(genome, h) {
  if (!h.isLocked('face.beardStyle')) h.applyBeardStyle(genome, 'clean');
  if (!h.isLocked('face.browProud')) genome.face.browProud = true;
  // The human's painted ear and nose would sit under the goblin's own.
  if (!h.isLocked('face.earPoint')) genome.face.earPoint = false;
  if (!h.isLocked('face.nose')) genome.face.nose = 0;
}

export function randomize(g, r, isLocked) {
  if (!isLocked('goblin.kind')) applyKind(g, r.pick(KIND_ORDER));
  if (!isLocked('goblin.ears') && r() < 0.35) applyEars(g, r.pick(EAR_ORDER));
  if (!isLocked('goblin.nose') && r() < 0.35) applyNose(g, r.pick(NOSE_ORDER));
  for (const spec of GENE_SPECS) {
    if (spec.kind || isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    let v = g.goblin[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    if (spec.int) v = Math.round(v);
    g.goblin[k] = clamp(v, spec.min, spec.max);
  }
  if (!isLocked('goblin.eyes') && r() < 0.3) g.goblin.eyes = r.pick(EYE_STYLES);
  if (!isLocked('goblin.teeth') && r() < 0.3) g.goblin.teeth = r.pick(TEETH);
  if (!isLocked('goblin.glow')) g.goblin.glow = g.goblin.glow ? r() < 0.8 : r() < 0.1;
  if (!isLocked('goblin.warpaint')) g.goblin.warpaint = r() < 0.55 ? 'none' : r.pick(WARPAINT);
  if (!isLocked('goblin.nosePierce')) g.goblin.nosePierce = r() < 0.75 ? 'none' : r.pick(PIERCINGS);
  if (!isLocked('goblin.hat') && g.goblin.hat === 'none')
    g.goblin.hat = r() < 0.6 ? 'none' : r.pick(HATS);
  if (!isLocked('goblin.posture')) g.goblin.posture = r() < 0.35 ? 'hunched' : 'upright';
  return g;
}

export const GENE_GROUPS = [
  { key: 'gbears', title: 'goblin: ears', race: 'goblin',
    note: 'Two blades off the sides of the skull. Pick a shape, then drag: ' +
          'how long, how broad, up or down, out or swept back, a flop at ' +
          'the tip, torn notches, tufts, and rings up the lower edge.' },
  { key: 'gbface', title: 'goblin: nose, teeth, eyes', race: 'goblin',
    note: 'Pick a kind to set ears, nose, teeth, eyes and colours together. ' +
          'The nose sweeps out of the bridge between the eyes; tusks rise ' +
          'from the lower jaw; teeth are painted on the mouth line.' },
  { key: 'gbhide', title: 'goblin: hide, paint, posture', race: 'goblin',
    note: 'Warts (raised where there is room) and darker blotches, war paint ' +
          'on the face and the body, claws carved from the nails, and an ' +
          'upright or hunched carriage (a held pose under the walk).' },
  { key: 'gbhat', title: 'goblin: hat', race: 'goblin',
    note: 'A redcap, a mushroom cap, an upturned cooking pot or a band of ' +
          'bone spikes. Hair under a hat is flattened; below the brim it ' +
          'still shows.' },
];

const N = (path, label, group, hint, extra = {}) => ({
  path: 'goblin.' + path, label, group, min: 0, max: 1, step: 0.05,
  sigma: 0.2, race: 'goblin', hint, ...extra });
const E = (path, label, group, choices, hint) => ({
  path: 'goblin.' + path, label, group, kind: 'enum', choices, sigma: 0.15,
  race: 'goblin', hint });

export const GENE_SPECS = [
  E('kind', 'kind', 'gbface', KIND_ORDER, 'A kind of goblin: forest, cave ' +
    '(huge lit eyes, bat ears), bog (warty, droop-eared), hill (tusked), ' +
    'redcap, hob, frost, ash.'),
  // ---- ears
  E('ears', 'ear shape', 'gbears', EAR_ORDER, 'A starting pair: leaf, bat, ' +
    'droop, swept, jug, spike.'),
  N('earLength', 'ear length', 'gbears', 'How far the ears reach off the ' +
    'skull: a stub at 0, longer than the head is wide at 1.'),
  N('earWidth', 'ear width', 'gbears', 'How broad the blade is.'),
  N('earRise', 'ear rise', 'gbears', 'Hanging down at -1, straight out at 0, ' +
    'standing up at 1.', { min: -1, max: 1 }),
  N('earSweep', 'ear sweep', 'gbears', 'Straight out to the sides at 0, swept ' +
    'back along the head at 1.'),
  N('earFlop', 'ear flop', 'gbears', 'How far the tip bends down under its ' +
    'own weight.'),
  N('earPoint', 'ear point', 'gbears', 'A round ear at 0, a needle point at 1.'),
  N('earNotch', 'torn ears', 'gbears', 'Notches bitten out of the edges. ' +
    '0 is whole.', { roll: 0.3 }),
  N('earTuft', 'ear tufts', 'gbears', 'A tuft of hair at each tip, in the ' +
    'hair colour.', { roll: 0.2 }),
  N('earRings', 'ear rings', 'gbears', 'Rings up the lower edge of each ear, ' +
    'in the metal colour.', { max: 4, step: 1, int: true, sigma: 0.8, roll: 0.3 }),
  // ---- nose
  E('nose', 'nose shape', 'gbface', NOSE_ORDER, 'A starting nose: hook, long, ' +
    'bulb, snub, beak, button.'),
  N('noseLength', 'nose length', 'gbface', 'How far the nose sweeps out of ' +
    'the face.'),
  N('noseHook', 'nose hook', 'gbface', 'Turned up into a snout at -1, ' +
    'straight at 0, bent down into a beak at 1.', { min: -1, max: 1 }),
  N('noseBulb', 'nose bulb', 'gbface', 'A round bulb at the tip.'),
  N('noseBend', 'crooked nose', 'gbface', 'Bent to one side, an old break: ' +
    'left below 0, right above.', { min: -1, max: 1, roll: 0.3 }),
  E('nosePierce', 'nose piercing', 'gbface', PIERCINGS, 'A ring through the ' +
    'tip, a bone through the bridge, or a stud in the side.'),
  // ---- mouth and eyes
  E('teeth', 'teeth', 'gbface', TEETH, 'On the mouth line: a crooked snaggle ' +
    'tooth, two fangs, a toothy grin, a saw-edge, or none showing.'),
  N('tusks', 'tusks', 'gbface', 'Tusks up from the lower jaw, past the lip. ' +
    '0 is none; they bite.'),
  N('tuskSpread', 'tusk spread', 'gbface', 'Set close under the nose at 0, ' +
    'out at the corners of the mouth at 1.'),
  E('eyes', 'eyes', 'gbface', EYE_STYLES, 'beady: all pupil. big: an iris ' +
    'round the pupil. huge: cave eyes. slit: a cat\'s pupil. squint: one ' +
    'eye half shut.'),
  { path: 'goblin.glow', label: 'lit eyes', group: 'gbface', kind: 'bool',
    sigma: 0, race: 'goblin',
    hint: 'The irises give light in the eye colour, like a cave goblin\'s.' },
  // ---- hide
  N('warts', 'warts', 'gbhide', 'Warts over the hide and the nose; past ' +
    'about 0.4 they stand proud where there is room.'),
  N('blotch', 'blotches', 'gbhide', 'Darker patches over the hide.'),
  E('warpaint', 'war paint', 'gbhide', WARPAINT, 'On the face, in the paint ' +
    'colour: stripes on the cheeks, a mask across the eyes, a skull, a ' +
    'handprint, dots, or half the face.'),
  N('bodyPaint', 'body paint', 'gbhide', 'How much of the war paint goes on ' +
    'the body too: bands round the arms, marks on the chest.'),
  N('claws', 'claws', 'gbhide', 'The nails carved into claws, fingers and ' +
    'toes. Bone, and they cut a little.'),
  E('posture', 'posture', 'gbhide', POSTURES, 'Upright, or hunched: the spine ' +
    'pitched forward and the head up, held under the walk.'),
  // ---- hat
  E('hat', 'hat', 'gbhat', HATS, 'redcap: a knitted stocking cap that flops. ' +
    'hood: pointed, round the face, ears through slits. mushroom: a ' +
    'toadstool cap. morel: a honeycomb cone. pot: an upturned cooking pot. ' +
    'bonecrown: a band of bone spikes. skull: a beast\'s skull, jaw and ' +
    'horns. tophat: a battered stolen top hat. witch: a wide brim and a ' +
    'bent cone.'),
  N('hatSize', 'hat size', 'gbhat', 'Spread: a mushroom\'s dome, a brim, a ' +
    'skull\'s snout, a pot\'s girth, a stocking\'s girth.'),
  N('hatHeight', 'hat height', 'gbhat', 'How tall or long the main part ' +
    'runs: a stocking or a hood\'s point, a cone, a crown, a pot\'s depth, ' +
    'the spikes, a skull\'s horns (0 is none).'),
  N('hatTilt', 'hat tilt', 'gbhat', 'Cocked, or flopping, to the left below ' +
    '0 and the right above.', { min: -1, max: 1 }),
  N('hatFlop', 'hat flop', 'gbhat', 'How far it gives: a stocking hangs, a ' +
    'witch\'s tip bends over, a brim or a mushroom\'s rim droops, a top hat ' +
    'is crushed, spikes and horns curl.'),
  N('hatDetail', 'hat detail', 'gbhat', 'Its extra: a bobble, warts, a ' +
    'feather, a buckle, more spikes, finer honeycomb, a saucepan handle ' +
    'instead of two loops, the hood\'s trim.'),
  N('hatWear', 'hat wear', 'gbhat', 'Patches and holes in cloth; dents, ' +
    'chips and nibbles in anything else.', { roll: 0.4 }),
];

export const COLOR_SPECS = [
  ['wart', 'warts', 'Warts over the hide.'],
  ['tusk', 'tusks and teeth', 'Tusks, teeth, a bone through the nose, bone spikes.'],
  ['warpaint', 'war paint', 'Paint on the face and the body.'],
  ['ring', 'metal', 'Ear rings, a nose ring or stud, the cooking pot.'],
  ['hat', 'hat', 'A redcap, a mushroom cap, a bone crown\'s band.'],
  ['hatTrim', 'hat trim', 'A redcap\'s band and bobble, a mushroom\'s spots ' +
   'and gills.'],
  ['eyeDark', 'pupils', 'The pupils, and the nostrils.'],
];
export const COLOR_LABELS = {
  skin: ['hide', 'Most of the body is this.'],
  skinShade: ['hide, shaded', 'The hide turned from the light, the inside of ' +
              'the ears, the blotches.'],
  skinLight: ['hide, lit', 'The hide toward the light.'],
  eye: ['irises', 'The iris round the pupil, and the light it gives when lit.'],
  cloth: ['loincloth', 'The human\'s shorts, as a rag.'],
  clothShade: ['loincloth, shaded', 'The same, turned from the light.'],
  nail: ['claws', 'The nails, and the claws carved from them.'],
};

// =============================================================================
// presets: the race default and the characters filed in assets/mobs/goblin/
// =============================================================================

export const PRESETS = {
  goblin: { kind: 'forest' },
  // GRIB: the goblin you meet first. Forest-green, big-eared, hook-nosed, a
  // snaggle tooth and a cropped scrub of black hair.
  grib: {
    kind: 'forest',
    hairStyle: 'cropped',
    goblin: { earNotch: 0.15, warts: 0.25 },
    colors: { ...SKINS.moss, ...EYES.yellow, ...RAGS.sack, ...HAIRS.black },
  },
  // SNIKKIT: a forest sneak. Small, hunched, long-eared and quick-clawed,
  // with a ring through a hooked nose.
  snikkit: {
    kind: 'forest',
    body: { heightM: 1.05, shoulderWidth: 10, hipWidth: 8, limbWidth: 4 },
    shape: { waist: 0.9, armGirth: 0.85, legGirth: 0.85 },
    hairStyle: 'bald',
    goblin: { posture: 'hunched', claws: 0.75, earLength: 0.8, earNotch: 0.4,
              nosePierce: 'ring', hat: 'hood', hatHeight: 0.75, hatFlop: 0.6,
              hatDetail: 0.3, hatWear: 0.4 },
    colors: { ...SKINS.moss, ...EYES.yellow, ...METALS.brass, ...RAGS.hide,
              ...HAT_COLORS.moss },
  },
  // GUTROT: a hobgoblin chieftain. Big for a goblin, broad, pot-bellied,
  // tusked, ringed in brass, crowned in bone and striped in blood.
  gutrot: {
    kind: 'hob',
    body: { heightM: 1.55, shoulderWidth: 16, hipWidth: 12, bodyDepth: 10,
            limbWidth: 6 },
    shape: { chest: 1.15, shoulder: 1.1, waist: 1.25, chestDepth: 1.1,
             armGirth: 1.2 },
    hairStyle: 'topknot',
    goblin: { hat: 'skull', hatSize: 0.6, hatHeight: 0.75, hatFlop: 0.55,
              tusks: 0.85, earRings: 3,
              warpaint: 'stripes', bodyPaint: 0.7, earNotch: 0.3 },
    colors: { ...SKINS.ochre, ...EYES.amber, ...METALS.brass, ...PAINTS.blood,
              ...HAT_COLORS.brown, ...RAGS.umber, ...HAIRS.black },
  },
  // MIREWART: a bog witch under a mushroom cap. Warty, hunched, a bone through
  // her bulb of a nose, dotted in ochre.
  mirewart: {
    kind: 'bog',
    body: { sex: 'female', heightM: 1.15 },
    hairStyle: 'long',
    goblin: { hat: 'witch', hatSize: 0.55, hatHeight: 0.6, hatFlop: 0.85,
              hatDetail: 0.7, hatWear: 0.5, hatTilt: -0.4, warts: 0.85,
              posture: 'hunched',
              warpaint: 'dots', nosePierce: 'bone', earTuft: 0.4 },
    colors: { ...SKINS.bog, ...EYES.green, ...HAT_COLORS.moss, ...PAINTS.ochre,
              ...HAIRS.grey, ...RAGS.moss },
  },
  // THE REDCAP: the murderous one of the old stories, cap dyed in what it
  // kills. Saw-toothed, long-clawed, a bloody handprint over its face.
  redcap: {
    kind: 'redcap',
    body: { heightM: 1.2 },
    hairStyle: 'long',
    goblin: { hat: 'redcap', hatSize: 0.6, hatHeight: 0.8, hatFlop: 0.85,
              hatTilt: 0.3, hatDetail: 0.6, hatWear: 0.25, claws: 0.95,
              earRings: 1, warpaint: 'hand' },
    colors: { ...SKINS.ruddy, ...EYES.red, ...HAT_COLORS.red, ...PAINTS.blood,
              ...METALS.iron, ...HAIRS.white, ...RAGS.hide },
  },
  // GLOOMEYE: a cave goblin. Thin, pale, hunched, with great bat ears and
  // huge eyes that give light.
  gloomeye: {
    kind: 'cave',
    body: { heightM: 1.0, shoulderWidth: 10, hipWidth: 8, limbWidth: 4 },
    shape: { waist: 0.85, chest: 0.9, armGirth: 0.85, legGirth: 0.95 },
    hairStyle: 'bald',
    goblin: { posture: 'hunched', claws: 0.85, earLength: 0.75,
              blotch: 0.4, hat: 'morel', hatHeight: 0.55, hatFlop: 0.3,
              hatDetail: 0.7 },
    colors: { ...SKINS.pale, ...EYES.white, ...RAGS.sack, ...HAT_COLORS.tan },
  },
  // TINKERPOT: a hill goblin tinker under an upturned pot. Grinning,
  // copper-ringed, tusked, a stud in the nose.
  tinkerpot: {
    kind: 'hill',
    body: { heightM: 1.3 },
    hairStyle: 'spiky',
    goblin: { hat: 'pot', hatSize: 0.3, hatHeight: 0.4, hatTilt: 0.35,
              hatDetail: 0.8, hatWear: 0.45, teeth: 'grin',
              earRings: 2, nosePierce: 'stud', tusks: 0.35 },
    colors: { ...SKINS.olive, ...EYES.amber, ...METALS.copper, ...HAIRS.ginger,
              ...RAGS.umber },
  },
  // FROSTNIP: a frost goblin. Blue-grey, spike-eared and tufted, a woad
  // skull painted over her face, a white ponytail.
  frostnip: {
    kind: 'frost',
    body: { sex: 'female', heightM: 1.2 },
    hairStyle: 'ponytail',
    goblin: { earTuft: 0.8, warpaint: 'skull', bodyPaint: 0.4 },
    colors: { ...SKINS.frost, ...EYES.ice, ...HAIRS.white, ...PAINTS.woad,
              ...RAGS.hide },
  },
  // SOOTSKIN: an ash goblin out of the forge-pits. Black-hided, ember-eyed,
  // a grin of teeth, a half-painted face.
  sootskin: {
    kind: 'ash',
    body: { heightM: 1.25, shoulderWidth: 14, limbWidth: 6 },
    shape: { armGirth: 1.1, forearmMuscle: 0.5 },
    hairStyle: 'mohawk',
    goblin: { warpaint: 'split', blotch: 0.35, earNotch: 0.5, earRings: 1,
              noseBend: 0.6, hat: 'tophat', hatHeight: 0.6, hatFlop: 0.6,
              hatTilt: -0.5, hatDetail: 0.8, hatWear: 0.5 },
    colors: { ...SKINS.soot, ...EYES.amber, ...PAINTS.ochre, ...METALS.iron,
              ...HAT_COLORS.black,
              ...HAIRS.ginger, ...RAGS.hide },
  },
};
export const PRESET_ORDER = Object.keys(PRESETS);

// =============================================================================
// the generator hooks only a goblin has (mobgen buildSidecar)
// =============================================================================

/** A hunched goblin's base posture: the library's `hunch` clip held under
 *  everything (demon.js's mechanism), appended after every damage state. */
export function extraStates(g) {
  if (!g.goblin || g.goblin.posture !== 'hunched') return [];
  return [{ name: 'hunch', always: true, clip: 'hunch' }];
}

/** CLAWS SCRATCH, TUSKS AND TEETH BITE. The edges stay the human's; only the
 *  strike profiles move, and less than a demon's (goblin claws are dirty
 *  nails, not talons of bone). */
export function patchNatural(g, natural) {
  const s = g.goblin;
  if (!s) return;
  const c = s.claws;
  const toothy = s.teeth === 'fangs' || s.teeth === 'saw' ? 0.25 : 0;
  for (const nw of natural) {
    if (nw.name.startsWith('fist.') && c > 0.05)
      nw.strike = { ...nw.strike, cut: Math.round(c * 30) / 10 };
    if (nw.name === 'jaws' && (s.tusks > 0.05 || toothy))
      nw.strike = { ...nw.strike,
                    bite: Math.round(nw.strike.bite * (1 + 0.4 * s.tusks + toothy) * 10) / 10 };
  }
}

// =============================================================================
// THE SURFACE: blotches, warts, claws, body paint; on the head the eyes, the
// teeth and the face paint. Part-local at the shipped lattice, front LOW y,
// z up; never grows a box (a raised wart is put only inside it).
// =============================================================================

export function surfacePass(g, role, name, cells, size, info) {
  const s = g.goblin, A = info.ART;
  const G = grid(cells, size);
  const { sx, sy, sz, cx, cy } = G;
  const seed = strHash(name) ^ 0x60b1;
  const HIDE = new Set([A.SKIN_BASE, A.SKIN_SHADE, A.SKIN_LIGHT]);
  const LIT = new Set([A.SKIN_BASE, A.SKIN_LIGHT]);
  const frontOf = (() => {
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y < m.get(k)) m.set(k, y);
    }
    return (x, z) => m.get(x * 4096 + z);
  })();
  const paintFront = (x, z, c, over = HIDE) => {
    const y = frontOf(x, z);
    if (y === undefined) return;
    const v = G.get(x, y, z);
    if (v && over.has(v[3])) v[3] = c;
  };

  // ---- blotches: coarse noise patches, in the shaded hide -------------------
  if (s.blotch > 0.05 && role !== 'hand') {
    const thr = 1 - s.blotch * 0.4;
    for (const v of G.list()) {
      if (!LIT.has(v[3]) || !G.lateral(v[0], v[1], v[2])) continue;
      if (vnoise(seed, v[0], v[1], v[2], 3.6) > thr) v[3] = A.SKIN_SHADE;
    }
  }

  // ---- body paint: bands round the upper arms, marks on the chest ------------
  if (s.warpaint !== 'none' && s.bodyPaint > 0.05) {
    if (name === 'armU.L' || name === 'armU.R' ||
        (s.bodyPaint > 0.5 && (name === 'armL.L' || name === 'armL.R'))) {
      const n = 1 + Math.round(s.bodyPaint * 1.5);
      for (let i = 0; i < n; i++) {
        const z0 = Math.round(sz * (0.3 + 0.22 * i));
        for (const v of G.list())
          if ((v[2] === z0 || v[2] === z0 + 1) && HIDE.has(v[3]) &&
              G.lateral(v[0], v[1], v[2])) v[3] = SLOTS.PAINT;
      }
    }
    if (name === 'torso') chestPaint(s, G, frontOf, paintFront);
  }

  // ---- warts: dark bumps, raised where the box has room ----------------------
  if (s.warts > 0.05 && role !== 'hand' && role !== 'foot') {
    const p = s.warts * (role === 'head' ? 0.05 : 0.02);
    for (const v of G.list()) {
      if (!HIDE.has(v[3]) || !G.exposed(v[0], v[1], v[2])) continue;
      if (rnd(seed, v[0], v[1], v[2], 0x3a) >= p) continue;
      v[3] = SLOTS.WART;
      if (s.warts > 0.4) {
        // Proud: one cell outward on the first open lateral side.
        const [ix, iy] = G.inward(v[0], v[1]);
        const ox = v[0] + (v[0] - ix), oy = v[1] + (v[1] - iy);
        G.put(ox, oy, v[2], SLOTS.WART);
      }
    }
  }

  // ---- claws: the nails carved into talons (demon.js) -------------------------
  let shaped = G.list();
  if (s.claws > 0.05 && (role === 'hand' || role === 'foot')) {
    shaped = talons(shaped, role, size, s.claws * 0.8, A.NAIL);
  }
  if (role !== 'head') return keepLargest(shaped, size);

  // ---- the eyes -------------------------------------------------------------------
  const eyeRow = info.eyeRow ?? sz * 0.6;
  const eyeCells = G.list().filter(c => c[3] === A.EYE &&
                                       Math.abs(c[2] + 0.5 - eyeRow) < 5);
  const eyes = [eyeCells.filter(c => c[0] + 0.5 < cx),
                eyeCells.filter(c => c[0] + 0.5 >= cx)].filter(e => e.length);
  const grow = { beady: 0, big: 1, huge: 2, slit: 1, squint: 1 }[s.eyes] ?? 1;
  eyes.forEach((grp, side) => {
    const xs = grp.map(c => c[0]), zs = grp.map(c => c[2]);
    const x0 = Math.min(...xs), x1 = Math.max(...xs);
    const z0 = Math.min(...zs), z1 = Math.max(...zs);
    const mx = (x0 + x1 + 1) / 2, mz = (z0 + z1 + 1) / 2;
    const cellsOf = [];
    for (let z = z0 - grow; z <= z1 + grow; z++)
      for (let x = x0 - grow; x <= x1 + grow; x++) {
        // Rounded: the corners of the grown eye stay hide.
        const ex = Math.abs(x + 0.5 - mx) - (x1 - x0 + 1) / 2;
        const ez = Math.abs(z + 0.5 - mz) - (z1 - z0 + 1) / 2;
        if (grow > 0 && ex > grow - 1 && ez > grow - 1) continue;
        const y = frontOf(x, z);
        if (y === undefined) continue;
        const v = G.get(x, y, z);
        if (!v || !(HIDE.has(v[3]) || v[3] === A.EYE)) continue;
        cellsOf.push(v);
      }
    for (const v of cellsOf) {
      const dx = Math.abs(v[0] + 0.5 - mx), dz = v[2] + 0.5 - mz;
      let c = A.EYE;
      if (s.eyes === 'beady') c = SLOTS.PUPIL;
      else if (s.eyes === 'slit') c = dx < 0.75 ? SLOTS.PUPIL : A.EYE;
      else if (dx < 1.1 && Math.abs(dz) < 1.1) c = SLOTS.PUPIL;
      // SQUINT: the left eye's lid comes half down, in the shaded hide.
      if (s.eyes === 'squint' && side === 0 && dz > -0.1) c = A.SKIN_SHADE;
      v[3] = c;
    }
    // A beady eye keeps one glint of the iris colour, top outer corner.
    if (s.eyes === 'beady') {
      const gx = side === 0 ? x0 : x1;
      const y = frontOf(gx, z1);
      const v = y !== undefined && G.get(gx, y, z1);
      if (v) v[3] = A.EYE;
    }
  });
  const eyeX = eyes.map(grp => grp.reduce((a, c) => a + c[0] + 0.5, 0) / grp.length);
  const eyeZ = eyes.length
    ? eyes[0].reduce((a, c) => a + c[2] + 0.5, 0) / eyes[0].length : eyeRow;

  // ---- teeth on the mouth line ----------------------------------------------------
  if (s.teeth !== 'none' && info.mouthRow !== undefined) {
    const mz = Math.floor(info.mouthRow);
    const mx = cx + (info.mouthDx || 0);
    const hw = Math.round(g.face.mouthWidth * 1.5) + 1;
    const TEETHY = new Set([...HIDE, A.LIP]);
    const tooth = (x, z) => {
      const y = frontOf(x, z);
      if (y === undefined) return;
      const v = G.get(x, y, z);
      if (v && (TEETHY.has(v[3]) || v[3] !== A.EYE)) v[3] = SLOTS.TUSK;
    };
    if (s.teeth === 'snaggle') {
      // One crooked tooth off-centre, down over the lower lip, and a gap.
      const x = Math.floor(mx) + 1;
      tooth(x, mz); tooth(x, mz - 1);
      tooth(Math.floor(mx) - 2, mz);
    } else if (s.teeth === 'fangs') {
      for (const side of [-1, 1]) {
        const x = Math.floor(mx + side * 2 - (side < 0 ? 1 : 0));
        tooth(x, mz); tooth(x, mz - 1); tooth(x, mz - 2);
      }
    } else if (s.teeth === 'grin') {
      // A wide grin of teeth: the mouth row and the one above, gapped.
      for (let x = Math.floor(mx - hw - 1); x <= Math.ceil(mx + hw); x++) {
        tooth(x, mz);
        if ((x & 1) === 0) tooth(x, mz + 1);
      }
    } else if (s.teeth === 'saw') {
      // A saw-edge: a row of teeth and a point under every other one.
      for (let x = Math.floor(mx - hw); x < Math.ceil(mx + hw); x++) {
        tooth(x, mz);
        if ((x & 1) === 0) tooth(x, mz - 1);
      }
    }
  }

  // ---- war paint on the face ------------------------------------------------------
  const P = SLOTS.PAINT;
  const M = s.warpaint;
  const ez = Math.floor(eyeZ);
  if (M === 'stripes') {
    for (const ex of eyeX) {
      const out = ex < cx ? -1 : 1;
      for (let i = 0; i < 3; i++)
        for (let k = 0; k < 4; k++)
          paintFront(Math.floor(ex + out * (1 + k)) , ez - 3 - i * 2, P);
    }
  } else if (M === 'mask') {
    for (let z = ez - 2; z <= ez + 2; z++)
      for (let x = 0; x < sx; x++) paintFront(x, z, P);
  } else if (M === 'skull') {
    // Rings round the eyes and the teeth drawn down the jaw.
    for (const ex of eyeX)
      for (let z = ez - 4; z <= ez + 4; z++)
        for (let x = Math.floor(ex - 4); x <= Math.ceil(ex + 4); x++) {
          const d = Math.hypot(x + 0.5 - ex, z + 0.5 - eyeZ);
          if (d > 2.6 && d < 4.2) paintFront(x, z, P);
        }
    if (info.mouthRow !== undefined) {
      const mz = Math.floor(info.mouthRow);
      for (let x = Math.floor(cx) - 4; x <= Math.floor(cx) + 4; x += 2)
        for (let z = mz - 3; z <= mz + 1; z++) paintFront(x, z, P);
    }
  } else if (M === 'hand') {
    // A handprint over the right of the face: a palm and four fingers up.
    const hx = Math.floor(cx + sx * 0.12), hz = ez - 5;
    for (let z = hz; z < hz + 5; z++)
      for (let x = hx - 2; x < hx + 3; x++) paintFront(x, z, P);
    for (let f = 0; f < 4; f++)
      for (let z = hz + 5; z < hz + 9 - (f === 0 || f === 3 ? 1 : 0); z++)
        paintFront(hx - 2 + f + (f > 1 ? 1 : 0) - (f === 0 ? 1 : 0), z, P);
  } else if (M === 'dots') {
    for (let z = ez - 8; z < sz; z += 3)
      for (let x = (z & 1) * 2; x < sx; x += 4)
        if (Math.abs(z - eyeZ) > 2) paintFront(x, z, P);
  } else if (M === 'split') {
    // Half the face, the left, from the brow to the chin.
    for (let z = 0; z < sz; z++)
      for (let x = 0; x < Math.floor(cx); x++) paintFront(x, z, P);
  }
  return keepLargest(G.list(), size);
}

/** The chest's share of the war paint, matched to the face's. */
function chestPaint(s, G, frontOf, paintFront) {
  const { sx, sz, cx } = G;
  const P = SLOTS.PAINT;
  const amt = s.bodyPaint;
  if (s.warpaint === 'stripes' || s.warpaint === 'split') {
    // Diagonal slashes across the chest.
    const n = 1 + Math.round(amt * 2);
    for (let i = 0; i < n; i++)
      for (let t = 0; t < sx; t++)
        for (const dz of [0, 1])
          paintFront(t, Math.round(sz * (0.75 - 0.14 * i) - t * 0.5 + sx * 0.25) + dz, P);
  } else if (s.warpaint === 'hand') {
    const hx = Math.floor(cx - 3), hz = Math.round(sz * 0.55);
    for (let z = hz; z < hz + 6; z++)
      for (let x = hx; x < hx + 6; x++) paintFront(x, z, P);
    for (let f = 0; f < 4; f++)
      for (let z = hz + 6; z < hz + 10; z++) paintFront(hx + f * 2 - 1 + (f > 1 ? 1 : 0), z, P);
  } else if (s.warpaint === 'skull') {
    // Ribs.
    const n = 2 + Math.round(amt * 3);
    for (let i = 0; i < n; i++) {
      const z = Math.round(sz * (0.82 - 0.09 * i));
      for (let x = 0; x < sx; x++)
        if (Math.abs(x + 0.5 - cx) > 1.5 && Math.abs(x + 0.5 - cx) < sx * 0.36)
          paintFront(x, z, P);
    }
    for (let z = Math.round(sz * 0.4); z < Math.round(sz * 0.9); z++)
      for (const x of [Math.floor(cx) - 1, Math.floor(cx)]) paintFront(x, z, P);
  } else {
    // mask, dots: dots over the shoulders and down the breastbone.
    for (let z = Math.round(sz * 0.45); z < sz - 2; z += 3)
      for (let x = (z & 1); x < sx; x += 4)
        if (rnd(z, x, 7) < 0.5 + amt * 0.5) paintFront(x, z, P);
  }
  void frontOf;
}

// =============================================================================
// THE EXTRAS: ears, nose and tusks, and the hat (all ride the head). SCENE
// coordinates at the shipped lattice (x right, y BACK, z up).
// =============================================================================

export function extrasWant(g) {
  const s = g.goblin;
  return s.earLength > 0.02 || s.noseLength > 0.02 || s.noseBulb > 0.05 ||
         s.tusks > 0.05 || s.hat !== 'none';
}

const add3 = (a, b, k = 1) => [a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k];
const norm3 = a => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };
const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                          a[0] * b[1] - a[1] * b[0]];
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

export function extras(g, ctx) {
  const s = g.goblin;
  const { K, body, head, ART } = ctx;
  const mass = ctx.mass || null;
  const out = new Map();
  const free = (X, Y, Z) => !body.has(K(X, Y, Z)) && !out.has(K(X, Y, Z));
  const put = (X, Y, Z, c) => {
    X = Math.floor(X); Y = Math.floor(Y); Z = Math.floor(Z);
    if (free(X, Y, Z)) out.set(K(X, Y, Z), [X, Y, Z, c]);
  };
  // A cell that is ALWAYS this colour, even over one an earlier step put.
  const force = (X, Y, Z, c) => {
    X = Math.floor(X); Y = Math.floor(Y); Z = Math.floor(Z);
    if (!body.has(K(X, Y, Z))) out.set(K(X, Y, Z), [X, Y, Z, c]);
  };

  const H = extent(head);
  const hc = sceneCells(head);
  const EYEISH = new Set([ART.EYE, SLOTS.PUPIL]);
  const eyeCells = hc.filter(c => EYEISH.has(c[3]));
  const eyeZ = eyeCells.length
    ? eyeCells.reduce((a, c) => a + c[2], 0) / eyeCells.length + 0.5
    : H.z0 + (H.z1 - H.z0) * 0.6;
  const xc = (H.x0 + H.x1 + 1) / 2;
  const headW = H.x1 - H.x0 + 1;
  const front = new Map(), topAt = new Map();
  for (const [x, y, z] of hc) {
    const kf = x * 4096 + z;
    if (!front.has(kf) || y < front.get(kf)) front.set(kf, y);
    const kt = x * 4096 + y;
    if (!topAt.has(kt) || z > topAt.get(kt)) topAt.set(kt, z);
  }
  const frontAt = (x, z) => front.get(Math.floor(x) * 4096 + Math.floor(z));
  const skullTop = Math.max(...topAt.values());
  const mouthZ = eyeZ - 2 * g.face.mouthDrop;
  const seed = 0x9b1;

  // ---- the ears ---------------------------------------------------------------
  if (s.earLength > 0.02) for (const side of [-1, 1]) ear(side);
  function ear(side) {
    const zr = Math.round(eyeZ) - 1;
    let edge = side > 0 ? -1e9 : 1e9;
    for (const [x, , z] of hc)
      if (Math.abs(z - zr) < 2 && (side > 0 ? x > edge : x < edge)) edge = x;
    if (!Number.isFinite(edge)) return;
    const ys = hc.filter(([x, , z]) => x === edge && Math.abs(z - zr) < 2).map(c => c[1]);
    const ey = ys.reduce((a, b) => a + b, 0) / ys.length + 0.5;
    const root = [edge + 0.5 - side * 1.5, ey + 1, zr + 0.5];
    const len = 3 + s.earLength * headW * 0.95;
    const W = 2.5 + s.earWidth * headW * 0.42;
    const yaw = s.earSweep * 70 * Math.PI / 180;
    const el0 = s.earRise * 60 * Math.PI / 180;
    // The blade's profile: rises from the root, tapers to the tip -- sharper
    // with earPoint. Normalised so its widest is 1.
    const prof = t => Math.pow(t + 0.06, 0.45) * Math.pow(Math.max(0, 1 - t), 0.25 + s.earPoint * 1.3);
    let pmax = 0;
    for (let t = 0; t <= 1; t += 0.02) pmax = Math.max(pmax, prof(t));
    // Notches: deterministic spots along the upper and lower edges.
    const nN = Math.round(s.earNotch * 3);
    const notches = [];
    for (let i = 0; i < nN; i++)
      notches.push({ t: 0.3 + 0.5 * rnd(seed, side, i, 1), upper: (i + (side > 0 ? 1 : 0)) % 2 === 0,
                     depth: 0.35 + 0.35 * rnd(seed, side, i, 2) });
    // March the centre line, bending the elevation down with the flop.
    let p = root.slice();
    const ds = 0.35;
    const steps = Math.ceil(len / ds);
    const rings = [];
    const nR = Math.round(s.earRings);
    for (let i = 0; i < nR; i++) rings.push(0.18 + i * 0.13);
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      const el = el0 - s.earFlop * 110 * Math.PI / 180 * Math.pow(t, 1.5);
      const d = [side * Math.cos(yaw) * Math.cos(el), Math.sin(yaw) * Math.cos(el), Math.sin(el)];
      // The blade's breadth runs as near vertical as it can, across d.
      const up = [0, 0, 1];
      let w = add3(up, d, -dot3(up, d));
      if (Math.hypot(...w) < 0.2) w = [0, 1, 0];
      w = norm3(w);
      let n = norm3(cross3(d, w));
      if (n[1] > 0) n = [-n[0], -n[1], -n[2]];       // n points to the FRONT
      const f = prof(t) / pmax;
      const upper = W * f * 0.42, lower = W * f * 0.58;
      const thick = t < 0.8 ? 2 : 1;
      for (let b = -lower; b <= upper; b += 0.35) {
        // A notch bites a V out of the edge.
        let cut = false;
        for (const nt of notches) {
          const edgeDist = nt.upper ? upper - b : b + lower;
          const span = 0.09;
          if (Math.abs(t - nt.t) < span &&
              edgeDist < W * f * nt.depth * (1 - Math.abs(t - nt.t) / span)) cut = true;
        }
        if (cut) continue;
        const rim = upper - b < 1.1 || b + lower < 1.1 || t > 0.85;
        const q = add3(p, w, b);
        for (let k = 0; k < thick; k++) {
          const r = add3(q, n, -k * 0.9);
          // The front face, inside the rim, is the shaded inner ear.
          put(r[0], r[1], r[2], k === 0 && !rim && thick === 2 ? ART.SKIN_SHADE : ART.SKIN_BASE);
        }
      }
      // Rings: a loop hanging off the lower edge at each ring's place.
      for (const rt of rings)
        if (Math.abs(t - rt) < ds / len / 2 + 1e-6) {
          const c0 = add3(p, w, -lower - 1.4);
          for (let a = 0; a < 2 * Math.PI; a += 0.25) {
            const rr = add3(add3(c0, d, Math.cos(a) * 1.5), w, Math.sin(a) * 1.5);
            force(rr[0], rr[1], rr[2], SLOTS.RING);
          }
        }
      if (i === steps && s.earTuft > 0.05) {
        // A tuft of hair at the tip, mostly upward.
        const R = 1.2 + s.earTuft * 2.4;
        for (let dz = 0; dz <= Math.ceil(R * 1.6); dz++)
          for (let dy = -Math.ceil(R); dy <= Math.ceil(R); dy++)
            for (let dx = -Math.ceil(R); dx <= Math.ceil(R); dx++) {
              const rr = (dx * dx + dy * dy) / (R * R) + (dz * dz) / (R * R * 2.6);
              if (rr > 1 || rnd(seed, dx, dy, dz + side * 31) < rr * 0.5) continue;
              put(p[0] + dx, p[1] + dy, p[2] + dz, ART.HAIR);
            }
      }
      p = add3(p, d, ds);
    }
  }

  // ---- the nose ---------------------------------------------------------------
  if (s.noseLength > 0.02 || s.noseBulb > 0.05) {
    const zr = eyeZ - 2;
    const fy = frontAt(xc - 0.5, zr) ?? H.y0;
    const len = 1.5 + s.noseLength * headW * 0.55;
    // A WEDGE, not a trunk: broad where it leaves the face, narrowing to the
    // tip (or swelling into the bulb).
    const r0 = 2.4 + 0.6 * (g.face.noseWidth || 1);
    const rb = s.noseBulb > 0.05 ? 1.2 + s.noseBulb * 2.4 : 0;
    const rt = Math.max(1.2, rb * 0.6);
    let p = [xc + 0.0, fy + 2, zr];
    const ds = 0.4;
    const steps = Math.ceil(len / ds);
    const a0 = 22 * Math.PI / 180;
    let tip = p, dir = [0, -1, 0];
    const noseCells = [];
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      // Angle below level: the bridge goes down and out; the hook bends the
      // end further down (a beak) or up (a snout).
      const a = a0 + s.noseHook * 55 * Math.PI / 180 * t * t;
      dir = [s.noseBend * 0.35 * t, -Math.cos(a), -Math.sin(a)];
      const r = r0 + (rt - r0) * Math.pow(t, 1.4);
      ball(p, r, noseCells);
      tip = p;
      p = add3(p, norm3(dir), ds);
    }
    if (rb > 0) ball(add3(tip, norm3(dir), rb * 0.3), rb, noseCells);
    // Nostrils: two dark cells under the end.
    const nz = Math.floor(tip[2] - Math.max(rt, rb * 0.8));
    for (const dx of [-1, 1]) {
      const X = Math.floor(tip[0] + dx * Math.max(0.6, rt * 0.6));
      const k = K(X, Math.floor(tip[1]), nz);
      if (out.has(k)) out.get(k)[3] = SLOTS.PUPIL;
    }
    // Warts on the nose.
    if (s.warts > 0.1)
      for (const c of noseCells)
        if (rnd(seed, c[0], c[1], c[2], 5) < s.warts * 0.08) {
          c[3] = SLOTS.WART;
          if (s.warts > 0.4) put(c[0], c[1] - 1, c[2] + 1, SLOTS.WART);
        }
    // Piercings.
    if (s.nosePierce === 'ring') {
      const cy0 = Math.floor(tip[1]), cz0 = nz - 1.2;
      for (let a = 0; a < 2 * Math.PI; a += 0.3)
        force(tip[0] + Math.cos(a) * 1.1, cy0 - 0.5, cz0 + Math.sin(a) * 1.1, SLOTS.RING);
    } else if (s.nosePierce === 'bone') {
      // Across the bridge, a knob at each end.
      const m = add3([xc, fy + 1.5, zr], [0, -1, -0.5], len * 0.35);
      const half = r0 + 3.5;
      for (let x = -half; x <= half; x += 0.5) force(m[0] + x, m[1] - r0 * 0.6, m[2], SLOTS.TUSK);
      for (const sd of [-1, 1])
        for (const [dx, dz] of [[0, 1], [0, -1], [sd, 1], [sd, -1]])
          force(m[0] + sd * half + dx, m[1] - r0 * 0.6, m[2] + dz, SLOTS.TUSK);
    } else if (s.nosePierce === 'stud') {
      // A stud on the left side, halfway down.
      const m = noseCells.filter(c => c[0] < xc - r0 * 0.6);
      if (m.length) { const c = m[Math.floor(m.length / 2)]; c[3] = SLOTS.RING; }
    }
  }
  function ball(c, r, list) {
    const R = Math.ceil(r);
    for (let dz = -R; dz <= R; dz++)
      for (let dy = -R; dy <= R; dy++)
        for (let dx = -R; dx <= R; dx++) {
          const X = Math.floor(c[0] + dx), Y = Math.floor(c[1] + dy), Z = Math.floor(c[2] + dz);
          const ex = X + 0.5 - c[0], ey = Y + 0.5 - c[1], ez = Z + 0.5 - c[2];
          if (ex * ex + ey * ey + ez * ez > r * r + 0.25) continue;
          const k = K(X, Y, Z);
          if (body.has(k) || out.has(k)) continue;
          const cell = [X, Y, Z, ez >= -0.3 ? ART.SKIN_BASE : ART.SKIN_SHADE];
          out.set(k, cell);
          list.push(cell);
        }
  }

  // ---- tusks: up from the lower jaw ----------------------------------------------
  if (s.tusks > 0.05) {
    const len = 2 + s.tusks * 4;
    const off = 2.5 + s.tuskSpread * 3.5;
    for (const side of [-1, 1]) {
      // Rooted on a REAL jaw cell, INSIDE it (cells there are skipped), so
      // even a short tusk touches the head it rides -- validateMob refuses a
      // floating one. A narrow chin may have no face where the spread asks,
      // so the root walks inward and up the jaw until it finds some.
      let x0 = xc + side * off, z0 = mouthZ - 3, fy;
      search: for (let k = 0; k <= 6; k++)
        for (const dz of [0, 1, 2, -1]) {
          const f = frontAt(xc + side * (off - k), mouthZ - 3 + dz);
          if (f !== undefined) { x0 = xc + side * (off - k); z0 = mouthZ - 3 + dz; fy = f; break search; }
        }
      if (fy === undefined) continue;
      let p = [x0, fy + 0.5, z0];
      const thickTo = s.tusks > 0.4 ? 0.55 : 0;
      for (let d = 0; d <= len; d += 0.4) {
        const t = d / len;
        const dir = norm3([side * 0.5 * t, -1.1 + 0.9 * t, 1]);
        force(p[0], p[1], p[2], SLOTS.TUSK);
        if (t < thickTo) {
          force(p[0] + side, p[1], p[2], SLOTS.TUSK);
          force(p[0], p[1] - 1, p[2], SLOTS.TUSK);
          force(p[0] + side, p[1] - 1, p[2], SLOTS.TUSK);
        }
        p = add3(p, dir, 0.4);
      }
    }
  }

  // ---- the hat (goblinhats.js) ---------------------------------------------------
  if (s.hat !== 'none')
    buildHat(s, { K, body, out, mass, hc, eyeZ, mouthZ, SLOTS,
                  eyeTop: eyeCells.length ? Math.max(...eyeCells.map(c => c[2])) : undefined });

  return { head: [...out.values()], torso: [] };
}
