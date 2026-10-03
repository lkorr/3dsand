/* automaton.js — THE AUTOMATON RACE: steampunk clockwork men on the human rig.
 *
 * WHAT AN AUTOMATON IS, TO THE GENERATOR. The human body -- the SAME fifteen
 * limb boxes, anchors, sockets, chains and clips, so every helm, cuirass and
 * blade in the game fits it unchanged (test_mobgen section O pins that) --
 * plated in riveted brass, with gearwork showing through cut-away windows,
 * copper pipes along the limbs, leather bellows at the joints, a furnace
 * glowing through a grate in the chest, gauges, lamp eyes behind glass
 * lenses, and off the body a chimney, a top hat, a gear crest, exhaust stacks
 * and the WIND-UP KEY in its back.
 *
 * WHAT IT IS MADE OF (materials.json, the robot rows). Plating is `brass`
 * (hardness 70, `shell`: a sword takes a ninth of the kerf it takes from
 * skin); under it `clockwork` (the works, swimming in oil), an `iron` frame
 * for a skeleton, a `boiler` in the chest, a `cogitator` (brainHp) at the
 * core of the head. Eye lamps and the furnace glow are `gem_arcane` (its
 * emission is the light, the art colour its colour), lenses and gauge faces
 * `glass`, joints `leather`. IT BLEEDS OIL: oil is flammable, so a holed
 * automaton is a fire waiting for a spark -- and brass sparks when an edge
 * rings off it. A blow that reaches the boiler vents steam; breaching it, or
 * killing the automaton, lets the whole head of steam go.
 *
 * brass / clockwork / boiler / cogitator are ids > 127, which a .vox cannot
 * paint, so they are written as STAND-INS (STAND_INS) and the sidecar's
 * anatomy.becomes rewrites them at load (mob.cpp), as the snake's gland.
 *
 * HUMANS ARE UNTOUCHED: every entry point is reached only for
 * `genome.body.race === 'automaton'`, and every gene here is race-gated, so a
 * human's mutation, roll and cross loops skip it WITHOUT a random draw.
 * PURE AND NODE-RUNNABLE; imports only robotkit.js.
 */

'use strict';

import { rnd, strHash, clamp, vnoise, grid, disc, keepLargest, readFace,
         line6, cylinder, sceneCells, extent } from './robotkit.js';

export const RACE = 'automaton';
export const FOLDER = 'automaton';
export const NEW_NAME = 'cog';
export const BLURB = 'an automaton: a clockwork man in riveted brass -- gears ' +
  'behind cut-away windows, a furnace in the chest, lamp eyes, a wind-up key. ' +
  'Bleeds oil, vents steam, takes a blade the way plate does. Same limbs and ' +
  'proportions, so armour and weapons still fit.';

// =============================================================================
// art slots and materials
// =============================================================================

/** The automaton's own art slots, continuing mobgen's ART table downward
 *  past the sylvan's (236). Written into a palette only by an automaton. */
export const SLOTS = {
  RIVET: 235,       // rivet heads, bezels, collars, bands: bright brass
  PIPE: 234,        // pipes and stacks: copper by default
  GEAR: 233,        // gear teeth and spokes
  GEAR_DARK: 232,   // between the teeth, grilles, soot, bolt holes
  PATINA: 231,      // verdigris
  GLASS: 230,       // lenses, gauge faces, the diver's porthole
  FIRE: 229,        // the furnace glow (gem_arcane: it gives light)
  JOINT: 228,       // leather bellows at the joints
};

export const COLOR_SLOTS = {
  rivet: SLOTS.RIVET, pipe: SLOTS.PIPE, gear: SLOTS.GEAR,
  gearDark: SLOTS.GEAR_DARK, patina: SLOTS.PATINA, glass: SLOTS.GLASS,
  fire: SLOTS.FIRE, joint: SLOTS.JOINT,
};

/** What each kind of surface is made of, by name (resolved at write time). */
export const MATS = {
  plate: 'brass',
  glow: 'gem_arcane',
  glass: 'glass',
  joint: 'leather',
};
/** What a slot no row below names is made of: plate. */
export const DEFAULT_MAT = MATS.plate;

/** Ids > 127 are painted as a stand-in and rewritten at load (anatomy.becomes,
 *  mob.cpp). Each stand-in is a material no generated body otherwise paints,
 *  and the four are distinct, so the rewrite is unambiguous. */
export const STAND_INS = {
  brass: 'cactus_rib',
  clockwork: 'cactus_flesh',
  boiler: 'cap_red',
  cogitator: 'cap_tan',
};

/** What runs out of a cut, and how much (sidecar `bleed`). Oil: flammable. */
export const BLEED = { material: 'oil', perDamage: 1.4 };

/** The inside (anatomy.js format; mobgen swaps in the stand-ins). Disjoint
 *  from the SURFACE materials (brass, glass, gem_arcane, leather): a kept
 *  surface voxel the recipe also puts underneath is rewritten as interior. */
export const ANATOMY = {
  layers: [
    { material: 'brass', depth: 1, keep: true },
    { material: 'clockwork', depth: 2 },
    { material: 'iron' },
  ],
  garments: [],
  limbs: {
    head: {
      layers: [
        { material: 'brass', depth: 1, keep: true },
        { material: 'iron', depth: 1 },
        { material: 'clockwork', depth: 1 },
        { material: 'cogitator' },
      ],
    },
    torso: {
      carve: [
        { material: 'boiler', where: 'clockwork',
          box: { x: [0.26, 0.74], y: [0.4, 0.76], z: [0.4, 1] } },
        { material: 'boiler', where: 'iron',
          box: { x: [0.26, 0.74], y: [0.4, 0.76], z: [0.4, 1] } },
      ],
    },
  },
};

export const ANATOMY_NOTES = {
  '//': 'AN AUTOMATON: brass plating over the clockwork (wheels and springs in ' +
        'oil) over an iron frame, a boiler in the chest behind the furnace ' +
        'grate and a cogitator (brainHp) at the core of the head. The surface ' +
        'is brass, glass lenses, gem_arcane lamps and furnace glow, leather ' +
        'joints -- all KEPT; none of them appears below. brass, clockwork, ' +
        'boiler and cogitator are material ids above 127, which a .vox cannot ' +
        'paint, so the recipe names their stand-ins and `becomes` rewrites ' +
        'them at load (mob.cpp, as snake.json\'s venom gland).',
};

/** Art slot -> material name. Every slot a body cell can carry is named, so
 *  nothing falls back to skin. */
export function slotMaterialNames(g, ART) {
  const m = {};
  for (const s of [ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT, ART.NAIL,
                   ART.CLOTH, ART.CLOTH_SHADE, ART.HAIR, ART.HAIR_SHADE,
                   ART.HAIR_WISP1, ART.HAIR_WISP2, ART.HAIR_WISP3,
                   SLOTS.RIVET, SLOTS.PIPE, SLOTS.GEAR, SLOTS.GEAR_DARK,
                   SLOTS.PATINA])
    m[s] = MATS.plate;
  m[SLOTS.GLASS] = MATS.glass;
  m[SLOTS.FIRE] = MATS.glow;
  m[SLOTS.JOINT] = MATS.joint;
  m[ART.EYE] = g.automaton && g.automaton.glow ? MATS.glow : MATS.glass;
  return m;
}

// =============================================================================
// the genome
// =============================================================================

