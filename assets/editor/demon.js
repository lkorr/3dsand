/* demon.js — THE DEMON RACE: summoned things on the human rig
 * (docs/PLAN_demons.md D1).
 *
 * WHAT A DEMON IS, TO THE GENERATOR. The human body -- the SAME fifteen limb
 * boxes, anchors, sockets, chains and clips, so every helm and blade fits and
 * every system that binds by limb name binds -- in a skin of reds, ash or
 * sulfur, mottled darker in patches, with horns off the skull, a tail off the
 * small of the back, claws where the nails are, and eyes that give light.
 *
 * IT IS FLESH. Unlike the machine races (automaton.js, android.js) a demon
 * walks, falls, burns, BLEEDS and dies exactly like a human: its anatomy is
 * the human's recipe (`ANATOMY = null` -> mobgen falls back to it), its bleed
 * is blood, its hair is hair. Only the parts that are not flesh name a
 * material: horns and claws are `bone`, lit eyes `gem_arcane` (emission, the
 * art colour is the light's colour -- the android's and the sylvan's
 * mechanism). Every other art slot keeps the human's answer (`DEFAULT_MAT =
 * null`: skin on the body, hair_white on the hair mass).
 *
 * SIZE IS THE RACE'S OWN BAND. A human is held to mobgen.HEIGHT_BAND
 * (1.53..1.87 m); a demon to HEIGHT_BAND below (0.85..2.1 m), so an imp can
 * be knee-high and a greater demon a head over a man while every human still
 * clamps where it did. The proportions are the human's genes either way --
 * an imp is the same rig with a small budget and a big head.
 *
 * POSTURE IS A GENE. `posture: 'hunched'` adds an `always` loco state that
 * holds the library's additive `hunch` clip (assets/anims/hunch.json: the
 * spine pitched forward, the head lifted to look ahead, the arms hanging
 * forward) under everything else -- the snake's base-state mechanism, so the
 * gait still walks underneath it and every damage state (crawl, limp, squirm)
 * listed before it still wins. No C++ knows what a hunch is.
 *
 * HUMANS ARE UNTOUCHED: every entry point is reached only for
 * `genome.body.race === 'demon'`, and every gene here is race-gated (no human
 * roll, mutation or cross takes a draw for it). PURE AND NODE-RUNNABLE;
 * imports only robotkit.js.
 */

'use strict';

import { rnd, strHash, clamp, grid, vnoise, keepLargest, line6, sceneCells,
         extent } from './robotkit.js';

export const RACE = 'demon';
export const FOLDER = 'demon';
export const NEW_NAME = 'fiend';
/** Not a machine: section P of test_mobgen (no flesh inside, stand-ins, the
 *  human's natural weapons verbatim) is the machines' promise; section Q is
 *  this race's. */
export const MACHINE = false;
export const BLURB = 'a demon: a summoned thing in a red, ash or sulfur ' +
  'hide, horned, clawed, tailed, with eyes that give light. Flesh like a ' +
  'human -- it bleeds and burns -- on the same limbs, from an imp at knee ' +
  'height to a greater demon a head over a man.';

/** The race's own height band (metres), replacing mobgen.HEIGHT_BAND for a
 *  demon only. 0.85 is the smallest figure validateMob passes (8 world
 *  voxels tall) with a margin; 2.1 is where the human reach contract starts
 *  to lie badly (mobgen's HEIGHT_BAND note), and a greater demon is meant to
 *  be a fight, not a broken rig. */
export const HEIGHT_BAND = [0.85, 2.1];

// =============================================================================
// art slots and materials
// =============================================================================

/** The demon's own art slots, below the android's (222). */
export const SLOTS = {
  HORN: 221,       // horn
  HORN_TIP: 220,   // the horn's darker or paler tip
  CLAW: 219,       // claws: fingertips and toes
  MARK: 218,       // mottling, the darker patches of hide
};

export const COLOR_SLOTS = {
  horn: SLOTS.HORN, hornTip: SLOTS.HORN_TIP, claw: SLOTS.CLAW, mark: SLOTS.MARK,
};

export const MATS = { horn: 'bone', glow: 'gem_arcane' };
/** null: every slot not named below keeps the HUMAN's material (mobgen's
 *  `p.mat || flesh`) -- skin on the body, hair_white on the hair mass. */