export function defaultGenes() {
  return {
    face: 'lamp',      // a face preset (FACES); the numbers below are the truth
    glow: true,        // eye lamps are lit (gem_arcane), or dark glass
    lens: 1,           // eye lens size: 0 pinholes .. 2 saucers
    bezel: 0.5,        // the brass ring round each lens
    eyeDepth: 0.4,     // how far the lenses sit back in the face
    grille: 0.5,       // the mouth grille: slots
    jaw: 0.4,          // a hinged-jaw seam round the lower face
    brow: 0.3,         // a plate ridge over the eyes
    monocle: 0,        // one lens much bigger than the other
    porthole: 0,       // a diving-helmet window over the whole face
    furnace: 0,        // eye slits and a glowing furnace mouth
    cyclops: false,    // one central lens
    plates: 0.5,       // how finely the plating is divided into panels
    rivets: 0.6,       // rivets along the seams
    gears: 0.4,        // cut-away windows onto the gearwork
    pipes: 0.35,       // copper pipes along the limbs and up the back
    joints: 0.6,       // leather bellows at elbows, knees and neck
    boiler: 0.6,       // the furnace grate in the chest
    gauge: 0.5,        // pressure gauges on the chest
    patina: 0.15,      // verdigris
    wear: 0.2,         // dents and scratches
    polish: 0.4,       // bright edges where the plating faces up
    stack: 'none',     // a smokestack preset (STACKS); the numbers rule
    stackHead: 0,      // a chimney out of the top of the head
    hat: 0,            // a top hat
    crest: 0,          // a gear-wheel crest along the skull
    stackBack: 0,      // twin exhaust stacks up the back
    key: 0.6,          // the wind-up key in the back
  };
}


/** Colour sets a roll picks WHOLE: a plating, a trim, a lamp and a fire.
 *  The nail, gear-shadow, glass, verdigris and bellows colours are the
 *  race's own and NOT in any set: every automaton shares them, which is
 *  five fewer colours each in the engine's one 255-colour art palette. */
export const PLATINGS = {
  brass:    { skin: '#b8862a', skinShade: '#7a5418', skinLight: '#e8c060', rivet: '#e8c060', gear: '#7a5418', hair: '#7a5418', hairShade: '#5a3c12' },
  copper:   { skin: '#b86a3c', skinShade: '#7e4424', skinLight: '#e09468', rivet: '#e09468', gear: '#7e4424', hair: '#7e4424', hairShade: '#5a3018' },
  bronze:   { skin: '#9a7038', skinShade: '#664820', skinLight: '#c49a5a', rivet: '#c49a5a', gear: '#664820', hair: '#664820', hairShade: '#4a3416' },
  iron:     { skin: '#6e7176', skinShade: '#46484c', skinLight: '#9a9ea4', rivet: '#9a9ea4', gear: '#46484c', hair: '#46484c', hairShade: '#303236' },
  gunmetal: { skin: '#4c5258', skinShade: '#30353a', skinLight: '#767e86', rivet: '#767e86', gear: '#30353a', hair: '#30353a', hairShade: '#202326' },
  gilded:   { skin: '#e0b84a', skinShade: '#a8822a', skinLight: '#fff0a0', rivet: '#fff0a0', gear: '#a8822a', hair: '#a8822a', hairShade: '#7c5e1e' },
  nickel:   { skin: '#b8bcc0', skinShade: '#80868c', skinLight: '#e6eaee', rivet: '#e6eaee', gear: '#80868c', hair: '#80868c', hairShade: '#5a5e62' },
  rusted:   { skin: '#8a5a34', skinShade: '#5a3820', skinLight: '#b07a48', rivet: '#b07a48', gear: '#5a3820', hair: '#5a3820', hairShade: '#3e2616' },
};
export const TRIMS = {
  copper:  { pipe: '#b8673a', cloth: '#5a5650', clothShade: '#3e3b37' },
  brass:   { pipe: '#c9a24a', cloth: '#4a3a2a', clothShade: '#32271c' },
  iron:    { pipe: '#5c6066', cloth: '#6a5a40', clothShade: '#4a3e2c' },
  lacquer: { pipe: '#2a2624', cloth: '#7a2a24', clothShade: '#521c18' },
  verdant: { pipe: '#4f8a72', cloth: '#3a4a3a', clothShade: '#283428' },
};
/** Lamp colours (the eye swatches on the Characters page). */
export const GLOWS = {
  // The SYLVAN's glow hexes where a lamp is the same colour (sylvan.js
  // GLOWS), so the engine's shared art palette holds one entry for both.
  amber: '#ffc24a', gaslight: '#f4ffd8', ember: '#ff6a3a', red: '#ff3a3a',
  radium: '#b6ff6a', tesla: '#6a9cff', violet: '#c08aff', white: '#cfe4ff',
};
export const FIRES = {
  coal: { fire: '#ff8a2a' }, whitehot: { fire: '#fff0b0' },
  gas: { fire: '#5ab0ff' }, alchemical: { fire: '#7aff6a' },
  infernal: { fire: '#ff3a2a' },
};
/** The race's default look: brass, copper trim, a coal fire, an amber lamp,
 *  and the colours every automaton shares (SHARED_COLORS). */
export const SHARED_COLORS = {
  nail: '#4a3a22', gearDark: '#2e2010', patina: '#5fa58a', glass: '#cfe6e0',
  joint: '#3a2a1e',
};
export const DEFAULT_COLORS = {
  ...PLATINGS.brass, ...TRIMS.copper, ...FIRES.coal, eye: GLOWS.amber,
  ...SHARED_COLORS,
};
/** What a colour roll picks from, one entry of each. */
const LAMPS = Object.fromEntries(Object.entries(GLOWS)
  .map(([k, v]) => [k, { eye: v }]));
export const COLOR_SETS = [PLATINGS, TRIMS, FIRES, LAMPS];

/** A STOCK list the Characters page's colour stepper walks (the hair stepper
 *  on a human): platings, darkest to lightest. */
export const STOCK = [
  ['gunmetal', PLATINGS.gunmetal], ['rusted', PLATINGS.rusted],
  ['iron', PLATINGS.iron], ['bronze', PLATINGS.bronze],
  ['copper', PLATINGS.copper], ['brass', PLATINGS.brass],
  ['nickel', PLATINGS.nickel], ['gilded', PLATINGS.gilded],
];
export const STOCK_LABEL = 'plating';

/** Within-box build offsets only (the human's boxes are the armour's). An
 *  automaton is barrel-chested and square-jawed. */
export const BUILD_DELTA = {
  'shape.waist': 0.06,
  'shape.chest': 0.05,
  'shape.chestDepth': 0.05,
  'head.jaw': 0.08,
};

const FACE_KEYS = ['lens', 'bezel', 'eyeDepth', 'grille', 'jaw', 'brow',
                   'monocle', 'porthole', 'furnace', 'cyclops'];