export const DEFAULT_MAT = null;
export const STAND_INS = {};
/** Blood, like a human (null would be "bleeds nothing", the android). */
export const BLEED = { material: 'blood', perDamage: 2.5 };
/** null: the human's recipe (anatomy.js DEFAULT_ANATOMY) -- skin, flesh,
 *  muscle, bone, a skull round a brain. A demon is flesh. */
export const ANATOMY = null;
export const ANATOMY_NOTES = {};

export function slotMaterialNames(g, ART) {
  const m = {};
  // The hide is SKIN wherever it is painted -- including the tail, which is
  // an extra (a hair-mass part, hair_white unless a slot says otherwise).
  for (const sl of [ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT, ART.NAIL,
                    SLOTS.MARK])
    m[sl] = 'skin';
  m[SLOTS.HORN] = MATS.horn;
  m[SLOTS.HORN_TIP] = MATS.horn;
  m[SLOTS.CLAW] = MATS.horn;
  // The eyes give light when lit; otherwise they stay the human's (skin).
  if (g.demon && g.demon.glow) m[ART.EYE] = MATS.glow;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export function defaultGenes() {
  return {
    hornStyle: 'goat',   // a horn preset (HORNS); the numbers below are the truth
    horns: 0.5,          // horn length, 0 = none
    hornCurl: 0.5,       // straight up (0) .. swept back and down (1)
    hornSpread: 0.4,     // set close on the brow (0) .. wide on the temples (1)
    tail: 0.5,           // tail length, 0 = none
    claws: 0.6,          // how far the claws run up the fingers and toes
    mottle: 0.35,        // darker patches over the hide
    glow: true,          // the eyes are lit
    posture: 'upright',  // 'upright' | 'hunched' (the `hunch` loco state)
  };
}

export const POSTURES = ['upright', 'hunched'];

/** Hides a roll picks WHOLE. The skin family (skin, shade, light, nail) and
 *  the mottle are one set, so a crimson hide never wears ash patches. */
export const HIDES = {
  crimson: { skin: '#9a2a22', skinShade: '#6e1a16', skinLight: '#c4483a', nail: '#5a1410', mark: '#4a0e0c' },
  ember:   { skin: '#b8481e', skinShade: '#82300f', skinLight: '#e0703a', nail: '#5a1e0a', mark: '#3a1206' },
  ash:     { skin: '#5a5450', skinShade: '#3c3836', skinLight: '#7c7470', nail: '#24201e', mark: '#2a2422' },
  sulfur:  { skin: '#b89a2a', skinShade: '#86701a', skinLight: '#dcc050', nail: '#4a3a0a', mark: '#6a5210' },
  soot:    { skin: '#2e2826', skinShade: '#1e1a18', skinLight: '#4a4240', nail: '#100e0e', mark: '#5a1410' },
  bruise:  { skin: '#5a2a4a', skinShade: '#3e1a32', skinLight: '#7a3e66', nail: '#24101c', mark: '#2a0e20' },
};
/** Horn colours: the shaft and its tip. */
export const HORN_COLORS = {
  bone:   { horn: '#d8ccb0', hornTip: '#8a7e66' },
  black:  { horn: '#2a2624', hornTip: '#5a5450' },
  ochre:  { horn: '#a07a3a', hornTip: '#4a3418' },
  ivory:  { horn: '#efe6cc', hornTip: '#b4a888' },
};
/** Eye glows: the sylvan's and the android's hexes where they match, so the
 *  shared art palette spends nothing on them. */
export const EYES = {
  ember:  { eye: '#ff7a2a' },
  blood:  { eye: '#ff3a3a' },
  sulfur: { eye: '#ffc24a' },
  pale:   { eye: '#cfe4ff' },
};
/** The loincloth (the human's shorts) and the hair. */
export const CLOTHS = {
  hide:   { cloth: '#3a2a20', clothShade: '#261a12', hair: '#1a1210', hairShade: '#0e0a08' },
  sack:   { cloth: '#5c4f39', clothShade: '#433a2a', hair: '#2a1e18', hairShade: '#1a120e' },
  ash:    { cloth: '#3a3634', clothShade: '#262422', hair: '#4a4240', hairShade: '#2e2826' },
};
export const SHARED_COLORS = { claw: '#1a1412' };
export const DEFAULT_COLORS = {
  ...HIDES.crimson, ...HORN_COLORS.bone, ...EYES.ember, ...CLOTHS.hide,
  ...SHARED_COLORS,
};
export const GLOWS = Object.fromEntries(Object.entries(EYES).map(([k, v]) => [k, v.eye]));
export const COLOR_SETS = [HIDES, HORN_COLORS, EYES, CLOTHS];
function lum(hex) {
  const n = parseInt(hex.slice(1), 16);
  return ((n >> 16) & 255) * 0.3 + ((n >> 8) & 255) * 0.59 + (n & 255) * 0.11;
}
export const STOCK = Object.entries(HIDES).sort((a, b) => lum(a[1].skin) - lum(b[1].skin));
export const STOCK_LABEL = 'hide';

/** Within-box build offsets: a demon is a touch leaner in the waist and
 *  harder in the jaw than the human it was switched from. */
export const BUILD_DELTA = {
  'shape.waist': -0.05,
  'head.jaw': 0.08,
};

const HORN_KEYS = ['horns', 'hornCurl', 'hornSpread'];
export const HORNS = {
  none:   { horns: 0,    hornCurl: 0,    hornSpread: 0.4 },
  nubs:   { horns: 0.15, hornCurl: 0.1,  hornSpread: 0.35 },
  goat:   { horns: 0.5,  hornCurl: 0.55, hornSpread: 0.35 },
  spikes: { horns: 0.6,  hornCurl: 0,    hornSpread: 0.5 },
  ram:    { horns: 0.8,  hornCurl: 1,    hornSpread: 0.8 },
};
export const HORN_ORDER = Object.keys(HORNS);

export function applyHorns(genome, style) {
  const f = HORNS[style];
  if (!f) return genome;
  genome.demon.hornStyle = style;
  for (const k of HORN_KEYS) genome.demon[k] = f[k];
  return genome;
}

export const PICKERS = [
  { key: 'horns', path: 'demon.hornStyle', label: 'horns', order: HORN_ORDER,
    apply: applyHorns,
    title: 'a starting pair of horns: none, nubs, goat, spikes, ram' },
];

/** A demon keeps its hair (it is flesh); a beard is the human's business. */
export function onBecome(genome, h) {
  if (!h.isLocked('face.beardStyle')) h.applyBeardStyle(genome, 'clean');
}

export function randomize(g, r, isLocked) {
  if (!isLocked('demon.hornStyle')) applyHorns(g, r.pick(HORN_ORDER));
  for (const spec of GENE_SPECS) {
    if (spec.kind || isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    const v = g.demon[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    g.demon[k] = clamp(v, spec.min, spec.max);
  }
  if (!isLocked('demon.glow')) g.demon.glow = r() < 0.9;
  if (!isLocked('demon.posture')) g.demon.posture = r() < 0.3 ? 'hunched' : 'upright';
  return g;
}

export const GENE_GROUPS = [
  { key: 'dmhead', title: 'horns and eyes', race: 'demon',
    note: 'Horns of bone off the skull, from nubs to a ram\'s sweep, and eyes ' +
          'that give light in the eye colour.' },
  { key: 'dmbody', title: 'hide, claws, tail, posture', race: 'demon',
    note: 'A hide mottled darker in patches, claws where the nails are, a ' +
          'tail off the small of the back, and an upright or a hunched ' +
          'carriage (a held pose under the walk, not a different rig).' },
];

const S = (path, label, group, hint, extra = {}) => ({
  path: 'demon.' + path, label, group, min: 0, max: 1, step: 0.05,
  sigma: 0.2, race: 'demon', hint, ...extra });

export const GENE_SPECS = [
  { path: 'demon.hornStyle', label: 'horns', group: 'dmhead', kind: 'enum',
    choices: HORN_ORDER, sigma: 0.15, race: 'demon',
    hint: 'A starting pair: none, nubs, goat, spikes, ram.' },
  S('horns', 'horn length', 'dmhead', 'How far the horns stand off the skull. ' +
    '0 is none.'),
  S('hornCurl', 'horn curl', 'dmhead', 'Straight up at 0; swept back and down ' +
    'past the ears at 1.'),
  S('hornSpread', 'horn spread', 'dmhead', 'Set close over the brow at 0, out ' +
    'on the temples at 1.'),
  { path: 'demon.glow', label: 'lit eyes', group: 'dmhead', kind: 'bool',
    sigma: 0, race: 'demon',
    hint: 'The eyes give light in the eye colour. Off, they are the hide\'s.' },
  S('mottle', 'mottling', 'dmbody', 'Darker patches over the hide, in the ' +
    'mark colour.'),
  S('claws', 'claws', 'dmbody', 'How far the claws run up the fingers and ' +
    'toes. Bone, and they cut.'),
  S('tail', 'tail', 'dmbody', 'A tail off the small of the back, curling up ' +
    'at the tip. 0 is none.', { roll: 0.8 }),
  { path: 'demon.posture', label: 'posture', group: 'dmbody', kind: 'enum',
    choices: POSTURES, sigma: 0.1, race: 'demon',
    hint: 'Upright, or hunched: the spine pitched forward and the head up, ' +
          'held under the walk.' },
];

export const COLOR_SPECS = [
  ['horn', 'horns', 'The horns\' shaft.'],
  ['hornTip', 'horn tips', 'The horns\' tips.'],
  ['claw', 'claws', 'Finger and toe claws.'],
  ['mark', 'mottling', 'The darker patches over the hide.'],
];
export const COLOR_LABELS = {
  skin: ['hide', 'Most of the body is this.'],
  skinShade: ['hide, shaded', 'The hide turned from the light.'],
  skinLight: ['hide, lit', 'The hide toward the light.'],
  eye: ['eyes', 'The eyes, and the colour of the light they give when lit.'],
  cloth: ['loincloth', 'The human\'s shorts, as a rag of hide.'],
  clothShade: ['loincloth, shaded', 'The same, turned from the light.'],
  nail: ['nails', 'Under the claws.'],
};

export const PRESETS = {
  demon: { horns: 'goat' },
  // THE IMP: knee-high, big-headed, long-armed and hunched. Every number is
  // an ordinary human gene at the bottom of the race's band.
  imp: {
    horns: 'nubs',
    body: { heightM: 0.85, shoulderWidth: 8, hipWidth: 8, bodyDepth: 6,
            limbWidth: 4, headWidth: 10, headDepth: 10, footDepth: 5,
            stack: { foot: 6, shin: 9, thigh: 9, hips: 7, torso: 14, head: 20 },
            armStack: { upper: 12, fore: 12, hand: 6 } },
    shape: { waist: 0.8, chest: 0.9, armGirth: 0.8, legGirth: 0.8 },
    hairStyle: 'bald',
    colors: { ...HIDES.crimson, ...HORN_COLORS.black, ...EYES.sulfur, ...CLOTHS.hide },
    demon: { horns: 0.3, tail: 0.7, claws: 0.8, mottle: 0.4, posture: 'hunched' },
  },
  // A GREATER DEMON: a head over a man, broad, ram-horned, upright.
  fiend: {
    horns: 'ram',
    body: { heightM: 2.0, shoulderWidth: 16, hipWidth: 12, bodyDepth: 10,
            limbWidth: 6, headWidth: 12, headDepth: 12,
            stack: { shin: 14, thigh: 15, torso: 20, head: 15 },
            armStack: { upper: 12, fore: 12, hand: 6 } },
    shape: { chest: 1.15, shoulder: 1.15, chestDepth: 1.1, armGirth: 1.15,
             legGirth: 1.1 },
    hairStyle: 'bald',
    colors: { ...HIDES.ash, ...HORN_COLORS.black, ...EYES.ember, ...CLOTHS.ash },
    demon: { tail: 0.8, claws: 0.7, mottle: 0.3, posture: 'upright' },
  },
};
export const PRESET_ORDER = Object.keys(PRESETS);

// =============================================================================
// the generator hooks only a demon has (mobgen buildSidecar)
// =============================================================================

/** Loco states appended AFTER the human's (so every damage state still wins):
 *  a hunched demon's base posture. */
export function extraStates(g) {
  if (!g.demon || g.demon.posture !== 'hunched') return [];
  return [{ name: 'hunch', always: true, clip: 'hunch' }];
}

/** CLAWS CUT. The fists keep their edge (the human's segment, so section Q's
 *  "same rig" holds) and gain a kerf in proportion to the claws gene: a rake
 *  of bone rather than a punch. */
export function patchNatural(g, natural) {
  const c = g.demon ? g.demon.claws : 0;
  if (!(c > 0.05)) return;
  for (const nw of natural)
    if (nw.name.startsWith('fist.'))
      nw.strike = { ...nw.strike, cut: Math.round(c * 50) / 10,
                    blunt: Math.round(nw.strike.blunt * 6) / 10 };
}

// =============================================================================
// the surface: mottling and claws. Part-local, front LOW y, z up; never grows
// a box.
// =============================================================================

export function surfacePass(g, role, name, cells, size, info) {
  const s = g.demon, A = info.ART;
  const G = grid(cells, size);
  const seed = strHash(name) ^ 0xd3a7;
  const HIDE = new Set([A.SKIN_BASE, A.SKIN_SHADE, A.SKIN_LIGHT]);

  // ---- mottling: coarse noise patches over the hide ------------------------
  if (s.mottle > 0.05 && role !== 'hand') {
    const thr = 1 - s.mottle * 0.45;
    for (const v of G.list()) {
      if (!HIDE.has(v[3]) || !G.lateral(v[0], v[1], v[2])) continue;
      if (vnoise(seed, v[0], v[1], v[2], 3.2) > thr) v[3] = SLOTS.MARK;
    }
  }

  // ---- claws: the fingertips (a hand's LOW z) and the toes ------------------
  if (s.claws > 0.05) {
    if (role === 'hand') {
      const rows = 1 + Math.round(s.claws * 2);
      for (const v of G.list()) if (v[2] < rows && G.lateral(v[0], v[1], v[2])) v[3] = SLOTS.CLAW;
    } else if (role === 'foot') {
      // The front of the foot at the sole: the toes.
      const ys = G.list().map(v => v[1]);
      const yMin = Math.min(...ys);
      const reach = 1 + Math.round(s.claws * 2);
      for (const v of G.list())
        if (v[2] <= 1 && v[1] < yMin + reach) v[3] = SLOTS.CLAW;
    }
  }
  // The human's nails (below this resolution) read as claws' roots.
  if (role === 'hand')
    for (const v of G.list()) if (v[3] === A.NAIL) v[3] = SLOTS.CLAW;

  return keepLargest(G.list(), size);
}

// =============================================================================
// the extras: horns (ride the head) and the tail (rides the torso). SCENE
// coordinates (x right, y BACK, z up).
// =============================================================================

export function extrasWant(g) {
  const s = g.demon;
  return s.horns > 0.02 || s.tail > 0.02;
}

export function extras(g, ctx) {
  const s = g.demon;
  const { K, body, head, torso } = ctx;
  const headOut = new Map(), torsoOut = new Map();
  const free = (X, Y, Z) => !body.has(K(X, Y, Z)) && !headOut.has(K(X, Y, Z)) &&
                            !torsoOut.has(K(X, Y, Z));
  const addH = (X, Y, Z, c) => { if (free(X, Y, Z)) headOut.set(K(X, Y, Z), [X, Y, Z, c]); };
  const addT = (X, Y, Z, c) => { if (free(X, Y, Z)) torsoOut.set(K(X, Y, Z), [X, Y, Z, c]); };

  // ---- horns: a 2x2 shaft from the top of the skull, tapering to one cell --
  if (s.horns > 0.02) {
    const H = extent(head);
    const hc = sceneCells(head);
    const topAt = new Map();
    for (const [x, y, z] of hc) {
      const k = x * 4096 + y;
      if (!topAt.has(k) || z > topAt.get(k)) topAt.set(k, z);
    }
    const halfW = (H.x1 - H.x0 + 1) / 2, halfD = (H.y1 - H.y0 + 1) / 2;
    // Horn length scales with the head, so an imp's nubs are an imp's.
    const len = Math.max(2, Math.round(s.horns * (H.z1 - H.z0 + 1) * 0.9));
    for (const side of [-1, 1]) {
      const bx = Math.floor(H.cx + side * halfW * (0.25 + s.hornSpread * 0.45));
      const by = Math.floor(H.cy - halfD * 0.25);
      // The base sits on the skull's top at that column (or the nearest
      // column that has one).
      let bz;
      for (let r = 0; r <= 3 && bz === undefined; r++)
        for (const [dx, dy] of [[0, 0], [0, r], [0, -r], [r * -side, 0]])
          if (bz === undefined && topAt.has((bx + dx) * 4096 + (by + dy)))
            bz = topAt.get((bx + dx) * 4096 + (by + dy));
      if (bz === undefined) continue;
      let prev = [bx, by, bz];
      for (let i = 1; i <= len; i++) {
        const t = i / len;
        // Up first, then (with curl) back and down; outward with spread.
        const ang = t * s.hornCurl * 2.2;          // radians swept back
        const up = Math.cos(ang), back = Math.sin(ang);
        const p = [Math.round(bx + side * t * len * (0.15 + s.hornSpread * 0.35)),
                   Math.round(by + back * i * 0.9),
                   Math.round(bz + up * i * 0.9 + 1)];
        const tip = t > 0.7;
        const c = tip ? SLOTS.HORN_TIP : SLOTS.HORN;
        line6(prev, p, (x, y, z) => {
          addH(x, y, z, c);
          // Thick at the root: a 2x2 section for the first half.
          if (t <= 0.5) {
            addH(x + side, y, z, c);
            addH(x, y + 1, z, c);
            addH(x + side, y + 1, z, c);
          }
        });
        if (i === 1) {
          addH(bx, by, bz + 1, SLOTS.HORN);
          addH(bx + side, by, bz + 1, SLOTS.HORN);
          addH(bx, by + 1, bz + 1, SLOTS.HORN);
          addH(bx + side, by + 1, bz + 1, SLOTS.HORN);
        }
        prev = p;
      }
    }
  }

  // ---- the tail: out of the small of the back, down, then curling up -------
  if (s.tail > 0.02) {
    const T = extent(torso);
    const backAt = new Map();
    for (const [x, y, z] of sceneCells(torso)) {
      const k = x * 4096 + z;
      if (!backAt.has(k) || y > backAt.get(k)) backAt.set(k, y);
    }
    const tx = Math.floor(T.cx);
    const tz = T.z0 + 1;
    let yb;
    for (let z = tz; z <= tz + 3 && yb === undefined; z++)
      if (backAt.has(tx * 4096 + z)) yb = backAt.get(tx * 4096 + z);
    if (yb !== undefined) {
      const tH = T.z1 - T.z0 + 1;
      const len = Math.max(4, Math.round(s.tail * tH * 1.4));
      let prev = [tx, yb, tz];
      for (let i = 1; i <= len; i++) {
        const t = i / len;
        // Back and down, then the last third curls up.
        const p = [tx,
                   Math.round(yb + i * 0.8),
                   Math.round(tz - Math.sin(t * Math.PI) * len * 0.35 +
                              (t > 0.66 ? (t - 0.66) * len * 0.9 : 0))];
        line6(prev, p, (x, y, z) => {
          addT(x, y, z, A_SKIN_SHADE(ctx));
          if (t < 0.6) { addT(x + 1, y, z, A_SKIN_SHADE(ctx)); addT(x, y, z + 1, A_SKIN_SHADE(ctx));
                         addT(x + 1, y, z + 1, A_SKIN_SHADE(ctx)); }
        });
        prev = p;
      }
      // The root, solid against the back.
      for (let dz = 0; dz <= 1; dz++)
        for (let dx = 0; dx <= 1; dx++) addT(tx + dx, yb + 1, tz + dz, A_SKIN_SHADE(ctx));
    }
  }

  return { head: [...headOut.values()], torso: [...torsoOut.values()] };
}

/** The tail is hide, the shaded tone (it hangs behind, away from the light). */
const A_SKIN_SHADE = ctx => ctx.ART.SKIN_SHADE;

// keep the unused-import linter quiet: rnd is part of the toolbox contract
void rnd;