export const FACES = {
  lamp:      { lens: 1.1, bezel: 0.6, eyeDepth: 0.4, grille: 0.55, jaw: 0.5, brow: 0.3, monocle: 0, porthole: 0, furnace: 0, cyclops: false },
  gentleman: { lens: 0.8, bezel: 0.5, eyeDepth: 0.3, grille: 0.3, jaw: 0.35, brow: 0.6, monocle: 1, porthole: 0, furnace: 0, cyclops: false },
  diver:     { lens: 0.8, bezel: 0.4, eyeDepth: 0.2, grille: 0, jaw: 0.2, brow: 0, monocle: 0, porthole: 1, furnace: 0, cyclops: false },
  furnace:   { lens: 1, bezel: 0.3, eyeDepth: 0.5, grille: 0, jaw: 0.6, brow: 0.8, monocle: 0, porthole: 0, furnace: 1, cyclops: false },
  cyclops:   { lens: 1.6, bezel: 0.8, eyeDepth: 0.5, grille: 0.7, jaw: 0.4, brow: 0.4, monocle: 0, porthole: 0, furnace: 0, cyclops: true },
  mask:      { lens: 0.35, bezel: 0, eyeDepth: 0.9, grille: 0, jaw: 0, brow: 0.2, monocle: 0, porthole: 0, furnace: 0, cyclops: false },
};
export const FACE_ORDER = Object.keys(FACES);

const STACK_KEYS = ['stackHead', 'hat', 'crest', 'stackBack'];
export const STACKS = {
  none:    { stackHead: 0,    hat: 0,   crest: 0,   stackBack: 0 },
  chimney: { stackHead: 0.6,  hat: 0,   crest: 0,   stackBack: 0 },
  tophat:  { stackHead: 0,    hat: 0.8, crest: 0,   stackBack: 0 },
  stovepipe: { stackHead: 0.5, hat: 0.6, crest: 0,  stackBack: 0 },
  crest:   { stackHead: 0,    hat: 0,   crest: 0.7, stackBack: 0 },
  exhaust: { stackHead: 0,    hat: 0,   crest: 0,   stackBack: 0.7 },
  works:   { stackHead: 0.35, hat: 0,   crest: 0.45, stackBack: 0.5 },
};
export const STACK_ORDER = Object.keys(STACKS);

export function applyFace(genome, style) {
  const f = FACES[style];
  if (!f) return genome;
  genome.automaton.face = style;
  for (const k of FACE_KEYS) genome.automaton[k] = f[k];
  return genome;
}
export function applyStack(genome, style) {
  const f = STACKS[style];
  if (!f) return genome;
  genome.automaton.stack = style;
  for (const k of STACK_KEYS) genome.automaton[k] = f[k];
  return genome;
}

/** The Characters page's named pickers for this race: a dropdown + stepper
 *  each, above the sliders (breed.js). `apply` sets every gene it governs. */
export const PICKERS = [
  { key: 'face', path: 'automaton.face', label: 'face', order: FACE_ORDER,
    apply: applyFace,
    title: 'a starting face: lamp eyes, the gentleman\'s monocle, a diving ' +
           'helmet\'s porthole, a furnace, a cyclops, a smooth mask' },
  { key: 'stack', path: 'automaton.stack', label: 'stacks', order: STACK_ORDER,
    apply: applyStack,
    title: 'what stands up off the body: a chimney, a top hat (with or ' +
           'without a stovepipe), a gear crest, exhaust stacks up the back' },
];

/** What becoming an automaton does to the human genes beside the race's own:
 *  no hair, no beard, no fuzz (the plating is the scalp). */
export function onBecome(genome, h) {
  if (!h.isLocked('hair.style')) h.applyHairStyle(genome, 'bald');
  if (!h.isLocked('face.beardStyle')) h.applyBeardStyle(genome, 'clean');
  if (!h.isLocked('hair.fuzz')) genome.hair.fuzz = 0;
}

/** A random automaton on top of a random human (after applyRace): a face and
 *  stacks picked whole, every number jittered, a lit lamp most of the time. */
export function randomize(g, r, isLocked) {
  if (!isLocked('automaton.face')) applyFace(g, r.pick(FACE_ORDER));
  if (!isLocked('automaton.stack')) applyStack(g, r.pick(STACK_ORDER));
  for (const spec of GENE_SPECS) {
    if (spec.kind || isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    const v = g.automaton[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    g.automaton[k] = clamp(v, spec.min, spec.max);
  }
  if (!isLocked('automaton.glow')) g.automaton.glow = r() < 0.9;
  if (!isLocked('automaton.key')) g.automaton.key = r() < 0.75 ? 0.3 + r() * 0.7 : 0;
  return g;
}

export const GENE_GROUPS = [
  { key: 'autoface', title: 'automaton face', race: 'automaton',
    note: 'Lamp eyes behind glass lenses in brass bezels, a mouth grille, a ' +
          'hinged jaw. Pick a face to set them all, then drag. The lamps give ' +
          'light; their colour is the eye colour.' },
  { key: 'autobody', title: 'plating and works', race: 'automaton',
    note: 'Riveted plates over the clockwork. Seams and pipes are cut one cell ' +
          'into the plating, never out of it, so armour still fits. The ' +
          'furnace grate and the gear windows are where a blade reaches the ' +
          'works soonest.' },
  { key: 'autostack', title: 'stacks, hat and key', race: 'automaton',
    note: 'What stands off the body: a chimney, a top hat, a gear crest, ' +
          'exhaust stacks, the wind-up key. Bloodless parts -- knocking a ' +
          'chimney off costs the machine nothing.' },
];

const S = (path, label, group, hint, extra = {}) => ({
  path: 'automaton.' + path, label, group, min: 0, max: 1, step: 0.05,
  sigma: 0.2, race: 'automaton', hint, ...extra });

export const GENE_SPECS = [
  { path: 'automaton.face', label: 'face', group: 'autoface', kind: 'enum',
    choices: FACE_ORDER, sigma: 0.15, race: 'automaton',
    hint: 'A starting face: lamp, gentleman (a monocle), diver (a porthole), ' +
          'furnace (slit eyes, a glowing mouth), cyclops, mask.' },
  { path: 'automaton.glow', label: 'lit lamps', group: 'autoface', kind: 'bool',
    sigma: 0, race: 'automaton',
    hint: 'The eye lamps glow and light the dark. Off, they are dark glass ' +
          'tinted the eye colour.' },
  S('lens', 'lens size', 'autoface', 'Pinholes to saucers.', { max: 2, sigma: 0.3 }),
  S('bezel', 'bezel', 'autoface', 'The bright brass ring round each lens.'),
  S('eyeDepth', 'lens depth', 'autoface', 'How far back in the face the lenses sit.'),
  S('grille', 'mouth grille', 'autoface', 'Slots cut across the mouth: none to four.'),
  S('jaw', 'jaw seam', 'autoface', 'A seam round the lower face, so the jaw ' +
    'reads as a hinged plate.'),
  S('brow', 'brow plate', 'autoface', 'A ridge of plate over the eyes; high ' +
    'values stand it proud.'),
  S('monocle', 'monocle', 'autoface', 'One lens grows, the other shrinks: the ' +
    'gentleman\'s eyeglass.', { roll: 0.25 }),
  S('porthole', 'porthole', 'autoface', 'A diving helmet\'s round window over ' +
    'the whole face, bolted round, with the lamps behind it.', { roll: 0.2 }),
  S('furnace', 'furnace mouth', 'autoface', 'Slit eyes and a mouth that is a ' +
    'glowing furnace grate.', { roll: 0.2 }),
  { path: 'automaton.cyclops', label: 'cyclops', group: 'autoface',
    kind: 'bool', sigma: 0, race: 'automaton', roll: 0.15,
    hint: 'One great central lens instead of two.' },

  S('plates', 'panels', 'autobody', 'How finely the plating is divided: a few ' +
    'broad plates to many small ones.'),
  S('rivets', 'rivets', 'autobody', 'Rivets along every seam: none to close-set.'),
  S('gears', 'gear windows', 'autobody', 'Cut-away windows onto the turning ' +
    'gearwork, on the chest, the back and the limbs.'),
  S('pipes', 'pipes', 'autobody', 'Copper pipes along the limbs and up the ' +
    'back, standing proud of grooves cut beside them.'),
  S('joints', 'bellows', 'autobody', 'Corrugated leather bellows at the ' +
    'elbows, knees and neck.'),
  S('boiler', 'furnace grate', 'autobody', 'The furnace glowing through a grate ' +
    'in the chest. It is a lamp: it lights the ground in front.'),
  S('gauge', 'gauges', 'autobody', 'Pressure gauges on the chest: one, then two.'),
  S('patina', 'verdigris', 'autobody', 'Green corrosion, thickest on the pipes ' +
    'and on what faces up.'),
  S('wear', 'wear', 'autobody', 'Dents and bright scratches.'),
  S('polish', 'polish', 'autobody', 'Bright edges where the plating faces up.'),

  { path: 'automaton.stack', label: 'stacks', group: 'autostack', kind: 'enum',
    choices: STACK_ORDER, sigma: 0.15, race: 'automaton',
    hint: 'A starting set: none, chimney, tophat, stovepipe (a hat with a ' +
          'chimney), crest, exhaust (up the back), works (all of it).' },
  S('stackHead', 'chimney', 'autostack', 'A chimney out of the top of the ' +
    'head (or the hat): stub to tall.'),
  S('hat', 'top hat', 'autostack', 'A top hat: its height.', { roll: 0.3 }),
  S('crest', 'gear crest', 'autostack', 'Half a gear wheel standing along the ' +
    'skull, teeth up.', { roll: 0.3 }),
  S('stackBack', 'exhaust stacks', 'autostack', 'Twin exhaust pipes up the ' +
    'back, past the shoulders.', { roll: 0.3 }),
  S('key', 'wind-up key', 'autostack', 'The key in the back that winds the ' +
    'mainspring: its size. 0 is none.'),
];

export const COLOR_SPECS = [
  ['rivet', 'rivets and bezels', 'Rivet heads, lens bezels, bands and collars.'],
  ['pipe', 'pipes', 'Pipes, chimneys and exhaust stacks.'],
  ['gear', 'gears', 'The gear teeth and spokes behind the windows.'],
  ['gearDark', 'gear shadow', 'Between the teeth; grilles, soot, bolt holes.'],
  ['patina', 'verdigris', 'The green corrosion.'],
  ['glass', 'glass', 'Lenses, gauge faces, a porthole.'],
  ['fire', 'furnace', 'The furnace glow behind the chest grate. It gives light.'],
  ['joint', 'bellows', 'The leather bellows at the joints.'],
];
export const COLOR_LABELS = {
  skin: ['plating', 'The plating. Most of the body is this.'],
  skinShade: ['plating, shaded', 'Plating turned from the light, and the seams.'],
  skinLight: ['plating, polished', 'Bright edges and scratches.'],
  hair: ['sculpted hair', 'Hair, if you give an automaton any: it is metal.'],
  hairShade: ['sculpted hair, shaded', 'The same, turned from the light.'],
  eye: ['lamp', 'The eye lamps, and the colour of the light they give.'],
  cloth: ['belt and hat', 'The girdle at the waist, and a top hat.'],
  clothShade: ['belt and hat, shaded', 'The same, turned from the light.'],
  nail: ['fingertips', 'Dark iron at the finger and toe tips.'],
};

export const PRESETS = {
  automaton: { face: 'lamp', stack: 'none' },
  tinker: {
    face: 'gentleman', stack: 'tophat',
    body: { heightM: 1.7 },
    colors: { ...PLATINGS.brass, ...TRIMS.copper, ...FIRES.coal, eye: GLOWS.amber },
    automaton: { key: 0.7, gauge: 0.8, pipes: 0.3 },
  },
  boilerman: {
    face: 'furnace', stack: 'exhaust',
    body: { heightM: 1.86, shoulderWidth: 14, bodyDepth: 10, limbWidth: 6 },
    shape: { chest: 1.15, shoulder: 1.12, armGirth: 1.05 },
    colors: { ...PLATINGS.iron, ...TRIMS.copper, ...FIRES.coal, eye: GLOWS.ember },
    automaton: { boiler: 1, pipes: 0.7, rivets: 0.9, wear: 0.5, key: 0 },
  },
  deepdiver: {
    face: 'diver', stack: 'none',
    body: { heightM: 1.78 },
    colors: { ...PLATINGS.copper, ...TRIMS.brass, ...FIRES.gas, eye: GLOWS.gaslight },
    automaton: { patina: 0.55, rivets: 0.8, gears: 0.25, key: 0.5 },
  },
  clocksmith: {
    face: 'cyclops', stack: 'crest',
    body: { sex: 'female', heightM: 1.64 },
    colors: { ...PLATINGS.gilded, ...TRIMS.lacquer, ...FIRES.whitehot, eye: GLOWS.tesla },
    automaton: { gears: 0.9, plates: 0.75, polish: 0.8, key: 0.9 },
  },
  derelict: {
    face: 'mask', stack: 'chimney',
    body: { heightM: 1.75 },
    colors: { ...PLATINGS.rusted, ...TRIMS.iron, ...FIRES.infernal, eye: GLOWS.red },
    automaton: { wear: 0.9, patina: 0.4, polish: 0, gears: 0.6, key: 0.4 },
  },
};
export const PRESET_ORDER = Object.keys(PRESETS);

// =============================================================================
// the surface: plates, seams, rivets, gear windows, pipes, bellows, the
// furnace, gauges, wear, verdigris and the face
//
// On the SHIPPED lattice, part-local ([x, y, z, slot], front is LOW y, z up),
// after the upscale and the shoulder round. It may remove cells and add them
// inside the part's own box; it never grows the box.
// =============================================================================

/**
 * @param g     normalised genome (race automaton)
 * @param role  'trunk' | 'head' | 'limb' | 'hand' | 'foot'
 * @param name  the part's name (a hash seed only)
 * @param cells [[x,y,z,slot]]
 * @param size  the part's shipped box
 * @param info  { ART, mn, eyeRow, mouthRow, mouthDx, neckTop } -- the head
 *              gets `info.face` back
 */
export function surfacePass(g, role, name, cells, size, info) {
  const s = g.automaton, A = info.ART;
  const G = grid(cells, size);
  const { sx, sy, sz, cx, cy } = G;
  const seed = strHash(name) ^ 0xa070;
  const PLATE = new Set([A.SKIN_BASE, A.SKIN_SHADE, A.SKIN_LIGHT]);
  const plateOf = v => (v[1] + 0.5 > cy ? A.SKIN_SHADE : A.SKIN_BASE);
  // Cells a feature owns (the face, a window, the grate, a pipe): later
  // passes neither groove nor repaint them.
  const owned = new Set();
  const own = (x, y, z) => owned.add(G.idx(x, y, z));
  const isOwned = (x, y, z) => owned.has(G.idx(x, y, z));
  const paintOwn = (x, y, z, c) => { if (G.paint(x, y, z, c)) own(x, y, z); };
  const free = v => PLATE.has(v[3]) && !isOwned(v[0], v[1], v[2]);

  // THE HUMAN'S SHORTS ARE PLATE on an automaton, all but a BELT: the top
  // rows of the cloth on the hips keep the CLOTH colours (a gunmetal girdle
  // with a bright buckle). Before anything reads `front`.
  {
    const CL = new Set([A.CLOTH, A.CLOTH_SHADE]);
    let top = -1;
    if (name === 'hips')
      for (const v of G.list()) if (CL.has(v[3])) top = Math.max(top, v[2]);
    for (const v of G.list()) {
      if (!CL.has(v[3])) continue;
      if (name === 'hips' && top >= 0 && v[2] >= top - 2) {
        own(v[0], v[1], v[2]);
        if (v[1] + 0.5 < cy && Math.abs(v[0] + 0.5 - cx) < 1.6 && v[2] >= top - 1)
          v[3] = SLOTS.RIVET;
      } else v[3] = plateOf(v);
    }
  }
  const front = (() => {
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y < m.get(k)) m.set(k, y);
    }
    return (x, z) => m.get(x * 4096 + z);
  })();
  const back = (() => {
    const m = new Map();
    for (const [x, y, z] of G.list()) {
      const k = x * 4096 + z;
      if (!m.has(k) || y > m.get(k)) m.set(k, y);
    }
    return (x, z) => m.get(x * 4096 + z);
  })();

  // ---- the face first: it decides what the rest must leave alone ----------
  let face = null;
  if (role === 'head') {
    face = carveFace(g, A, G, info, seed, paintOwn, plateOf);
    // The face's own region is the face's: no seams or rivets across it.
    for (const [x, y, z] of G.list())
      if (y + 0.5 < cy && z + 0.5 > face.chinZ && z + 0.5 < face.browZ + 2 &&
          Math.abs(x + 0.5 - cx) < face.halfW) own(x, y, z);
  }

  // ---- the furnace grate and the gauges (the chest) -------------------------
  if (role === 'trunk' && name === 'torso') {
    if (s.boiler > 0.05) {
      const R = Math.min(2 + s.boiler * 4.2, Math.min(sx, sz) * 0.33);
      const pz = sz * 0.56, px = cx;
      disc(G, front, -1, px, pz, R * 1.15, R, 2,
           (d, a, x, z) => (z % 3 === 0 && d < 0.92 ? SLOTS.GEAR_DARK : SLOTS.FIRE),
           (d, a) => (Math.abs(Math.sin(a * 4)) < 0.2 ? SLOTS.GEAR_DARK : SLOTS.RIVET),
           1.4);
      for (const v of G.list())
        if (Math.hypot((v[0] + 0.5 - px) / (R * 1.15), (v[2] + 0.5 - pz) / R) <
            1 + 1.6 / R && v[1] + 0.5 < cy) own(v[0], v[1], v[2]);
    }
    if (s.gauge > 0.1) {
      const n = s.gauge > 0.7 ? 2 : 1;
      for (let i = 0; i < n; i++) {
        const side = i === 0 ? -1 : 1;
        const r = 1.1 + s.gauge * 1.4;
        const px = cx + side * sx * 0.24, pz = sz * 0.8;
        const needle = rnd(seed, 41, i) * Math.PI * 1.4 - Math.PI * 0.2;
        disc(G, front, -1, px, pz, r, r, 0,
             (d, a) => {
               const off = Math.abs(Math.sin(a - needle)) * d * r;
               return d < 0.3 || (off < 0.55 && Math.cos(a - needle) > 0)
                 ? SLOTS.GEAR_DARK : SLOTS.GLASS;
             },
             () => SLOTS.RIVET, 0.9);
        for (const v of G.list())
          if (Math.hypot(v[0] + 0.5 - px, v[2] + 0.5 - pz) < r + 1.2 &&
              v[1] + 0.5 < cy) own(v[0], v[1], v[2]);
      }
    }
  }

  // ---- gear windows: the works, turning behind a bezel ----------------------
  const nWin = role === 'trunk' ? Math.round(s.gears * (name === 'torso' ? 2.6 : 1.4))
             : role === 'limb' ? (rnd(seed, 51) < s.gears * 0.8 ? 1 : 0) : 0;
  for (let i = 0; i < nWin; i++) {
    const onBack = role === 'trunk' ? i % 2 === 1 || name === 'hips'
                                    : rnd(seed, 53, i) < 0.4;
    const R = Math.min(1.8 + s.gears * 2.4, Math.min(sx, sz) * 0.36);
    if (R < 1.6) continue;
    const px = role === 'trunk'
      ? cx + (rnd(seed, 55, i) - 0.5) * (sx - 2 * R - 2)
      : cx;
    const pz = R + 1.5 + rnd(seed, 57, i) * Math.max(0, sz - 2 * R - 3);
    const teeth = 8 + Math.floor(rnd(seed, 59, i) * 5);
    const spin = rnd(seed, 61, i) * 6.283;
    const spokes = 3 + Math.floor(rnd(seed, 63, i) * 3);
    const paint = (d, a) => {
      if (d < 0.2) return SLOTS.GEAR_DARK;                       // the arbor
      if (d < 0.6) {
        const sp = Math.abs(Math.sin((a + spin) * spokes / 2));
        return sp < 0.28 ? SLOTS.GEAR : SLOTS.GEAR_DARK;          // spokes
      }
      if (d < 0.76) return SLOTS.GEAR;                            // the rim
      if (d < 0.94) return Math.cos((a + spin) * teeth) > 0 ? SLOTS.GEAR
                                                            : SLOTS.GEAR_DARK;
      return SLOTS.GEAR_DARK;
    };
    disc(G, onBack ? back : front, onBack ? 1 : -1, px, pz, R, R,
         R > 2.6 ? 2 : 1, paint, () => SLOTS.RIVET, 0.9);
    for (const v of G.list())
      if (Math.hypot(v[0] + 0.5 - px, v[2] + 0.5 - pz) < R + 1.3 &&
          (onBack ? v[1] + 0.5 > cy : v[1] + 0.5 < cy)) own(v[0], v[1], v[2]);
  }

  // ---- bellows: corrugated leather at the joints ----------------------------
  if (s.joints > 0.05 && (role === 'limb' || role === 'head')) {
    const n = Math.max(1, Math.round(s.joints * 3));
    const inBand = z => role === 'head'
      ? z <= (info.neckTop || 0) + 1
      : z < n || z >= sz - n;
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!inBand(z) || !G.lateral(x, y, z) || isOwned(x, y, z)) continue;
      v[3] = SLOTS.JOINT;
      own(x, y, z);
    }
    // The furrows: every other row of the band cut one cell in.
    for (const [x, y, z] of G.list()) {
      if (!inBand(z) || z % 2 || G.get(x, y, z)[3] !== SLOTS.JOINT) continue;
      if (!G.lateral(x, y, z)) continue;
      const [ix, iy] = G.inward(x, y);
      if (G.has(ix, iy, z) && rnd(seed, x, y, z, 67) < 0.85) {
        G.del(x, y, z);
        G.paint(ix, iy, z, SLOTS.GEAR_DARK);
        own(ix, iy, z);
      }
    }
  }

  // ---- pipes: copper runs along the limb, standing proud --------------------
  const nP = s.pipes > 0.05
    ? Math.round(s.pipes * (role === 'trunk' ? 3 : role === 'limb' ? 2 : 0))
    : 0;
  if (nP) {
    const perim = Math.PI * (sx + sy) / 2;
    const w = 0.55 + s.pipes * 0.6;
    // Pipes run down the SIDES and the back (turn 0 is +x, 0.25 the back).
    const at = k => (role === 'trunk' ? [0.04, 0.46, 0.25, 0.3][k % 4]
                                      : [0.02, 0.27][k % 2]) +
                    (rnd(seed, 71, k) - 0.5) * 0.06;
    const groove = [];
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!free(v) || !G.lateral(x, y, z)) continue;
      const t = G.turn(x, y);
      let best = 9;
      for (let k = 0; k < nP; k++) {
        let d = Math.abs(t - at(k));
        d = Math.min(d, 1 - d) * perim;
        best = Math.min(best, d);
      }
      if (best < w) {
        // A coupling every eight rows: a bright collar round the pipe.
        v[3] = (z + (seed & 7)) % 8 === 0 ? SLOTS.RIVET : SLOTS.PIPE;
        own(x, y, z);
      } else if (best < w + 1.3 && role !== 'hand') groove.push([x, y, z]);
    }
    for (const [x, y, z] of groove) G.groove(x, y, z, A.SKIN_SHADE);
  }

  // ---- seams and rivets ------------------------------------------------------
  if (role !== 'hand' && (s.plates > 0.05 || s.rivets > 0.05)) {
    const N = Math.round(18 - s.plates * 11);              // rows per plate
    const off = Math.floor(rnd(seed, 73) * N);
    const perim = Math.PI * (sx + sy) / 2;
    const vSeams = s.plates < 0.25 ? []
      : role === 'trunk' ? (s.plates > 0.6 ? [0, 0.25, 0.5, 0.75] : [0, 0.5])
      : role === 'head' ? [0.25]
      : [0.0, 0.5];
    const onV = (x, y) => vSeams.some(t0 => {
      let d = Math.abs(G.turn(x, y) - t0);
      return Math.min(d, 1 - d) * perim < 0.5;
    });
    const onH = z => role === 'head' ? false
                   : (z + off) % N === 0 && z > 1 && z < sz - 2;
    const seam = [];
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!free(v) || !G.lateral(x, y, z)) continue;
      if (onH(z) || onV(x, y)) seam.push(v);
    }
    // Seams are the darkest line on the body (the gear shadow), so the
    // plating reads as PLATES at a distance where a rivet is a pixel.
    const cut = s.plates > 0.45;
    for (const v of seam) {
      const [x, y, z] = v;
      if (cut && G.groove(x, y, z, SLOTS.GEAR_DARK)) continue;
      v[3] = SLOTS.GEAR_DARK;
    }
    // Rivets beside the seams, spaced along them.
    if (s.rivets > 0.05) {
      const step = Math.max(2, Math.round(7 - s.rivets * 4.5));
      const seamAt = new Set(seam.map(v => G.idx(v[0], v[1], v[2])));
      for (const v of G.list()) {
        const [x, y, z] = v;
        if (!free(v) || !G.lateral(x, y, z)) continue;
        const nearH = seamAt.has(G.idx(x, y, z + 1)) || seamAt.has(G.idx(x, y, z - 1)) ||
                      onH(z + 1) || onH(z - 1);
        const nearV = !onV(x, y) && vSeams.length &&
                      (onV(x + 1, y) || onV(x - 1, y) || onV(x, y + 1) || onV(x, y - 1));
        const along = nearH ? Math.round(G.turn(x, y) * perim) : z;
        if ((nearH || nearV) && (along + off) % step === 0) v[3] = SLOTS.RIVET;
      }
    }
  }

  // ---- wear: dents and scratches --------------------------------------------
  if (s.wear > 0.05 && role !== 'hand') {
    const surf = G.list().filter(v => free(v) && G.lateral(v[0], v[1], v[2]));
    const nd = Math.floor(s.wear * surf.length / 300 + rnd(seed, 79));
    for (let i = 0; i < nd && surf.length; i++) {
      const [x, y, z] = surf[Math.floor(rnd(seed, i, 81) * surf.length)];
      if (!G.has(x, y, z)) continue;
      G.groove(x, y, z, A.SKIN_SHADE);
      if (rnd(seed, i, 83) < 0.5) G.groove(x, y, z + 1, A.SKIN_SHADE);
    }
    const ns = Math.floor(s.wear * surf.length / 160 + rnd(seed, 85));
    for (let i = 0; i < ns && surf.length; i++) {
      const [x0, y0, z0] = surf[Math.floor(rnd(seed, i, 87) * surf.length)];
      const dz = rnd(seed, i, 89) < 0.5 ? 1 : -1;
      const len = 2 + Math.floor(rnd(seed, i, 91) * 4);
      for (let k = 0; k < len; k++) {
        const v = G.get(x0 + (k >> 1), y0, z0 + dz * k);
        if (v && free(v)) v[3] = A.SKIN_LIGHT;
      }
    }
  }

  // ---- polish: bright where the plating faces up -----------------------------
  if (s.polish > 0.05)
    for (const v of G.list()) {
      const [x, y, z] = v;
      if (!free(v) || G.has(x, y, z + 1)) continue;
      if (z === sz - 1 && role !== 'trunk' && role !== 'head') continue;
      if (rnd(seed, x, y, z, 93) < s.polish * 0.8) v[3] = A.SKIN_LIGHT;
    }

  // ---- verdigris: thickest on the pipes and on what faces up -----------------
  if (s.patina > 0.02)
    for (const v of G.list()) {
      const [x, y, z] = v;
      const pipe = v[3] === SLOTS.PIPE || v[3] === SLOTS.RIVET;
      if (!(free(v) || pipe) || !G.exposed(x, y, z)) continue;
      const up = !G.has(x, y, z + 1);
      const n = vnoise(seed ^ 0x9e7d, x, y, z, 3.2);
      const t = s.patina * (pipe ? 1.15 : 0.8) * (up ? 1.3 : 1);
      if (n < t * 0.9 && rnd(seed, x, y, z, 97) < 0.4 + s.patina * 0.5)
        v[3] = SLOTS.PATINA;
    }

  // ---- fingertips, knuckles, toe caps ----------------------------------------
  if (role === 'hand') {
    for (const v of G.list()) if (v[2] <= 1 && PLATE.has(v[3])) v[3] = A.NAIL;
    if (s.rivets > 0.3) {
      const kz = Math.round(sz * 0.42);
      for (const v of G.list())
        if (v[2] === kz && v[1] === front(v[0], v[2]) && v[0] % 2 === 0 &&
            PLATE.has(v[3])) v[3] = SLOTS.RIVET;
    }
  }
  if (role === 'foot')
    for (const v of G.list())
      if (PLATE.has(v[3]) && (v[2] === 0 || v[1] <= 1)) v[3] = A.NAIL;

  const out = keepLargest(G.list(), size);
  if (face) info.face = face;
  return out;
}

/** The automaton's face: lenses in bezels (or a porthole, a cyclops lens,
 *  furnace slits), a brow plate, a mouth grille or a furnace mouth, a jaw
 *  seam, ear bolts. Returns what the extras need, head-local. */
function carveFace(g, A, G, info, seed, paintOwn, plateOf) {
  const s = g.automaton;
  const F = readFace(G, A, info, plateOf);
  const { cx, cy } = G;
  const front = F.front;
  const R0 = 0.9 + s.lens * 0.9;
  const depth = Math.round(s.eyeDepth * 2.2);
  const lensPaint = R => (d) => (d < 0.55 ? A.EYE : d < 0.8 ? SLOTS.GLASS
                                                     : SLOTS.GEAR_DARK);
  const bezel = () => SLOTS.RIVET;
  const eyes = [];
  const eyeZ = F.eyes.reduce((a, e) => a + e.z, 0) / F.eyes.length;
  if (s.porthole > 0.05) {
    // THE DIVING HELMET: a bolted ring, glass inside, the lamps behind it.
    const Rp = (0.22 + s.porthole * 0.16) * G.sx;
    const pz = eyeZ - 1;
    disc(G, front, -1, cx, pz, Rp, Rp * 0.95, 1,
         (d, a, x, z) => {
           for (const e of F.eyes)
             if (Math.hypot(x + 0.5 - e.x, z + 0.5 - e.z) < R0 * 0.75) return A.EYE;
           return SLOTS.GLASS;
         },
         (d, a) => (Math.abs(Math.sin(a * 4)) < 0.22 ? SLOTS.GEAR_DARK : SLOTS.RIVET),
         1.6);
    for (const e of F.eyes) eyes.push({ x: e.x, z: e.z, r: R0 });
  } else if (s.cyclops) {
    const R = R0 * 1.6;
    disc(G, front, -1, cx, eyeZ, R, R, depth, lensPaint(R), bezel, 0.6 + s.bezel * 1.4);
    eyes.push({ x: cx, z: eyeZ, r: R });
  } else if (s.furnace > 0.5) {
    for (const e of F.eyes) {
      disc(G, front, -1, e.x, e.z, R0 * 1.3, 0.75, depth + 1,
           () => A.EYE, () => SLOTS.GEAR_DARK, 0.6);
      eyes.push({ x: e.x, z: e.z, r: R0 * 1.3 });
    }
  } else {
    F.eyes.forEach((e, i) => {
      const big = s.monocle > 0.05 && i === 1;
      const R = R0 * (big ? 1 + s.monocle * 0.6 : 1 - s.monocle * 0.2);
      disc(G, front, -1, e.x, e.z, R, R, depth, lensPaint(R), bezel,
           s.bezel > 0.02 ? 0.5 + s.bezel * (big ? 1.8 : 1.2) : 0);
      eyes.push({ x: e.x, z: e.z, r: R });
    });
  }
  const top = Math.max(...eyes.map(e => e.z + e.r));
  const browZ = top + 0.8;

  // THE BROW PLATE.
  if (s.brow > 0.05 && s.porthole <= 0.05) {
    const x0 = Math.floor(Math.min(...eyes.map(e => e.x - e.r)) - 1);
    const x1 = Math.ceil(Math.max(...eyes.map(e => e.x + e.r)) + 1);
    const rows = 1 + Math.round(s.brow * 1.5);
    for (let z = Math.floor(browZ); z < Math.floor(browZ) + rows; z++)
      for (let x = x0; x <= x1; x++) {
        const f = front(x, z);
        if (f === undefined) continue;
        paintOwn(x, f, z, A.SKIN_SHADE);
        if (s.brow > 0.45 && z === Math.floor(browZ) && x > x0 && x < x1 &&
            G.put(x, f - 1, z, SLOTS.RIVET)) paintOwn(x, f - 1, z, SLOTS.RIVET);
      }
  }

  // THE MOUTH: a furnace grate, or slots of a grille.
  const mz = F.mouthRow + 0.5, mx = cx + F.mouthDx;
  let mouthLow = mz;
  if (s.furnace > 0.5) {
    const rx = 1.6 + s.furnace * 1.8, rz = 1 + s.furnace;
    disc(G, front, -1, mx, mz - 0.5, rx, rz, 2,
         (d, a, x, z) => (z % 2 === 0 && d < 0.9 ? SLOTS.GEAR_DARK : SLOTS.FIRE),
         () => SLOTS.RIVET, 0.9);
    mouthLow = mz - 0.5 - rz;
  } else if (s.grille > 0.05) {
    const k = 1 + Math.round(s.grille * 3);
    const hw = 1.2 + s.grille * 2;
    for (let i = 0; i < k; i++) {
      const z = Math.floor(mz) - i * 2;
      for (let x = Math.floor(mx - hw); x <= Math.ceil(mx + hw); x++) {
        const f = front(x, z);
        if (f === undefined || !G.has(x, f, z) || !G.has(x, f + 1, z)) continue;
        G.del(x, f, z);
        paintOwn(x, f + 1, z, SLOTS.GEAR_DARK);
      }
      mouthLow = z - 1;
    }
  }

  // THE JAW: a seam under the mouth and up the sides, so the jaw is a plate.
  const halfW = Math.max(...eyes.map(e => Math.abs(e.x - cx) + e.r)) + 1.5;
  if (s.jaw > 0.1) {
    const jz = Math.floor(mouthLow - 1);
    const jw = Math.min(halfW + 0.5, G.sx / 2 - 1);
    const zTop = Math.floor(F.eyeRow - 2);
    for (let x = Math.floor(cx - jw); x <= Math.ceil(cx + jw); x++) {
      const f = front(x, jz);
      if (f === undefined || !G.has(x, f + 1, jz)) continue;
      G.del(x, f, jz);
      paintOwn(x, f + 1, jz, A.SKIN_SHADE);
    }
    if (s.jaw > 0.35)
      for (let z = jz; z <= zTop; z++)
        for (const x of [Math.floor(cx - jw), Math.ceil(cx + jw) - 1]) {
          const f = front(x, z);
          if (f === undefined || !G.has(x, f + 1, z)) continue;
          G.del(x, f, z);
          paintOwn(x, f + 1, z, A.SKIN_SHADE);
        }
  }

  // EAR BOLTS: a rivet boss each side at the eye row.
  const ez = F.eyeRow - 1;
  const xs = G.list().map(v => v[0]);
  const xMin = Math.min(...xs), xMax = Math.max(...xs);
  for (const v of G.list()) {
    const [x, y, z] = v;
    if (x !== xMin && x !== xMax) continue;
    const d = Math.hypot(y + 0.5 - cy, z + 0.5 - ez);
    if (d < 0.9) paintOwn(x, y, z, SLOTS.GEAR_DARK);
    else if (d < 2.1) paintOwn(x, y, z, SLOTS.RIVET);
  }

  return { eyes, browZ, chinZ: F.neckTop + 1, halfW, mouth: { x: mx, z: mz },
           frontOf: front };
}

// =============================================================================
// the extras: chimney, top hat, gear crest, exhaust stacks, wind-up key
//
// Called from mobgen's hairMass on the SHIPPED lattice in SCENE coordinates
// (x right, y BACK, z up). Returns head-riding cells (`snout` parts: fixed to
// the head) and torso-riding ones (`bough` parts: fixed to the torso, and
// dropped unless they touch it). Nothing is ever written into a body cell.
// =============================================================================

export function extrasWant(g) {
  const s = g.automaton;
  return s.stackHead > 0.02 || s.hat > 0.02 || s.crest > 0.02 ||
         s.stackBack > 0.02 || s.key > 0.02;
}

export function extras(g, ctx) {
  const s = g.automaton;
  const { K, body, head, torso } = ctx;
  const A = ctx.ART;
  const clothAt = (y, cy) => (y + 0.5 > cy + 1 ? A.CLOTH_SHADE : A.CLOTH);
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
  // The top of the skull per column, and its crown.
  const topAt = new Map();
  for (const [x, y, z] of sceneCells(head)) {
    const k = x * 4096 + y;
    if (!topAt.has(k) || z > topAt.get(k)) topAt.set(k, z);
  }
  const top = (x, y) => topAt.get(Math.floor(x) * 4096 + Math.floor(y));
  const hcx = H.cx, hcy = H.cy;
  const crownZ = top(hcx, hcy) ?? H.z1;

  // The lowest skull top under a footprint: what a cylinder must start from
  // to sit flush on a domed head.
  const seat = (px, py, r) => {
    let lo = Infinity;
    for (let y = Math.floor(py - r); y <= py + r; y++)
      for (let x = Math.floor(px - r); x <= px + r; x++)
        if (Math.hypot(x + 0.5 - px, y + 0.5 - py) <= r) {
          const t = top(x, y);
          if (t !== undefined) lo = Math.min(lo, t);
        }
    return Number.isFinite(lo) ? lo + 1 : crownZ + 1;
  };

  // ---- a chimney, with bands and a flared, sooty mouth ----------------------
  const chimney = (px, py, z0, r, h, emit) => {
    const z1 = z0 + h;
    cylinder(px, py, z0, z1, r, (x, y, z, d) => {
      const band = (z - z0) % 4 === 3;
      emit(x, y, z, z === z1 && d < 0.6 ? SLOTS.GEAR_DARK
                    : band ? SLOTS.RIVET : SLOTS.PIPE);
    });
    cylinder(px, py, z1 - 1, z1, r + 1, (x, y, z, d) => {
      if (d * (r + 1) > r + 0.2) emit(x, y, z, SLOTS.PIPE);
    });
    return z1;
  };

  // ---- the top hat ------------------------------------------------------------
  let hatTop = null;
  if (s.hat > 0.02) {
    const halfW = (H.x1 - H.x0 + 1) / 2, halfD = (H.y1 - H.y0 + 1) / 2;
    const zb = crownZ - 1;
    const Rb = Math.max(halfW, halfD) + 1.5;
    // The brim, one row, resting on the head where it meets it.
    cylinder(hcx, hcy, zb, zb, Rb, (x, y, z) => addH(x, y, z, A.CLOTH));
    const Rc = Math.min(halfW, halfD) * 0.82;
    const hh = Math.round(5 + s.hat * 8);
    cylinder(hcx, hcy, zb + 1, zb + hh, Rc, (x, y, z) => {
      addH(x, y, z, z <= zb + 2 ? SLOTS.PIPE : clothAt(y, hcy));
    });
    hatTop = zb + hh;
  }
  // ---- the chimney: out of the hat, or out of the skull ---------------------
  if (s.stackHead > 0.02) {
    const r = 1.4 + s.stackHead * 1.1;
    const h = Math.round(3 + s.stackHead * 11);
    const py = hcy + (hatTop !== null ? 0 : 1);
    const z0 = hatTop !== null ? hatTop + 1 : seat(hcx, py, r);
    chimney(hcx, py, z0, r, h, addH);
  }
  // ---- the gear crest: half a wheel standing along the skull ----------------
  if (s.crest > 0.02 && hatTop === null) {
    const R = 3 + s.crest * 6;
    const teeth = 10 + Math.round(s.crest * 6);
    const cz = crownZ - R * 0.35;
    const x0 = Math.floor(hcx) - 1;
    for (let y = Math.floor(hcy - R - 1); y <= hcy + R + 1; y++)
      for (let z = Math.floor(cz); z <= cz + R + 1; z++) {
        const d = Math.hypot(y + 0.5 - hcy, z + 0.5 - cz);
        const a = Math.atan2(z + 0.5 - cz, y + 0.5 - hcy);
        const rim = Math.cos(a * teeth) > 0 ? R : R * 0.84;
        if (d > rim) continue;
        for (let x = x0; x <= x0 + 1; x++) {
          const t = top(x, y);
          if (t !== undefined && z <= t) continue;
          addH(x, y, z, d < R * 0.25 ? SLOTS.GEAR_DARK
                       : d < R * 0.62 && Math.abs(Math.sin(a * 2.5)) > 0.3
                         ? SLOTS.GEAR_DARK : SLOTS.GEAR);
        }
      }
  }

  // ---- the torso's back ------------------------------------------------------
  const T = extent(torso);
  const backAt = new Map();
  for (const [x, y, z] of sceneCells(torso)) {
    const k = x * 4096 + z;
    if (!backAt.has(k) || y > backAt.get(k)) backAt.set(k, y);
  }
  const backOf = (x, z) => backAt.get(Math.floor(x) * 4096 + Math.floor(z));
  const tH = T.z1 - T.z0 + 1;

  // ---- exhaust stacks up the back, past the shoulders ------------------------
  if (s.stackBack > 0.02) {
    const r = 1.1 + s.stackBack * 0.6;
    for (const side of [-1, 1]) {
      const px = T.cx + side * (T.x1 - T.x0 + 1) * 0.22;
      const zs = Math.round(T.z0 + tH * 0.55);
      const yb = backOf(px, zs);
      if (yb === undefined) continue;
      const py = yb + 1 + r;
      // The elbow out of the back...
      for (let y = yb + 1; y <= py; y++)
        for (let dz = -1; dz <= 1; dz++)
          for (let dx = -1; dx <= 1; dx++)
            if (Math.abs(dx) + Math.abs(dz) <= 1 + (r > 1.4 ? 1 : 0))
              addT(Math.floor(px) + dx, y, zs + dz, SLOTS.PIPE);
      // ...and the stack, up past the shoulder line.
      const h = Math.round(T.z1 - zs + 3 + s.stackBack * 10);
      chimney(px, py + 0.5, zs, r, h, addT);
    }
  }

  // ---- the wind-up key -------------------------------------------------------
  if (s.key > 0.02) {
    const zk = Math.round(T.z0 + tH * 0.6);
    const px = Math.floor(T.cx);
    const yb = backOf(px, zk);
    if (yb !== undefined) {
      const L = Math.round(3 + s.key * 3);
      // The shaft: 2 x 2, out of the back.
      for (let y = yb + 1; y <= yb + L; y++)
        for (const [dx, dz] of [[0, 0], [-1, 0], [0, 1], [-1, 1]])
          addT(px + dx, y, zk + dz, y === yb + 1 ? SLOTS.GEAR_DARK : SLOTS.RIVET);
      // The bow: two rings side by side in the x-z plane, two cells thick.
      const Rb = 2.2 + s.key * 3;
      for (let y = yb + L + 1; y <= yb + L + 2; y++)
        for (const side of [-1, 1]) {
          const rx = px + side * (Rb - 0.3);
          for (let z = Math.floor(zk - Rb - 1); z <= zk + Rb + 1; z++)
            for (let x = Math.floor(rx - Rb - 1); x <= rx + Rb + 1; x++) {
              const d = Math.hypot(x + 0.5 - rx, z + 0.5 - (zk + 0.5));
              if (d <= Rb && d >= Rb - 1.3) addT(x, y, z, SLOTS.RIVET);
            }
          // The web joining the ring to the shaft.
          line6([px, y, zk], [Math.round(rx), y, zk], (x, yy, z) => {
            addT(x, yy, z, SLOTS.RIVET); addT(x, yy, z + 1, SLOTS.RIVET);
          });
        }
    }
  }

  return { head: [...headOut.values()], torso: [...torsoOut.values()] };
}
