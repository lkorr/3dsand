/* mobgen.js — the ONE parametric humanoid character generator.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `scripts/gen_human.py` builds the stock player body: 1,588 lines of Python,
 * run by hand, writing one file. It is a good generator and a bad pipeline —
 * nothing else can call it, the tuner cannot preview it, and a second
 * character means a second copy of the whole script (there were three:
 * gen_mina, gen_wizard, gen_asha, all now deleted, all subtly diverged).
 *
 * This is that generator, ported to a PURE ES module with zero dependencies,
 * with the hardcoded figure lifted out into a GENOME. It runs in the browser
 * (the Characters page, assets/editor/breed.js) and under `node`
 * (scripts/gen_mobs.mjs, scripts/test_mobgen.mjs) from the same source, which
 * is the arrangement treegen.js / biomegen.js / watergen.js already use and
 * the reason the Trees page shows the engine's actual tree.
 *
 * THE PORT IS PINNED. `generateMob(defaultGenome())` reproduces
 * gen_human.py's output cell for cell — same limb boxes, same voxels, same
 * art slots, same placements, same sidecar fields. `scripts/test_mobgen.mjs`
 * asserts it against a digest of a live gen_human.py run. That is the one
 * test that makes a 1,588-line translation safe, and it is why every
 * default below is the exact literal the Python carries rather than a tidier
 * number nearby.
 *
 * NOTE: it does NOT reproduce assets/mobs/human.vox, and neither does
 * gen_human.py any more. The shipped human has been hand-extended past its
 * generator (its interior rewritten by the anatomy pass, clothing dye in the
 * art palette, seven sidecar blocks edited) and, in GEOMETRY, its shoulders
 * were rounded by hand in the model editor: thirteen of the fifteen limbs are
 * identical to the cell, and the torso and upper arms differ by 130 cells, all
 * removals, all in the shoulder. That sculpting is reproduced as a rule here
 * — see roundShoulders — so a generated character does not need the same hand
 * work. The generator is the reference; the shipped asset is a descendant of
 * it, and regenerating it is explicitly out of scope — new characters are new
 * files.
 *
 * ROLL SHAPE, DERIVE EVERYTHING ELSE
 * ----------------------------------
 * A human eye on a thumbnail grid filters UGLY perfectly and filters BROKEN
 * not at all, because a body whose ride height does not match its leg length
 * looks fine standing still and hovers the moment it walks. So the genome
 * carries SHAPE ONLY. Every quantity that has to agree with the shape —
 * limb anchors, socket offsets, natural-weapon edges, IK poles, per-limb hp,
 * the gait's rideHeight, the arm-swing clip periods, the eye row — is
 * COMPUTED from the shape here and is not a gene. A derived value needs no
 * mutation range and cannot drift wrong.
 *
 * FACING AND HANDEDNESS (got wrong twice before this file existed, do not
 * re-derive): the loader maps scene -> engine as (x, z, -y), which negates
 * scene y and therefore FLIPS handedness. Every builder authors its part
 * facing scene +Y and mirrors once with flipY at the end. Model +X is the
 * character's LEFT, so `.L` limbs take POSITIVE scene x.
 *
 * CENTRING: every box is centred so its ellipse centre (mn.x + sx*0.5) lands
 * on x = 0, and left/right boxes are exact mirrors about it. Invisible on a
 * robe, very visible on a bare shoulder line.
 *
 * HOW A GENE STAYS EXACT AT THE DEFAULT. Shape parameters that must scale
 * with a box (profile radii, tube radii, the skull's set-back) are authored
 * as ABSOLUTE micro lengths at the DEFAULT box size and multiplied by
 * `thisBox / defaultBox`. At the default genome that ratio is exactly 1.0 and
 * the multiply is bit-exact, which is what lets the pin above be an equality
 * rather than a tolerance. Authoring them as fractions of the box would round
 * -trip through a non-dyadic quotient and lose the last bit.
 *
 * ONE ARCHETYPE, DELIBERATELY. Every downstream system binds by limb NAME —
 * gait.groups, the four IK chains, the dismemberment state table, ~15 KB of
 * clips masked by name, the natural weapons, the anatomy head override,
 * attack_styles.json. A quadruped has no `armU.L`, so idle / bite / crawl /
 * limp / onearm all fail to bind at once. The tractable future version is
 * more ARCHETYPES, each a limb-name vocabulary with its own clip set; that is
 * why the vocabulary lives in ARCHETYPE below and no builder contains the
 * string "armU.L".
 */

'use strict';

import { writeVox, readVox, tightenPrefab, prefabToVoxModels,
         prefabRoundTripTest } from './vox.js';
import * as ANA from './anatomy.js';

// =============================================================================
// Python parity
// =============================================================================

/**
 * Python's `round()`: half-to-EVEN on the exact binary value, returning the
 * nearest double to the rounded decimal. `Math.round` is half-UP and
 * `toFixed` is half-up on the exact value, so both differ from Python at a
 * tie — and ties are not exotic here (`int(round(hcy))` on 4.5, an arm cycle
 * that lands on a whole half-millisecond).
 *
 * The exact decimal expansion of a double can run to ~1080 digits, but at the
 * magnitudes this file deals in (0..10^4) an ulp is never smaller than
 * ~1e-12, so 25 places always separate a true tie from a near-tie. Rendering
 * to 25 places and rounding the STRING is therefore exact, and parseFloat
 * back is the same "nearest double to the decimal" step Python performs.
 */
export function pyRound(x, nd = 0) {
  if (!Number.isFinite(x)) return x;
  const neg = x < 0 || Object.is(x, -0);
  const s = Math.abs(x).toFixed(25);
  const dot = s.indexOf('.');
  const digits = (s.slice(0, dot) + s.slice(dot + 1)).split('');
  const cut = dot + nd;                    // digits kept, as an index
  if (cut >= digits.length) return x;
  if (cut < 0) return neg ? -0 : 0;
  const rest = digits.slice(cut).join('');
  const first = rest.charCodeAt(0) - 48;
  const tail = rest.slice(1);
  let up;
  if (first > 5) up = true;
  else if (first < 5) up = false;
  else if (/[1-9]/.test(tail)) up = true;  // past the tie: round away
  else up = ((digits[cut - 1] || '0').charCodeAt(0) - 48) % 2 === 1;  // half-even
  const kept = digits.slice(0, cut);
  if (up) {
    let i = kept.length - 1;
    for (; i >= 0; i--) {
      if (kept[i] === '9') { kept[i] = '0'; continue; }
      kept[i] = String.fromCharCode(kept[i].charCodeAt(0) + 1);
      break;
    }
    if (i < 0) kept.unshift('1');
  }
  const intLen = kept.length - nd;
  const str = (intLen <= 0 ? '0.' + '0'.repeat(-intLen) + kept.join('')
                           : kept.slice(0, intLen).join('') + '.' +
                             kept.slice(intLen).join(''));
  const v = parseFloat(str || '0');
  return neg ? -v : v;
}

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const radians = d => d * Math.PI / 180;

// =============================================================================
// deterministic RNG — same discipline as treegen.js
//
// No Math.random() anywhere. A litter is reproducible from (parent, seed),
// which is what lets the Characters page re-render a thumbnail without the
// body changing under it and lets a bad roll be reported by seed.
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

/** A counter RNG: `rng()` in [0,1), `rng.norm()` roughly standard-normal. */
export function makeRng(seed) {
  let n = 0;
  const f = () => hashN(seed | 0, n++) / 4294967296;
  // Irwin-Hall(4), centred and scaled: bounded (never a 6-sigma limb) and
  // cheap. A mutation that can produce an arbitrarily large excursion makes
  // the sigma slider meaningless, because one gene in a litter always blows.
  f.norm = () => ((f() + f() + f() + f()) - 2) * 1.732;
  f.pick = arr => arr[Math.min(arr.length - 1, (f() * arr.length) | 0)];
  return f;
}

// =============================================================================
// the archetype: the limb-name vocabulary and everything keyed to it
//
// EVERY name in the rig appears here and nowhere else in this file. Adding a
// `quadruped` archetype is then a second table plus its own clip set, rather
// than a grep through nine builders.
// =============================================================================

export const ARCHETYPE = {
  name: 'humanoid',
  root: 'hips',
  sides: ['L', 'R'],
  /** Parent-before-child. The loader topologically sorts anyway; authoring in
   *  order keeps the .vox scene graph readable in MagicaVoxel. */
  order: ['hips', 'torso', 'head',
          'armU.L', 'armL.L', 'hand.L',
          'armU.R', 'armL.R', 'hand.R',
          'legU.L', 'legL.L', 'foot.L',
          'legU.R', 'legL.R', 'foot.R'],
  /** Held props are excluded from the height contract and from the
   *  prefab-origin measurement, mirroring the loader (mob.cpp skips
   *  tag == "prop" when it measures worldSize). This rig has none. */
  props: [],
  /** WHICH PARTS GET THE SHOULDER ROUND, and which of the two rules.
   *  `fin` de-fins a widening silhouette, `cap` domes a flat limb end.
   *  See roundShoulders() for what each one does and why there are two. */
  shoulders: { fin: ['torso'], cap: ['armU.L', 'armU.R'] },
  /** The vertical stack, sole to crown. `overlap` is how far this segment's
   *  base sits BELOW the previous segment's top — a joint seam, structural,
   *  not a gene: it is what keeps a shoulder inside the torso and a knee
   *  inside the thigh when the proportions move. */
  stack: [
    { gene: 'foot',  overlap: 0 },
    { gene: 'shin',  overlap: 0 },
    { gene: 'thigh', overlap: 1 },
    { gene: 'hips',  overlap: 2 },
    { gene: 'torso', overlap: 3 },
    { gene: 'head',  overlap: 1 },
  ],
  /** The arm hangs from the shoulder (the torso's top) downward, same idea. */
  armStack: [
    { gene: 'upper', overlap: 0 },
    { gene: 'fore',  overlap: 1 },
    { gene: 'hand',  overlap: 1 },
  ],
};

// =============================================================================
// material and art palette
// =============================================================================

/** EVERY VOXEL OF THE BODY IS ONE MATERIAL. `skin` (materials.json "skin") is
 *  the FLESH material: it cooks, burns, chars and bleeds everywhere, including
 *  under the shorts. What varies is `color`, an ART PALETTE slot carried in a
 *  parallel "<name>.col" model. Art colour never reaches the world grid, so it
 *  can never touch the sim hash, while material is what the CA reacts to.
 *
 *  Resolved BY NAME against the caller's materials list rather than pinned to
 *  51 — `src/sim/voxload.h` says palette index i+1 == materials.json[i], so a
 *  material inserted above `skin` would silently re-map the whole body. */
export const FLESH_ID = 'skin';

/** Art slots, allocated DOWNWARD from 255 exactly as vox.js's ArtPalette does,
 *  so a document round-tripped through the editor keeps its numbering.
 *  Mob voxel MATERIAL ids must stay <= 127; 128..255 is the art palette. */
export const ART = {
  SKIN_BASE: 255,
  SKIN_SHADE: 254,
  SKIN_LIGHT: 253,   // unused by geometry; reserved so the row exists
  HAIR: 252,
  HAIR_SHADE: 251,
  EYE: 250,
  CLOTH: 249,
  CLOTH_SHADE: 248,
  NAIL: 247,         // reserved (nails are below this resolution)
};

/** genome.colors key -> art slot. The genome names a colour by what it IS,
 *  the slot is where it lands; the UI pickers walk this map. */
export const COLOR_SLOTS = {
  skin: ART.SKIN_BASE,
  skinShade: ART.SKIN_SHADE,
  skinLight: ART.SKIN_LIGHT,
  hair: ART.HAIR,
  hairShade: ART.HAIR_SHADE,
  eye: ART.EYE,
  cloth: ART.CLOTH,
  clothShade: ART.CLOTH_SHADE,
  nail: ART.NAIL,
};

/**
 * NINE COLOUR SLOTS, FIVE COLOURS. A shade or a light is not an independent
 * colour, it is the SAME colour at another exposure, and the art paints it on
 * the same surface a few cells away: `shadeBack` puts the shade slot on the
 * away-facing half of every part, so skin and skinShade meet along a seam down
 * the character's side, and hair and hairShade meet across the back of the
 * head.
 *
 * Nothing enforced that, and everything that moves a genome moved the nine
 * independently -- `mutate` jittered each slot with its own noise, `cross`
 * drew a separate blend weight per slot, `randomGenome` rolled each at 0.30.
 * The result was not subtle. Measured, five generations of mutate at the
 * page's default sigma: hair #4a3728 (dark brown) -> #5f0e65 (PURPLE) with a
 * hairShade of #00250e (DARK GREEN). A head whose front is purple and whose
 * back is green is not a character, and no amount of shape work rescues it.
 *
 * So the dependents are carried ALONG with their base rather than beside it:
 * whatever tone relationship a genome currently has is measured and re-applied
 * to the new base (`reshadeFamily`). That keeps the hand-tuned pairs in
 * COMPLEXIONS and HAIR_COLORS exactly as authored -- a deep skin darkens more
 * than a pale one, which no single multiplier reproduces -- while making the
 * family move as one thing. A slider still edits any slot directly; the next
 * roll simply carries the relationship you left it at.
 */
export const COLOR_FAMILIES = {
  skin:  ['skinShade', 'skinLight', 'nail'],
  hair:  ['hairShade'],
  cloth: ['clothShade'],
};
/** slot -> the base it is a tone of, for every dependent slot. */
export const COLOR_BASE_OF = (() => {
  const m = {};
  for (const [base, deps] of Object.entries(COLOR_FAMILIES))
    for (const d of deps) m[d] = base;
  return m;
})();

// =============================================================================
// the scale contract — METRES (DESIGN.md 3b)
//
// kVoxelMeters does not appear. The sidecar declares artVoxelsPerMetre and the
// loader derives skinScale from it and the live kVoxelsPerMetre, so this
// generator is voxel-size independent by construction. A hand-copied
// VOX_PER_M is exactly how every tree in the game once baked at half height
// with nothing able to notice (see scripts/bake_trees.mjs).
// =============================================================================

export const ART_SCALE = 4;              // authored micro per world voxel
export const SKIN_UPSCALE = 2;           // authored -> shipped
export const SCALE = ART_SCALE * SKIN_UPSCALE;          // 8, the legacy skinScale
export const ART_VOXELS_PER_METRE = 80;                 // the sidecar value
export const AUTHORED_VOXELS_PER_METRE = ART_VOXELS_PER_METRE / SKIN_UPSCALE;  // 40
/** The scale the sidecar's WORLD-space rows (speed, severImpactSpeed,
 *  rideHeight, bodyYOffset, clip pos) are written at; mob.cpp rescales them
 *  from here to the live kVoxelsPerMetre. */
export const SIDECAR_VOXELS_PER_METRE = 10;

export const DEFAULT_HEIGHT_M = 1.70;
/** The first-person camera row. THE tightest contract in the file: it is
 *  Player::kEyeOffset above the AABB centre, the head builder derives the eye
 *  voxel row from it, and an assert backstops the derivation. */
export const DEFAULT_EYE_M = 1.50;

/** Height band. Locked narrow ON PURPOSE. Reach, melee range, the sprint
 *  speed contract and several gate fixtures bet on a 1.7 m figure; a 2.4 m
 *  NPC is not a taller character, it is a broken one whose arms no longer
 *  reach what the AI thinks they reach. +-10% moves a silhouette visibly and
 *  moves nothing else. */
export const HEIGHT_BAND = [1.53, 1.87];

/** The runtime constants the derived gait numbers are computed WITH. They
 *  live in src/game/avatar.cpp; restating them is a second source of truth,
 *  so `scripts/test_mobgen.mjs` parses the C++ and fails if these drift.
 *  Callers with the file in hand (gen_mobs.mjs) pass the parsed values. */
export const AVATAR_CONSTANTS = {
  swingTravelFrac: 0.7,     // kSwingTravelFrac
  maxLeadLegLengths: 0.9,   // kMaxLeadLegLengths
  minSwingScale: 0.35,      // kMinSwingScale
  minSwingSeconds: 0.09,    // kMinSwingSeconds
};

/** The player numbers the speed contract reads, from assets/materials/
 *  tuning.json. Same deal: callers pass the live values, these are the
 *  fallback and the gate checks them. */
export const PLAYER_CONSTANTS = {
  sprintSpeed: 3.15,
  walkSpeed: 1.6,
  halfHeight: 0.85,
  eyeOffset: 0.65,
};

// `speed` is the reference top speed the runtime divides measured speed by to
// get speedFactor, which scales cadence, bob, sway, roll, the spring goals and
// the swing duration. It must be the PLAYER's top speed, not a mob's — mina's
// was transcribed, went stale by 2x, and quietly halved every gait amplitude.
const STEP_DURATION = 0.10;
const STEP_THRESHOLD = 0.3;

// =============================================================================
// the genome
//
// SHAPE ONLY (see the banner). Lengths in the vertical stack are WEIGHTS, not
// absolute rows: the sum is pinned to the height contract and the weights say
// how the budget is spent, so "longer legs" cannot also mean "taller".
// =============================================================================

export function defaultGenome() {
  return {
    archetype: 'humanoid',
    name: 'untitled',
    displayName: 'Untitled',

    body: {
      heightM: DEFAULT_HEIGHT_M,
      /** Box widths in AUTHORED micro. Forced EVEN by normalizeGenome: a box
       *  is centred at mn.x = -sx/2 and an odd width puts the ellipse centre
       *  on a half cell, which breaks the left/right mirror. */
      hipWidth: 10,
      shoulderWidth: 12,
      bodyDepth: 8,
      headWidth: 10,
      headDepth: 10,
      limbWidth: 4,
      footDepth: 7,
      /** How the 1.7 m is spent, sole to crown. Defaults are gen_human.py's
       *  rows exactly: foot 4, shin 13, thigh 14, hips 10, torso 18, head 16,
       *  which with the archetype's 7 micro of joint overlap is 68 micro =
       *  17 world voxels. */
      stack: { foot: 4, shin: 13, thigh: 14, hips: 10, torso: 18, head: 16 },
      /** Shoulder to fingertip, same idea: weights over the arm's own span. */
      armStack: { upper: 11, fore: 11, hand: 5 },
    },

    /** Silhouette multipliers on the authored radius profiles. 1.0 is the
     *  drawn figure; the profile knots themselves are not genes because a
     *  five-knot curve is not a slider and a hand-edited knot is how you get
     *  a shoulder with a pimple on it (see the radius note in torsoProfile). */
    shape: {
      waist: 1.0,        // torso, low knots
      chest: 1.0,        // torso, middle
      shoulder: 1.0,     // torso, top — the widest thing on the upper body
      chestDepth: 1.0,   // torso, front-to-back
      seat: 1.0,         // hips
      armGirth: 1.0,
      legGirth: 1.0,
    },

    /** The face, as three regions of the skull sweep. Read as a face: the
     *  chin/jaw, the cheekbones, the round of the crown. */
    head: {
      jaw: 1.0,
      cheek: 1.0,
      crown: 1.0,
      /** The skull sits BEHIND the face plane — a human has far more cranium
       *  behind the eyes than in front, and centring the ellipsoid gives a
       *  snout. Authored micro at the default head depth. */
      skullBack: 0.6,
    },

    face: {
      eyeDx: 2.5,       // half the pupil separation, micro, about the midline
      eyeRow: 0,        // micro offset from the DERIVED eye row; not a licence
      eyeCols: 1,       // columns painted per eye
      mouthWidth: 2,
      mouthDrop: 3,     // micro below the eye row
      brow: false,      // an eyebrow row, off by default
    },

    /** HAIR IS A FUNCTION, not a texture: `back`/`front` are the z at or above
     *  which a depth row is hair, at the nape and at the forehead. Lower at
     *  the back is what makes the cap read as swept hair rather than a helmet,
     *  and swapping the two numbers is a fringe. This is the cheapest and
     *  highest-yield identity lever at a 20-voxel head — see STYLES. */
    hair: {
      style: 'swept',
      back: 5.5,        // authored micro at the default head height
      front: 10.0,
      length: 0,        // micro of mane below the skull, down the nape
      knot: false,      // a topknot nub above the crown
    },

    colors: {
      skin: '#c98d63',
      skinShade: '#a06e4b',
      skinLight: '#e2b189',
      hair: '#4a3728',
      hairShade: '#33251a',
      eye: '#241d18',
      cloth: '#7a6a4e',
      clothShade: '#5c4f39',
      nail: '#d9ae90',
    },
  };
}

/** Hairstyles, as the two numbers plus the two flags they set. A style is a
 *  PRESET of the hair genes, never a separate code path, so a hand-dragged
 *  slider and a picked style are the same thing to the generator and mutation
 *  does not have to know which one produced the body. Numbers are authored
 *  micro at the default 16-micro head and scale with it. */
export const HAIR_STYLES = {
  bald:    { back: 99, front: 99, length: 0, knot: false },
  cropped: { back: 8.5, front: 8.5, length: 0, knot: false },
  swept:   { back: 5.5, front: 10.0, length: 0, knot: false },
  fringe:  { back: 9.5, front: 5.0, length: 0, knot: false },
  long:    { back: 4.5, front: 9.5, length: 6, knot: false },
  mane:    { back: 3.5, front: 8.5, length: 10, knot: false },
  topknot: { back: 10.5, front: 11.5, length: 0, knot: true },
};
export const HAIR_STYLE_ORDER =
  ['bald', 'cropped', 'swept', 'fringe', 'long', 'mane', 'topknot'];

/**
 * THE GENE GROUPS, in the order the Characters page stacks them. `note` is the
 * one line under the heading that says what the whole group does — the thing
 * you need BEFORE any individual slider makes sense, and the only place the
 * "weights, not lengths" contract is stated where a person will read it.
 */
export const GENE_GROUPS = [
  { key: 'body', title: 'size and build',
    note: 'Absolute sizes. Widths are in body units (2 units = 1 world voxel ' +
          'of skin), so they move in steps of 2.' },
  { key: 'proportion', title: 'proportions',
    note: 'These SHARE OUT the height above, they do not add to it — longer ' +
          'legs make everything else shorter, and the character stays exactly ' +
          'as tall.' },
  { key: 'shape', title: 'silhouette',
    note: 'Multipliers on the drawn outline. 1.00 is the stock figure; below ' +
          '1 pinches it in, above 1 fills it out.' },
  { key: 'face', title: 'head and face',
    note: 'A face here is about twenty voxels across, so small moves read ' +
          'large. The side view is where the skull shape shows.' },
  { key: 'hair', title: 'hair',
    note: 'Hair is two heights on the skull, not a texture: how far down the ' +
          'back, and how far down the forehead. Which one is lower is the ' +
          'difference between swept-back hair and a fringe.' },
  { key: 'colour', title: 'colours',
    note: 'Nine art slots. Each surface picks base, shadow or highlight by ' +
          'which way it faces, so the three skin tones want to be the same ' +
          'colour at three brightnesses — pick them far apart and the figure ' +
          'looks striped.' },
];

/** Per-colour-slot wording, so the colour rows read as parts of a person
 *  rather than as the internal slot names. */
const COLOR_LABELS = {
  skin:       ['skin', 'The base complexion. Most of the body is this.'],
  skinShade:  ['skin, shaded', 'Skin on faces turned away from the light. ' +
               'A darker version of the base, not a different colour.'],
  skinLight:  ['skin, lit', 'Skin on upward faces. A lighter version of the ' +
               'base.'],
  hair:       ['hair', 'The base hair colour.'],
  hairShade:  ['hair, shaded', 'Hair on shaded faces — a darker version of ' +
               'the base.'],
  eye:        ['eyes', 'The pupils. Two to four voxels, so this only reads ' +
               'if it contrasts with the skin.'],
  cloth:      ['clothing', 'The worn layer painted onto the body — the ' +
               'tunic, the shorts and the shoes.'],
  clothShade: ['clothing, shaded', 'The same garment where it turns away ' +
               'from the light.'],
  nail:       ['nails', 'Fingertips and toenails. Barely visible; it stops ' +
               'the hands reading as mittens.'],
};

/**
 * THE GENE TABLE — one declaration per gene, and the ONLY place a gene's
 * range lives.
 *
 * It drives three consumers that must agree or the UI lies about what a roll
 * can produce: the sliders on the Characters page, `mutate`, and the clamping
 * in `normalizeGenome`. Adding a gene here is what makes it appear in all
 * three; adding it to `defaultGenome` alone makes it a constant.
 *
 * `sigma` is the mutation step at slider 1.0, in the gene's own units.
 *
 * `hint` is the sentence the Characters page shows for the row. It is written
 * for someone dragging the slider and watching the figure: what MOVES, and
 * what the ends of the travel look like. It is not a restatement of the label
 * and it does not name a variable — a hint that says "controls the shoulder
 * width parameter" is worse than no hint, because it costs a line and teaches
 * nothing. Ranges, units and defaults are already on screen in the readout, so
 * the hint never repeats them either.
 *
 * `uiMax` narrows the SLIDER without narrowing the gene (see hair.back).
 */
export const GENE_SPECS = [
  { path: 'body.heightM', label: 'height', group: 'body', unit: 'm',
    hint: 'Sole to crown. Every proportion below divides this up, so this is ' +
          'the only slider that changes how tall the character is.',
    min: HEIGHT_BAND[0], max: HEIGHT_BAND[1], step: 0.01, sigma: 0.05 },
  { path: 'body.shoulderWidth', label: 'shoulder width', group: 'body',
    hint: 'How wide the torso is across the top. The strongest single read ' +
          'on heavy versus slight, and the arms hang off whatever you set.',
    min: 8, max: 18, step: 2, int: true, even: true, sigma: 1.4 },
  { path: 'body.hipWidth', label: 'hip width', group: 'body',
    hint: 'Width across the pelvis. Wider than the shoulders reads as ' +
          'pear-shaped, much narrower as a wedge.',
    min: 8, max: 16, step: 2, int: true, even: true, sigma: 1.2 },
  { path: 'body.bodyDepth', label: 'body thickness', group: 'body',
    hint: 'Front to back through the chest. Invisible head-on — watch the ' +
          'side view.',
    min: 6, max: 14, step: 2, int: true, even: true, sigma: 1.2 },
  { path: 'body.limbWidth', label: 'limb thickness', group: 'body',
    hint: 'How thick arms and legs are cut. The arm and leg girth sliders ' +
          'further down scale whatever you pick here.',
    min: 4, max: 8, step: 2, int: true, even: true, sigma: 0.8 },
  { path: 'body.headWidth', label: 'head width', group: 'body',
    hint: 'Across the skull, ear to ear.',
    min: 8, max: 14, step: 2, int: true, even: true, sigma: 0.8 },
  { path: 'body.headDepth', label: 'head depth', group: 'body',
    hint: 'Front to back through the skull. Deeper reads as a long face in ' +
          'profile.',
    min: 8, max: 14, step: 2, int: true, even: true, sigma: 0.8 },
  { path: 'body.footDepth', label: 'foot length', group: 'body',
    hint: 'How far the foot reaches in front of the ankle. Long feet read as ' +
          'boots.',
    min: 5, max: 10, step: 1, int: true, sigma: 0.7 },

  { path: 'body.stack.shin', label: 'shin', group: 'proportion',
    hint: 'Knee to ankle.',
    min: 8, max: 18, step: 0.5, sigma: 1.0 },
  { path: 'body.stack.thigh', label: 'thigh', group: 'proportion',
    hint: 'Hip to knee. Thigh and shin together are the leg: raise both and ' +
          'the figure goes lanky, drop both and it goes stocky.',
    min: 9, max: 19, step: 0.5, sigma: 1.0 },
  { path: 'body.stack.hips', label: 'pelvis', group: 'proportion',
    hint: 'The block between the legs and the torso. Taller lengthens the ' +
          'waist without lengthening the ribcage.',
    min: 7, max: 14, step: 0.5, sigma: 0.7 },
  { path: 'body.stack.torso', label: 'torso', group: 'proportion',
    hint: 'Hips to shoulders. A long torso on short legs is the classic ' +
          'dwarf silhouette.',
    min: 13, max: 24, step: 0.5, sigma: 1.2 },
  { path: 'body.stack.head', label: 'head', group: 'proportion',
    hint: 'The skull’s share of the height. A small head reads heroic, ' +
          'a big one childlike.',
    min: 12, max: 20, step: 0.5, sigma: 0.8 },
  { path: 'body.stack.foot', label: 'foot height', group: 'proportion',
    hint: 'Sole to ankle. Tall feet read as thick-soled boots.',
    min: 3, max: 6, step: 0.5, sigma: 0.3 },
  { path: 'body.armStack.upper', label: 'upper arm', group: 'proportion',
    hint: 'Shoulder to elbow. This one shares out the ARM’s length, not ' +
          'the body’s.',
    min: 8, max: 15, step: 0.5, sigma: 0.8 },
  { path: 'body.armStack.fore', label: 'forearm', group: 'proportion',
    hint: 'Elbow to wrist.',
    min: 8, max: 15, step: 0.5, sigma: 0.8 },
  { path: 'body.armStack.hand', label: 'hand', group: 'proportion',
    hint: 'Wrist to fingertip. This is also where a held weapon’s grip ' +
          'sits, so a tiny hand holds a sword close in.',
    min: 4, max: 8, step: 0.5, sigma: 0.4 },

  { path: 'shape.waist', label: 'waist', group: 'shape',
    hint: 'The torso at the belly. Low pinches it in, high barrels it out.',
    min: 0.7, max: 1.3, step: 0.01, sigma: 0.07 },
  { path: 'shape.chest', label: 'chest', group: 'shape',
    hint: 'The torso across the ribs, between the waist and the shoulders.',
    min: 0.75, max: 1.25, step: 0.01, sigma: 0.06 },
  { path: 'shape.shoulder', label: 'shoulder taper', group: 'shape',
    hint: 'How far the torso flares at the very top. This is the SLOPE of ' +
          'the shoulders; the width they flare toward is set above.',
    min: 0.8, max: 1.2, step: 0.01, sigma: 0.05 },
  { path: 'shape.chestDepth', label: 'torso depth', group: 'shape',
    hint: 'Scales the torso front-to-back only, so you can have a deep chest ' +
          'without a broad one. Side view.',
    min: 0.75, max: 1.3, step: 0.01, sigma: 0.06 },
  { path: 'shape.seat', label: 'hips and seat', group: 'shape',
    hint: 'How full the pelvis block is all the way round.',
    min: 0.8, max: 1.25, step: 0.01, sigma: 0.05 },
  { path: 'shape.armGirth', label: 'arm girth', group: 'shape',
    hint: 'Thickens or thins both arms over the limb thickness set above.',
    min: 0.8, max: 1.25, step: 0.01, sigma: 0.05 },
  { path: 'shape.legGirth', label: 'leg girth', group: 'shape',
    hint: 'Thickens or thins both legs over the limb thickness set above.',
    min: 0.8, max: 1.25, step: 0.01, sigma: 0.05 },

  { path: 'head.jaw', label: 'jaw', group: 'face',
    hint: 'The lower skull. Low tapers to a pointed chin, high squares it ' +
          'into a slab.',
    min: 0.75, max: 1.3, step: 0.01, sigma: 0.07 },
  { path: 'head.cheek', label: 'cheekbones', group: 'face',
    hint: 'The face at eye level. High gives a broad flat face, low a gaunt ' +
          'one.',
    min: 0.8, max: 1.2, step: 0.01, sigma: 0.06 },
  { path: 'head.crown', label: 'crown', group: 'face',
    hint: 'How round the top of the skull is. Low flattens it off.',
    min: 0.8, max: 1.25, step: 0.01, sigma: 0.06 },
  { path: 'head.skullBack', label: 'skull set-back', group: 'face',
    hint: 'How much cranium sits BEHIND the eyes. At 0 the skull is centred ' +
          'and the face pushes out into a snout; a human wants most of it ' +
          'behind. Side view.',
    min: 0.0, max: 1.6, step: 0.1, sigma: 0.25 },
  { path: 'face.eyeDx', label: 'eye spacing', group: 'face',
    hint: 'Half the gap between the pupils. Wide-set eyes read young, or not ' +
          'quite human.',
    min: 1.5, max: 3.5, step: 0.25, sigma: 0.3 },
  { path: 'face.eyeRow', label: 'eye height', group: 'face',
    hint: 'Nudges the eyes one row up or down from where the skull puts ' +
          'them. Up is a high brow, down is a heavy one.',
    min: -1, max: 1, step: 1, int: true, sigma: 0.5 },
  { path: 'face.eyeCols', label: 'eye size', group: 'face',
    hint: 'Columns painted per eye. 1 is a dot, 2 is a wide stare.',
    min: 1, max: 2, step: 1, int: true, sigma: 0.4 },
  { path: 'face.mouthWidth', label: 'mouth width', group: 'face',
    hint: 'How far the mouth line runs. 1 is a dot, 4 spans the face.',
    min: 1, max: 4, step: 1, int: true, sigma: 0.6 },
  { path: 'face.mouthDrop', label: 'mouth height', group: 'face',
    hint: 'How far below the eyes the mouth sits. Bigger drops lengthen the ' +
          'face and shorten the chin under it.',
    min: 2, max: 5, step: 1, int: true, sigma: 0.6 },
  { path: 'face.brow', label: 'eyebrows', group: 'face', kind: 'bool',
    hint: 'Paints a brow row above the eyes in the hair colour.',
    sigma: 0.25 },

  { path: 'hair.style', label: 'style', group: 'hair',
    hint: 'A preset of the four settings below. Picking one overwrites them; ' +
          'drag them afterwards and you are off the preset, which is fine.',
    kind: 'enum', choices: HAIR_STYLE_ORDER, sigma: 0.35 },
  { path: 'hair.back', label: 'nape line', group: 'hair',
    hint: 'How far down the BACK of the head hair reaches. Lower than the ' +
          'hairline is what makes it read as swept back.',
    // The gene goes to 99 because that is how `bald` is expressed — a line
    // above the crown, so no row qualifies. Ninety of those ninety-seven units
    // are bald, though, so a slider spanning them is dead travel: the track
    // stops at the tallest real head and `bald` is reached by picking it.
    min: 2, max: 99, uiMax: 14, step: 0.5, sigma: 1.0 },
  { path: 'hair.front', label: 'hairline', group: 'hair',
    hint: 'How far down the FOREHEAD hair reaches. Lower than the nape line ' +
          'gives a fringe over the eyes.',
    min: 2, max: 99, uiMax: 14, step: 0.5, sigma: 1.0 },
  { path: 'hair.length', label: 'mane length', group: 'hair',
    hint: 'Hair hanging below the skull, down the neck. 0 is a close cap.',
    min: 0, max: 14, step: 1, int: true, sigma: 1.5 },
  { path: 'hair.knot', label: 'topknot', group: 'hair', kind: 'bool',
    hint: 'A nub of hair above the crown.',
    sigma: 0.15 },

  // BASE FIRST, THEN ITS TONES. COLOR_SLOTS is keyed for the palette writer
  // and puts `nail` last; on screen it belongs under the skin it follows, or
  // the indent says "a tone of the thing above" while pointing at the cloth.
  ...['skin', 'skinShade', 'skinLight', 'nail',
      'hair', 'hairShade', 'eye',
      'cloth', 'clothShade'].map(k => ({
    path: 'colors.' + k, label: (COLOR_LABELS[k] || [k, ''])[0],
    hint: (COLOR_LABELS[k] || [k, ''])[1],
    // A TONE ROW SAYS SO. It is still a real swatch you can drag -- and
    // dragging it sets the relationship the carry will keep -- but a roll
    // moves it with its base rather than on its own, and a row that looked
    // identical to an independent one would be a row that lies about that.
    tone: COLOR_BASE_OF[k] || null,
    group: 'colour', kind: 'color', sigma: 0.10,
  })),
];

const SPEC_BY_PATH = new Map(GENE_SPECS.map(s => [s.path, s]));

export const getPath = (o, path) =>
  path.split('.').reduce((v, k) => (v == null ? v : v[k]), o);

export function setPath(o, path, v) {
  const keys = path.split('.');
  let cur = o;
  for (let i = 0; i < keys.length - 1; i++) {
    if (typeof cur[keys[i]] !== 'object' || cur[keys[i]] === null) cur[keys[i]] = {};
    cur = cur[keys[i]];
  }
  cur[keys[keys.length - 1]] = v;
  return o;
}

const HEX = /^#[0-9a-f]{6}$/;

function coerceGene(spec, v, fallback) {
  if (spec.kind === 'color') {
    const s = String(v || '').trim().toLowerCase();
    return HEX.test(s) ? s : fallback;
  }
  if (spec.kind === 'bool') return !!v;
  if (spec.kind === 'enum')
    return spec.choices.includes(v) ? v : fallback;
  let n = Number(v);
  if (!Number.isFinite(n)) return fallback;
  if (spec.even) n = Math.max(2, Math.round(n / 2) * 2);
  else if (spec.int) n = Math.round(n);
  return clamp(n, spec.min, spec.max);
}

/**
 * Fill in and clamp. Deep-merges over `defaultGenome()` so a stored genome
 * from an older revision keeps working when a gene is added, and runs every
 * value in GENE_SPECS through its own declared range — a genome loaded from
 * disk is not trusted any more than one typed into a slider.
 */
export function normalizeGenome(src) {
  const g = defaultGenome();
  if (src && typeof src === 'object') {
    // Scalars and strings first, so `name`/`displayName`/`archetype` survive.
    for (const k of ['name', 'displayName', 'archetype'])
      if (typeof src[k] === 'string' && src[k]) g[k] = src[k];
    for (const k of ['body', 'shape', 'head', 'face', 'hair', 'colors']) {
      if (!src[k] || typeof src[k] !== 'object') continue;
      for (const kk of Object.keys(g[k])) {
        if (!(kk in src[k])) continue;
        if (g[k][kk] && typeof g[k][kk] === 'object' && !Array.isArray(g[k][kk])) {
          for (const k3 of Object.keys(g[k][kk]))
            if (k3 in src[k][kk]) g[k][kk][k3] = src[k][kk][k3];
        } else {
          g[k][kk] = src[k][kk];
        }
      }
    }
  }
  const def = defaultGenome();
  for (const spec of GENE_SPECS)
    setPath(g, spec.path, coerceGene(spec, getPath(g, spec.path),
                                     getPath(def, spec.path)));
  return g;
}

/** Snap the hair numbers to a named style. The generator never reads `style`
 *  except through this, so a style is data and not a branch. */
export function applyHairStyle(genome, style) {
  const s = HAIR_STYLES[style];
  if (!s) return genome;
  genome.hair.style = style;
  Object.assign(genome.hair, s);
  return genome;
}

// =============================================================================
// mutation and crossover — the whole "algorithm" of interactive evolution
//
// Small on purpose. A human is the fitness function: there is no scoring, no
// tournament and no generation counter, and every line spent on one of those
// is a line not spent on the generator or the UI.
// =============================================================================

/** Genes the caller is holding fixed, as a Set of paths (or a predicate). */
const isLocked = (locks, path) =>
  typeof locks === 'function' ? locks(path)
  : locks instanceof Set ? locks.has(path)
  : Array.isArray(locks) ? locks.includes(path)
  : false;

const chanOf = hex => {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};
const hexOf = ch => '#' + ch.map(v =>
  clamp(Math.round(v), 0, 255).toString(16).padStart(2, '0')).join('');

/**
 * Nudge a colour. MOSTLY A BRIGHTNESS MOVE, and that is the whole point: an
 * independent per-channel walk is not a walk through colours, it is a walk
 * through RGB, and RGB has no idea that brown is a hair colour and purple is
 * not. Measured on the old form (a flat +-amount*255 on each channel
 * separately), five generations of the Characters page's own mutate at its
 * default sigma took #4a3728 to #5f0e65.
 *
 * Scaling all three channels by one factor holds the hue and the saturation
 * exactly and moves only the exposure, which is the axis a complexion or a
 * hair colour actually varies along. The small additive term is kept so that
 * two siblings are not the same colour at two brightnesses, but it is a
 * fraction of what it was.
 */
function jitterColor(hex, amount, rng) {
  const c = chanOf(hex);
  // SOFT WALLS AT BOTH ENDS OF THE EXPOSURE. A multiplicative walk with no
  // bound is a walk to white or to black -- ten generations at this page's
  // sigma took a skin to #ffb589, with its red channel pinned at the top of
  // the range. That matters beyond the base itself: a base whose brightest
  // channel is already 255 has no headroom for a HIGHLIGHT, so skinLight
  // clips and the carry cannot keep the tone. Capping the brightest channel
  // at 240 leaves that headroom, and the floor keeps a complexion from
  // becoming a silhouette.
  const mx = Math.max(...c, 1);
  const lift = clamp(1 + rng.norm() * amount * 0.9, 36 / mx, 240 / mx);
  return hexOf(c.map(v => v * lift + rng.norm() * amount * 40));
}

/**
 * Carry every dependent colour slot along with the base it is a tone of.
 *
 * `was` is the genome's colours BEFORE the base moved. The relationship is
 * measured as a per-channel ratio there and re-applied to the new base, so
 * whatever the author (or the palette, or a hand-dragged swatch) had set is
 * preserved rather than replaced by some canonical darkening. The +4 bias is
 * what keeps that stable near black: black hair is #1e1a17 against a shade of
 * #100e0c, and a raw ratio on a channel that is nearly zero is noise.
 */
function reshadeOne(g, base, was, locks, only) {
  const b0 = chanOf(was[base]), b1 = chanOf(g.colors[base]);
  for (const d of COLOR_FAMILIES[base]) {
    if (only && !only(d)) continue;
    // A PIN BEATS THE FAMILY. Pinning skinShade and rolling means "keep this
    // exact shade while the rest moves" -- unusual, and the only reason the
    // per-slot pins exist at all, so it has to win over the carry.
    if (isLocked(locks, 'colors.' + d)) continue;
    const d0 = chanOf(was[d]);
    let out = d0.map((v, i) => ((v + 4) / (b0[i] + 4)) * (b1[i] + 4) - 4);
    // CLIP BY EXPOSURE, NOT PER CHANNEL. A highlight on an already-bright base
    // can land outside 0..255, and clamping each channel where it happens to
    // land is what turns a warm highlight into a pink one -- the channel that
    // clips stops moving while the other two carry on, which IS a hue change,
    // and a hue change is the one thing this whole mechanism exists to
    // prevent. Scaling all three by one factor gives up brightness instead,
    // which is invisible next to the alternative.
    const mn = Math.min(...out);
    if (mn < 0) out = out.map(v => v - mn);
    const mx = Math.max(...out);
    if (mx > 255) out = out.map(v => v * 255 / mx);
    g.colors[d] = hexOf(out);
  }
  return g;
}

function reshadeFamily(g, was, locks, only) {
  for (const base of Object.keys(COLOR_FAMILIES))
    reshadeOne(g, base, was, locks, only);
  return g;
}

/**
 * One offspring of one parent. `sigma` scales every gene's declared step, so
 * 0 is a clone and 1 is a visible but recognisable relative.
 */
export function mutate(genome, sigma, rng, locks) {
  const g = normalizeGenome(genome);
  const was = { ...g.colors };            // for reshadeFamily, below
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  for (const spec of GENE_SPECS) {
    if (isLocked(locks, spec.path)) continue;
    const cur = getPath(g, spec.path);
    const s = (spec.sigma ?? 0.1) * sigma;
    if (spec.kind === 'color') {
      // A dependent slot is not jittered; it is carried, after the loop. Two
      // slots painted on the same surface must not take two independent draws
      // -- see COLOR_FAMILIES for what that produced.
      if (COLOR_BASE_OF[spec.path.slice(7)]) continue;
      setPath(g, spec.path, jitterColor(cur, s, r));
    } else if (spec.kind === 'bool') {
      if (r() < s) setPath(g, spec.path, !cur);
    } else if (spec.kind === 'enum') {
      if (r() < s) setPath(g, spec.path, r.pick(spec.choices));
    } else {
      setPath(g, spec.path, cur + r.norm() * s);
    }
  }
  // A style pick has to take its numbers with it, or the enum changes and the
  // body does not — the one place the style/number duality can bite.
  if (!isLocked(locks, 'hair.style') && g.hair.style !== genome?.hair?.style)
    applyHairStyle(g, g.hair.style);
  reshadeFamily(g, was, locks);
  return normalizeGenome(g);
}

/**
 * Offspring of two or more parents. Numeric genes BLEND (a child's build is
 * between its parents'), categorical and colour genes are INHERITED WHOLE
 * from one parent picked per gene — blending two hairstyles gives a third
 * that is neither, and blending two hair colours gives mud.
 */
export function cross(parents, rng, opts = {}) {
  const ps = (Array.isArray(parents) ? parents : [parents]).map(normalizeGenome);
  if (!ps.length) return defaultGenome();
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  const g = normalizeGenome(ps[0]);
  const blendColors = opts.blendColors !== false;
  // One random draw per colour FAMILY, made on first use and reused by every
  // slot in it. Lazy rather than pre-drawn so the sequence of calls into `r`
  // is unchanged for genomes with no dependent slots -- this function's output
  // is pinned by scripts/test_mobgen.mjs on a fixed seed.
  const draws = new Map();
  const famDraw = (fam, rr) => {
    if (!draws.has(fam)) draws.set(fam, rr());
    return draws.get(fam);
  };
  for (const spec of GENE_SPECS) {
    if (isLocked(opts.locks, spec.path)) continue;
    if (spec.kind === 'enum' || spec.kind === 'bool') {
      setPath(g, spec.path, getPath(r.pick(ps), spec.path));
    } else if (spec.kind === 'color') {
      // A colour blend between two parents is legitimate (skin tone really
      // does average); between three it is grey. Two parents blend, more pick.
      //
      // ONE DRAW PER FAMILY, not per slot. Each slot used to take its own `t`
      // (or its own parent), so a child could inherit a pale father's skin and
      // a dark mother's skinShade -- and those two are painted on the same
      // surface, a few cells apart, so the seam down its side was two
      // different people. `famDraw` hands every slot in a family the same
      // draw, which is what makes a child's colouring a colouring.
      // A DEPENDENT SLOT IS NOT INHERITED AT ALL -- it is re-derived from the
      // child's own base after the loop, using the ratios of whichever parent
      // the base came mostly from. Blending it would have been enough to keep
      // a pale father's skin off a deep mother's skinShade, but not enough for
      // `nail`: no complexion declares one, so both parents carry the SAME
      // default and no blend weight can move it. A child with a deep
      // complexion and the default's pale fingertips looks like it is wearing
      // gloves, and that is a one-generation break, not a drift.
      const slot = spec.path.slice(7);
      if (COLOR_BASE_OF[slot]) continue;
      const t = famDraw(slot, r);
      if (blendColors && ps.length === 2) {
        const [a, b] = ps.map(p => parseInt(getPath(p, spec.path).slice(1), 16));
        const mixc = sh => clamp(Math.round(((a >> sh) & 255) * (1 - t) +
                                            ((b >> sh) & 255) * t), 0, 255);
        setPath(g, spec.path, '#' + [16, 8, 0].map(sh =>
          mixc(sh).toString(16).padStart(2, '0')).join(''));
      } else {
        setPath(g, spec.path,
                getPath(ps[Math.min(ps.length - 1,
                                    Math.floor(t * ps.length))], spec.path));
      }
    } else {
      // Weighted average with a random simplex weight, which spreads a litter
      // across the segment between the parents instead of piling it in the
      // middle (a plain mean of two parents gives one child, N times).
      const w = ps.map(() => r() + 0.001);
      const tot = w.reduce((s, v) => s + v, 0);
      let acc = 0;
      ps.forEach((p, i) => { acc += getPath(p, spec.path) * w[i] / tot; });
      setPath(g, spec.path, acc);
    }
  }
  // Hair numbers come from whichever parent supplied the style, so the child
  // has A hairstyle rather than the average of two.
  const donor = ps.find(p => p.hair.style === g.hair.style) || ps[0];
  if (!isLocked(opts.locks, 'hair.style'))
    Object.assign(g.hair, { back: donor.hair.back, front: donor.hair.front,
                            length: donor.hair.length, knot: donor.hair.knot });
  // Each family's tones come from the parent its BASE came mostly from, so the
  // child gets one person's relationship between a skin and its shade rather
  // than an average of two.
  for (const fam of Object.keys(COLOR_FAMILIES)) {
    const t = draws.get(fam) ?? 0;
    reshadeOne(g, fam,
               ps[Math.min(ps.length - 1, Math.floor(t * ps.length))].colors,
               opts.locks);
  }
  if (opts.sigma) return mutate(g, opts.sigma, r, opts.locks);
  return normalizeGenome(g);
}

/** A fresh character from nothing: every unlocked gene drawn uniformly across
 *  its declared range, which is what the range is FOR. */
export function randomGenome(rng, locks) {
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  const g = defaultGenome();
  const was = { ...g.colors };
  for (const spec of GENE_SPECS) {
    if (isLocked(locks, spec.path)) continue;
    if (spec.kind === 'color') {
      if (COLOR_BASE_OF[spec.path.slice(7)]) continue;      // carried, below
      setPath(g, spec.path, jitterColor(getPath(g, spec.path), 0.30, r));
    } else if (spec.kind === 'bool') {
      setPath(g, spec.path, r() < 0.4);
    } else if (spec.kind === 'enum') {
      setPath(g, spec.path, r.pick(spec.choices));
    } else {
      // Pulled toward the default rather than flat across the range: a body
      // with every gene independently at an extreme is not a rare character,
      // it is a broken one, and at ten genes flat sampling produces almost
      // nothing else.
      const d = getPath(g, spec.path);
      const t = (r() + r()) * 0.5;                       // triangular, centred
      setPath(g, spec.path, d + (t - 0.5) * 2 * Math.max(d - spec.min,
                                                         spec.max - d));
    }
  }
  applyHairStyle(g, g.hair.style);
  // Hair length and the knot belong to the style; re-roll length within it so
  // a `long` is not always exactly 6 micro.
  if (g.hair.length > 0) g.hair.length = Math.round(g.hair.length * (0.6 + r()));
  reshadeFamily(g, was, locks);
  return normalizeGenome(g);
}

/** Skin/hair/eye sets that read as PEOPLE rather than as a colour wheel.
 *  Picked as a set because the failure mode of independent colour genes is a
 *  character with slate skin and orange hair, which no amount of shape work
 *  rescues. `randomGenome` jitters within a set; it does not cross sets. */
export const COMPLEXIONS = {
  pale:   { skin: '#e8b898', skinShade: '#c2906f', skinLight: '#f6d6bd' },
  fair:   { skin: '#d9a97f', skinShade: '#b07f58', skinLight: '#efc9a6' },
  olive:  { skin: '#c98d63', skinShade: '#a06e4b', skinLight: '#e2b189' },
  tan:    { skin: '#a9714a', skinShade: '#7f5335', skinLight: '#c99468' },
  brown:  { skin: '#7d4e2e', skinShade: '#5b361f', skinLight: '#9c6c48' },
  deep:   { skin: '#4e3020', skinShade: '#372014', skinLight: '#6b4531' },
};
export const HAIR_COLORS = {
  black:  { hair: '#1e1a17', hairShade: '#100e0c' },
  dark:   { hair: '#4a3728', hairShade: '#33251a' },
  brown:  { hair: '#6b4a2e', hairShade: '#4a3220' },
  auburn: { hair: '#7a3a22', hairShade: '#552615' },
  sandy:  { hair: '#a8823f', hairShade: '#7c5e2a' },
  blond:  { hair: '#cfa85e', hairShade: '#9c7a3c' },
  grey:   { hair: '#8e8a83', hairShade: '#66625c' },
  white:  { hair: '#d8d3cb', hairShade: '#a8a39b' },
};
export const EYE_COLORS = {
  dark: '#241d18', brown: '#3b2a1c', hazel: '#5a4222',
  green: '#3a5236', blue: '#37506b', grey: '#525b60',
};
export const CLOTH_COLORS = {
  linen:  { cloth: '#7a6a4e', clothShade: '#5c4f39' },
  ash:    { cloth: '#6a6a66', clothShade: '#4e4e4b' },
  madder: { cloth: '#8a4438', clothShade: '#663027' },
  woad:   { cloth: '#44566f', clothShade: '#313f52' },
  moss:   { cloth: '#5a6a44', clothShade: '#414e31' },
  umber:  { cloth: '#6b4f33', clothShade: '#4d3824' },
};

/** A palette roll that picks SETS, then jitters inside them. */
export function rollColors(genome, rng) {
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  const pick = obj => obj[r.pick(Object.keys(obj))];
  const set = Object.assign({}, pick(COMPLEXIONS), pick(HAIR_COLORS),
                            pick(CLOTH_COLORS), { eye: pick(EYE_COLORS) });
  const was = { ...genome.colors };
  Object.assign(genome.colors, set);
  // A TONE THE PALETTE DOES NOT SUPPLY IS CARRIED -- in practice that is
  // `nail`, which no complexion declares and which has to follow the skin
  // anyway: a deep complexion with the default's pale fingertips is a
  // character wearing gloves. The tones the palette DOES supply are taken
  // verbatim, because a deep skin darkens toward its shade far more than a
  // pale one and that is hand-tuned per complexion rather than a multiplier.
  reshadeFamily(genome, was, null, d => !(d in set));
  // The sets are authored as whole families, so the only thing left that can
  // pull one apart is the jitter -- which is why it, too, moves the base and
  // carries the tones. At 0.02 the drift per roll is small; it is the LOOP
  // (roll, keep, roll again from the keeper) that makes small drift add up.
  const beforeJitter = { ...genome.colors };
  for (const k of Object.keys(COLOR_SLOTS))
    if (!COLOR_BASE_OF[k])
      genome.colors[k] = jitterColor(genome.colors[k], 0.02, r);
  reshadeFamily(genome, beforeJitter);
  return genome;
}

// =============================================================================
// shape helpers — ported from gen_human.py, semantics unchanged
// =============================================================================

/** Mirror front-to-back once, at the end. Every builder is authored in the
 *  natural "face at high y" reading and flipped here, which keeps shape
 *  placement and limb placement using ONE idea of "front". */
function flipY(size, cells) {
  const sy = size[1];
  return cells.map(([x, y, z, c]) => [x, sy - 1 - y, z, c]);
}

/** True when voxel (x,y)'s CENTRE is inside the axis-aligned ellipse. Voxel i
 *  covers [i, i+1) so its centre is i+0.5 — that half is added here, once. */
function ellipseMask(x, y, cx, cy, rx, ry) {
  if (rx <= 0 || ry <= 0) return false;
  const dx = (x + 0.5 - cx) / rx;
  const dy = (y + 0.5 - cy) / ry;
  return dx * dx + dy * dy <= 1.0;
}

/** Linear interpolation over an ascending (u, value) table. The torso, pelvis
 *  and skull are all "a stack of ellipses whose radii follow a curve", and a
 *  table shows you the waist, the chest and the shoulder line as three
 *  numbers. */
function profile(u, pts) {
  if (u <= pts[0][0]) return pts[0][1];
  for (let i = 0; i + 1 < pts.length; i++) {
    const [u0, v0] = pts[i], [u1, v1] = pts[i + 1];
    if (u <= u1) {
      const t = u1 > u0 ? (u - u0) / (u1 - u0) : 0.0;
      return v0 + (v1 - v0) * t;
    }
  }
  return pts[pts.length - 1][1];
}

/** A vertical (z-axis) limb: a tube whose XY radius lerps r0 -> r1.
 *
 *  WHY THE TAPER IS INVISIBLE, so nobody spends an hour tuning it. On a
 *  4-wide lattice the ellipse test has exactly two outcomes: r >= ~1.57 gives
 *  the 12-cell rounded square and anything below gives a bare 2x2. There is
 *  no cross-section in between, so r0/r1 select "limb" or "twig" and real
 *  taper comes from the BOX SIZES. Kept as parameters because the same helper
 *  at a wider box (limbWidth 6 or 8) does taper. */
function limbTube(size, r0, r1, colFor) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const out = [];
  for (let z = 0; z < sz; z++) {
    const t = z / Math.max(sz - 1, 1);
    const r = r0 + (r1 - r0) * t;
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++)
        if (ellipseMask(x, y, cx, cy, r, r))
          out.push([x, y, z, colFor(x, y, z, sx, sy, sz)]);
  }
  return out;
}

/** The one shading rule the whole figure uses: cells on the away-facing side
 *  take the darker slot. In the AUTHORED frame front is +y, so "back" is low
 *  y. Deliberately a REGION rule and not a surface rule — a surface rule
 *  repaints whatever happens to be outermost, so it moves when the silhouette
 *  changes and it speckles wherever the surface is one cell thick. */
function shadeBack(y, cy, base, shade, depth = 1.8) {
  return y + 0.5 < cy - depth ? shade : base;
}

// =============================================================================
// the limb table, DERIVED from the genome
//
// name -> { size, mn } in AUTHORED micro (4 per world voxel). Scene is Z-up
// and engine Y = scene Z, so the z column IS the height.
// =============================================================================

/** Spend a height budget over weighted segments, landing EXACTLY on it.
 *  Proportional, rounded, with the residual given to the largest segment —
 *  the height contract is not negotiable (a 16.6-micro head does not exist)
 *  and silently ending up one micro short is how a figure stops standing on
 *  z = 0. */
function spendBudget(weights, budget) {
  const keys = Object.keys(weights);
  const tot = keys.reduce((s, k) => s + weights[k], 0);
  const out = {};
  let used = 0;
  for (const k of keys) {
    out[k] = Math.max(1, Math.round(weights[k] * budget / tot));
    used += out[k];
  }
  let residual = budget - used;
  // Hand the slack out one micro at a time, largest first, so no segment
  // absorbs a visible lump and none can be driven below 1.
  const order = keys.slice().sort((a, b) => out[b] - out[a]);
  for (let i = 0; residual !== 0; i = (i + 1) % order.length) {
    const k = order[i];
    if (residual < 0 && out[k] <= 1) continue;
    out[k] += residual > 0 ? 1 : -1;
    residual += residual > 0 ? -1 : 1;
  }
  return out;
}

/** The centred min corner of a box of depth `sy`. Odd depths (the foot) round
 *  AWAY from the face, which is where the heel is. */
const mnY = sy => -Math.round(sy / 2);

/**
 * Build the authored limb table for a genome.
 * Returns { limbs: {name: {size:[x,y,z], mn:[x,y,z]}}, microH, worldH, eyeZ,
 *           rows } where `rows` is the vertical stack, for the UI to show.
 */
export function limbTable(genome) {
  const g = genome;
  const b = g.body;
  const microH = Math.round(g.body.heightM * AUTHORED_VOXELS_PER_METRE);
  const worldH = Math.round(g.body.heightM * AUTHORED_VOXELS_PER_METRE / ART_SCALE);
  // The eye row, in AUTHORED scene z. DERIVED from the eye metre contract,
  // which is what makes the first-person camera land on the face instead of
  // inside the skull. gen_mina.py restated it and her face void has silently
  // not been carved since the 2x upscale landed.
  const eyeM = DEFAULT_EYE_M * (g.body.heightM / DEFAULT_HEIGHT_M);
  const eyeZ = Math.round(eyeM * AUTHORED_VOXELS_PER_METRE);

  const overlapTotal = ARCHETYPE.stack.reduce((s, e) => s + e.overlap, 0);
  const h = spendBudget(b.stack, microH + overlapTotal);

  // Walk the stack sole to crown: base = previous top - overlap.
  const rows = {};
  let top = 0;
  for (const e of ARCHETYPE.stack) {
    const base = top - e.overlap;
    rows[e.gene] = { base, h: h[e.gene], top: base + h[e.gene] };
    top = rows[e.gene].top;
  }
  // `spendBudget` guarantees the sum; this asserts the WALK, which is the part
  // an added overlap or a reordered stack would break.
  if (rows.foot.base !== 0 || top !== microH)
    throw new Error(`stack does not span 0..${microH} (got ` +
                    `${rows.foot.base}..${top})`);

  const lw = b.limbWidth;
  const limbs = {};
  const centred = (name, sx, sy, sz, z) => {
    limbs[name] = { size: [sx, sy, sz], mn: [-sx / 2, mnY(sy), z] };
  };
  centred('hips', b.hipWidth, b.bodyDepth, rows.hips.h, rows.hips.base);
  centred('torso', b.shoulderWidth, b.bodyDepth, rows.torso.h, rows.torso.base);
  centred('head', b.headWidth, b.headDepth, rows.head.h, rows.head.base);

  // ARMS HANG FLUSH AGAINST THE SHOULDER LINE. The torso's box is
  // x -sx/2..sx/2-1 but what it FILLS at its widest is one column narrower
  // either side (see the radius note in torsoProfile), so an arm at
  // sx/2 - 1 touches the shoulder and no lower. A 1-micro gap breaks up an
  // unbroken robe; on a bare shoulder it is a detached arm.
  const armX = b.shoulderWidth / 2 - 1;
  // The arm hangs from the shoulder to the pelvis line. Same budget arithmetic
  // as the vertical stack and the same trap: the SPAN is the sum of the
  // segments MINUS their overlaps, so the budget handed to spendBudget is the
  // span PLUS them. Getting this backwards shortened every arm segment by 8%
  // and left a one-micro gap at the elbow, which the "does it touch its
  // parent" assert caught and a thumbnail never would have.
  const armOverlap = ARCHETYPE.armStack.reduce((s, e) => s + e.overlap, 0);
  const armSpan = rows.torso.top - rows.hips.base;
  const armH = spendBudget(b.armStack, armSpan + armOverlap);
  let armTop = rows.torso.top;
  const armRows = {};
  for (const e of ARCHETYPE.armStack) {
    // `overlap` is how far this segment's TOP reaches ABOVE its parent's base,
    // i.e. how deep the elbow/wrist sits inside the bone above it.
    const t = armTop + e.overlap;
    armRows[e.gene] = { base: t - armH[e.gene], h: armH[e.gene], top: t };
    armTop = t - armH[e.gene];
  }
  // THE LEGS ARE PUSHED OUT to the pelvis edge so the inner columns stay
  // empty. Two adjacent 4-wide limb boxes render as one 8-wide column with no
  // inner edge at all (see limbTube) — a skirt hides that and a bare body
  // does not.
  const legX = Math.max(0, b.hipWidth / 2 - lw);
  for (const side of ARCHETYPE.sides) {
    const put = (name, x, sy, sz, z) => {
      limbs[name] = { size: [lw, sy, sz],
                      mn: [side === 'L' ? x : -x - lw, mnY(sy), z] };
    };
    put(`armU.${side}`, armX, lw, armRows.upper.h, armRows.upper.base);
    put(`armL.${side}`, armX, lw, armRows.fore.h, armRows.fore.base);
    put(`hand.${side}`, armX, lw, armRows.hand.h, armRows.hand.base);
    put(`legU.${side}`, legX, lw, rows.thigh.h, rows.thigh.base);
    put(`legL.${side}`, legX, lw, rows.shin.h, rows.shin.base);
    limbs[`foot.${side}`] = {
      size: [lw, b.footDepth, rows.foot.h],
      mn: [side === 'L' ? legX : -legX - lw, mnY(b.footDepth), rows.foot.base],
    };
  }

  // The mirror check, cheap, and it catches the one class of typo a
  // screenshot from the front cannot: a limb whose left and right boxes are
  // not reflections about x = 0.
  for (const n of Object.keys(limbs)) {
    if (!n.endsWith('.L')) continue;
    const a = limbs[n], r = limbs[n.slice(0, -2) + '.R'];
    if (!r || String(a.size) !== String(r.size) ||
        a.mn[1] !== r.mn[1] || a.mn[2] !== r.mn[2] ||
        r.mn[0] !== -a.mn[0] - a.size[0])
      throw new Error(`${n} and its .R twin are not mirrors about x=0`);
  }
  return { limbs, rows, armRows, microH, worldH, eyeZ };
}

/** The DEFAULT table, used as the denominator every time a shape parameter is
 *  scaled by "this box over the drawn box". Computed once. */
const DEF_TABLE = limbTable(defaultGenome());
const DEF = DEF_TABLE.limbs;

// =============================================================================
// per-limb builders
// =============================================================================

/** Scale an absolute authored radius list by this box's own size. See the
 *  banner: the ratio is exactly 1.0 at the default genome, so the default
 *  figure is bit-identical to the Python's. */
const scaleKnots = (knots, ratio, mods) =>
  knots.map(([u, r], i) => [u, r * ratio * (mods ? mods(u, i) : 1)]);

function torsoProfiles(g, size) {
  // RADII ARE KEPT CLEAR OF THE BOX EDGE ON PURPOSE. The test is an ellipse in
  // x AND y, so a radius that only just reaches the outermost column admits
  // one or two cells at mid-depth and none either side — a single 2x2x2
  // pimple on the shoulder line at ship scale, on exactly the row where the
  // curve rounds up. Every radius tops out where the widest column still
  // fills four depth rows.
  const rx = [[0.00, 3.5], [0.30, 3.9], [0.62, 4.8], [0.88, 5.2], [1.00, 5.0]];
  const ry = [[0.00, 2.7], [0.35, 3.2], [0.70, 3.4], [1.00, 2.8]];
  const s = g.shape;
  const mod = u => (u < 0.4 ? s.waist : u < 0.75 ? s.chest : s.shoulder);
  return {
    rx: scaleKnots(rx, size[0] / DEF.torso.size[0], mod),
    ry: scaleKnots(ry, size[1] / DEF.torso.size[1], () => s.chestDepth),
  };
}

function torsoVox(g, size) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const { rx: prx, ry: pry } = torsoProfiles(g, size);
  const out = [];
  for (let z = 0; z < sz; z++) {
    const u = z / Math.max(sz - 1, 1);
    const rx = profile(u, prx), ry = profile(u, pry);
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++)
        if (ellipseMask(x, y, cx, cy, rx, ry))
          out.push([x, y, z, shadeBack(y, cy, ART.SKIN_BASE, ART.SKIN_SHADE)]);
  }
  return flipY(size, out);
}

/** The pelvis, wearing the figure's only clothing.
 *
 *  THE SHORTS ARE PAINT, NOT GEOMETRY, and not a second material either: three
 *  rows of CLOTH slots on a body that is `skin` all the way through. That is
 *  what leaves the armour system a clean surface — an equipped greave has to
 *  fit a leg, not a leg-plus-a-hem — and the bare rows above the waistband are
 *  what tell you at a glance that the torso is unarmoured. */
function hipsVox(g, size) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const rxr = sx / DEF.hips.size[0], ryr = sy / DEF.hips.size[1];
  const prx = scaleKnots([[0.00, 4.4], [0.40, 4.9], [1.00, 4.6]], rxr,
                         () => g.shape.seat);
  const pry = scaleKnots([[0.00, 2.8], [0.50, 3.3], [1.00, 3.0]], ryr);
  const waist = sz - 3;      // the waistband row; everything above it is bare
  const out = [];
  for (let z = 0; z < sz; z++) {
    const u = z / Math.max(sz - 1, 1);
    const rx = profile(u, prx), ry = profile(u, pry);
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++) {
        if (!ellipseMask(x, y, cx, cy, rx, ry)) continue;
        let col;
        if (z > waist) col = shadeBack(y, cy, ART.SKIN_BASE, ART.SKIN_SHADE);
        else if (z === waist) col = ART.CLOTH_SHADE;
        else col = shadeBack(y, cy, ART.CLOTH, ART.CLOTH_SHADE);
        out.push([x, y, z, col]);
      }
  }
  return flipY(size, out);
}

/**
 * A bare head: neck stub, skull, a swept hair cap, two eyes and a mouth.
 *
 * THE EYE ROW IS DERIVED, NOT WRITTEN DOWN. `eyeZ` is the figure's eye row
 * minus this limb's own scene z, both in AUTHORED micro. gen_mina.py did the
 * same computation against a table that had already been rebound to the
 * SHIPPING scale, so hers worked out to -32, every face test failed, and her
 * face void was silently never carved. The throw below is the backstop.
 */
function headVox(g, size, ctx) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const hcy = cy - g.head.skullBack * (sy / DEF.head.size[1]);
  const eyeZ = ctx.eyeZ - ctx.limbs.head.mn[2] + g.face.eyeRow;
  if (!(eyeZ >= 0 && eyeZ < sz))
    throw new Error(`eye row ${eyeZ} is outside the head model (0..${sz - 1}); ` +
                    `the head's scene z or its height moved without the eye ` +
                    `metre contract following`);

  const neckTop = Math.max(1, Math.round(2 * sz / DEF.head.size[2]));
  const skullLo = neckTop + 1;
  // Radii as a fraction u of the skull's own height. Read as a face: chin,
  // jaw, cheekbones, temples, the round of the crown.
  const hm = g.head;
  const mod = u => (u < 0.30 ? hm.jaw : u < 0.72 ? hm.cheek : hm.crown);
  const prx = scaleKnots([[0.00, 2.3], [0.18, 3.4], [0.40, 4.2], [0.70, 4.3],
                          [0.88, 3.8], [1.00, 2.4]], sx / DEF.head.size[0], mod);
  const pry = scaleKnots([[0.00, 2.7], [0.18, 3.7], [0.45, 4.6], [0.75, 4.6],
                          [1.00, 2.9]], sy / DEF.head.size[1], mod);
  const neckR = 2.0 * (sx / DEF.head.size[0]);

  // HAIR IS A FUNCTION: the z at or above which this depth row is hair, lower
  // at the nape than at the forehead, which is what makes the cap read as
  // swept hair rather than a painted-on helmet. Both ends scale with the head
  // so a small skull is not bald by arithmetic.
  const hz = sz / DEF.head.size[2], hy = sy / DEF.head.size[1];
  const hBack = g.hair.back * hz, hFront = g.hair.front * hz;
  const hairLine = y => {
    const t = clamp((y + 0.5 - (hcy - 4.0 * hy)) / (8.0 * hy), 0.0, 1.0);
    return hBack + (hFront - hBack) * t;
  };

  const out = [];
  for (let z = 0; z < sz; z++) {
    let rx, ry;
    if (z <= neckTop) { rx = neckR; ry = neckR; }
    else {
      const u = (z - skullLo) / Math.max(sz - 1 - skullLo, 1);
      rx = profile(u, prx); ry = profile(u, pry);
    }
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++) {
        if (!ellipseMask(x, y, cx, hcy, rx, ry)) continue;
        const back = y + 0.5 < hcy - 1.5;
        let col;
        if (z >= skullLo && z >= hairLine(y))
          col = back ? ART.HAIR_SHADE : ART.HAIR;
        else col = back ? ART.SKIN_SHADE : ART.SKIN_BASE;
        out.push([x, y, z, col]);
      }
  }

  const has = new Set(out.map(([x, y, z]) => x + ',' + y + ',' + z));
  const add = (x, y, z, c) => {
    const k = x + ',' + y + ',' + z;
    if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return;
    if (has.has(k)) return;
    has.add(k);
    out.push([x, y, z, c]);
  };

  // A MANE: hair hanging behind the skull, down the nape. New cells below
  // where the cap ends; the neck stub's own ellipse is far narrower, so
  // nothing conflicts.
  //
  // THE RADIUS IS THE SKULL'S AT ITS OWN BASE (u = 0), NOT A WIDER RING. Every
  // mane cell then has a filled skull cell directly above it at the same
  // (x, y), so the mane is connected BY CONSTRUCTION. A wider ring reads as
  // more hair and leaves a lateral fringe hanging off nothing, which the
  // anatomy peel turns into a floating scrap of flesh.
  if (g.hair.length > 0 && hBack < sz) {
    const lz = Math.round(g.hair.length * hz);
    const rx = profile(0.0, prx), ry = profile(0.0, pry);
    for (let z = Math.max(0, skullLo - lz); z < skullLo; z++)
      for (let y = 0; y < sy; y++)
        for (let x = 0; x < sx; x++) {
          if (!ellipseMask(x, y, cx, hcy, rx, ry)) continue;
          if (y + 0.5 >= hcy - 1.0) continue;          // back half only
          add(x, y, z, ART.HAIR_SHADE);
        }
  }
  // A TOPKNOT: a bun on the back of the crown. GROWN FROM AN EXISTING CELL
  // rather than placed at an absolute row — the head box's top row is already
  // full at the centre, so there is no space above the crown to put a nub in,
  // and an absolutely-placed one either vanishes into the skull or floats
  // behind it depending on the head's depth.
  if (g.hair.knot) {
    const kd = Math.max(1, Math.round(2 * hy));      // how far back it bulges
    for (const x of [Math.floor(cx) - 1, Math.floor(cx)])
      for (let z = sz - Math.max(2, Math.round(3 * hz)); z < sz; z++) {
        // The back-most filled cell in this column, and the cells behind it.
        let backY = -1;
        for (let y = 0; y < sy; y++)
          if (has.has(x + ',' + y + ',' + z)) { backY = y; break; }
        if (backY <= 0) continue;
        for (let d = 1; d <= kd && backY - d >= 0; d++)
          add(x, backY - d, z, ART.HAIR_SHADE);
      }
  }

  // EARS: appended, not carved. They sit one cell PROUD OF THE WIDEST FILLED
  // COLUMN at the ear row, which is where the skull's own ellipse left empty
  // space — so there is nothing to conflict with and the dedup below never has
  // to arbitrate.
  //
  // Measured off the geometry rather than placed at x = 0 and sx - 1. On the
  // drawn head those are the same two cells (the ellipse reaches x = 1..sx-2
  // at the ear row), but a narrower skull does not fill the second column
  // either, and an ear at the box edge then hangs in the air — 32 detached
  // voxels on the `lanky` preset before this, which the "one connected piece"
  // assert caught and the preview did not.
  const earY = pyRound(hcy) - 1;
  for (const z of [eyeZ - 1, eyeZ]) {
    let loX = -1, hiX = -1;
    for (let x = 0; x < sx; x++)
      if (has.has(x + ',' + earY + ',' + z)) { if (loX < 0) loX = x; hiX = x; }
    if (loX < 0) continue;                    // no skull on this row: no ears
    if (loX - 1 >= 0) out.push([loX - 1, earY, z, ART.SKIN_SHADE]);
    if (hiX + 1 < sx) out.push([hiX + 1, earY, z, ART.SKIN_SHADE]);
  }

  // Eyes and mouth are repaints of the FRONTMOST existing cell in their
  // column, so they can never float in front of the face or sink into it.
  const paintFront = (cols, z, col) => {
    const front = new Map();
    for (let i = 0; i < out.length; i++) {
      const [x, y, zz] = out[i];
      if (zz !== z || !cols.has(x)) continue;
      if (!front.has(x) || y > out[front.get(x)][1]) front.set(x, i);
    }
    for (const i of front.values()) out[i] = [out[i][0], out[i][1], out[i][2], col];
  };
  const eyeDx = g.face.eyeDx * (sx / DEF.head.size[0]);
  const eyeCols = new Set();
  for (let k = 0; k < g.face.eyeCols; k++) {
    eyeCols.add(Math.trunc(cx - eyeDx) + k);
    eyeCols.add(Math.trunc(cx + eyeDx) - k);
  }
  paintFront(eyeCols, eyeZ, ART.EYE);
  const mw = g.face.mouthWidth;
  const mouthCols = new Set();
  for (let k = 0; k < mw; k++) mouthCols.add(Math.trunc(cx) - 1 + k - ((mw - 2) >> 1));
  paintFront(mouthCols, eyeZ - g.face.mouthDrop, ART.SKIN_SHADE);
  if (g.face.brow) {
    const browCols = new Set();
    for (const c of eyeCols) { browCols.add(c); browCols.add(c + (c < cx ? 1 : -1)); }
    paintFront(browCols, Math.min(sz - 1, eyeZ + 2), ART.HAIR_SHADE);
  }
  return flipY(size, out);
}

function upperArmVox(g, size) {
  const r = g.shape.armGirth * (size[0] / DEF['armU.L'].size[0]);
  return flipY(size, limbTube(size, 2.0 * r, 2.2 * r,
    (x, y, z, sx, sy) => shadeBack(y, sy * 0.5, ART.SKIN_BASE, ART.SKIN_SHADE)));
}

function foreArmVox(g, size) {
  const r = g.shape.armGirth * (size[0] / DEF['armL.L'].size[0]);
  return flipY(size, limbTube(size, 1.8 * r, 2.0 * r,
    (x, y, z, sx, sy) => shadeBack(y, sy * 0.5, ART.SKIN_BASE, ART.SKIN_SHADE)));
}

/** A mitten hand with a thumb nub. At 4 micro across there is no room for
 *  separate fingers — inventing five here produces speckle, not a hand. */
function handVox(g, size) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const rr = g.shape.armGirth * (sx / DEF['hand.L'].size[0]);
  const out = [];
  for (let z = 0; z < sz; z++) {
    const t = z / Math.max(sz - 1, 1);
    const r = (1.6 + 0.6 * t) * rr;        // narrower at the fingertips (low z)
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++)
        if (ellipseMask(x, y, cx, cy, r, r * 0.85))
          out.push([x, y, z, shadeBack(y, cy, ART.SKIN_BASE, ART.SKIN_SHADE)]);
  }
  // THE THUMB: one cell PROUD OF THE FRONT-MOST filled cell in its own column,
  // not at an absolute row. On the drawn hand those are the same cell, but a
  // slimmer hand does not reach the front of its box and an absolutely-placed
  // thumb then floats a cell off the knuckles (caught by the "one connected
  // piece" assert on two of twenty mutants).
  const thumbX = Math.trunc(cx) + 1, thumbZ = sz - 2;
  let frontY = -1;
  for (const [x, y, z] of out)
    if (x === thumbX && z === thumbZ && y > frontY) frontY = y;
  if (frontY >= 0 && frontY + 1 < sy)
    out.push([thumbX, frontY + 1, thumbZ, ART.SKIN_BASE]);
  return flipY(size, out);
}

/** Upper leg. The top rows carry the short's leg, so the hem lands on the
 *  THIGH rather than at the pelvis seam — a hem exactly on a joint pops when
 *  the leg swings. */
function thighVox(g, size) {
  const sz = size[2];
  const hem = sz - 4;
  const r = g.shape.legGirth * (size[0] / DEF['legU.L'].size[0]);
  const col = (x, y, z, sx, sy) => {
    if (z === hem) return ART.CLOTH_SHADE;
    if (z > hem) return shadeBack(y, sy * 0.5, ART.CLOTH, ART.CLOTH_SHADE);
    return shadeBack(y, sy * 0.5, ART.SKIN_BASE, ART.SKIN_SHADE);
  };
  return flipY(size, limbTube(size, 1.9 * r, 2.1 * r, col));
}

function shinVox(g, size) {
  const r = g.shape.legGirth * (size[0] / DEF['legL.L'].size[0]);
  return flipY(size, limbTube(size, 1.7 * r, 2.0 * r,
    (x, y, z, sx, sy) => shadeBack(y, sy * 0.5, ART.SKIN_BASE, ART.SKIN_SHADE)));
}

/** A bare foot: a sole reaching forward from an ankle column at the back. The
 *  ankle anchor's relationship to the sole is what `rideHeight` is trimmed
 *  against (see gaitNumbers), so the two move together or the figure hovers. */
function footVox(g, size) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5;
  const yr = sy / DEF['foot.L'].size[1], xr = sx / DEF['foot.L'].size[0];
  const soleRows = Math.max(1, Math.round(sz / 2));
  const ankleDepth = 3 * yr;
  const out = [];
  for (let z = 0; z < sz; z++)
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++) {
        const inSole = z < soleRows;
        const inAnkle = y < ankleDepth && z >= soleRows;
        if (!(inSole || inAnkle)) continue;
        if (!ellipseMask(x, inAnkle ? y : 2 * yr, cx, 2.0 * yr,
                         1.9 * xr, 2.4 * yr)) continue;
        out.push([x, y, z, z === 0 ? ART.SKIN_SHADE : ART.SKIN_BASE]);
      }
  return flipY(size, out);
}

/** name -> builder. The one place the name vocabulary meets the geometry. */
const SHAPES = {
  hips: hipsVox, torso: torsoVox, head: headVox,
  'armU.L': upperArmVox, 'armU.R': upperArmVox,
  'armL.L': foreArmVox, 'armL.R': foreArmVox,
  'hand.L': handVox, 'hand.R': handVox,
  'legU.L': thighVox, 'legU.R': thighVox,
  'legL.L': shinVox, 'legL.R': shinVox,
  'foot.L': footVox, 'foot.R': footVox,
};

// =============================================================================
// THE SHOULDER ROUND
//
// The shipped assets/mobs/human.vox has had its shoulders rounded BY HAND in
// the model editor, and this is that edit expressed as a rule so every
// generated character gets it. Measured cell for cell against the shipped file
// (see scripts/test_mobgen.mjs section K): the hand edit removes 87 cells from
// the torso and ~20 from each upper arm, adds none, and touches nothing else —
// so the whole divergence between the shipped human and its own generator is
// this one piece of sculpting.
//
// IT IS TWO DIFFERENT ARTEFACTS, hence two rules:
//
//  1. A FIN on the torso. Along the rows where the silhouette widens toward
//     the shoulder line, the ellipse reaches the outermost column at mid-depth
//     ONLY — a one-column-wide blade sticking out of the shoulder over a third
//     of the body's depth. gen_human.py's own comment claims its radii were
//     chosen to avoid exactly this ("a single 2x2x2 pimple on the shoulder line
//     at ship scale"); measured, they are not. It is not an upscale artefact
//     either: the ellipse genuinely reaches that column, and removing it is
//     removing real geometry, because a silhouette that comes to a one-cell
//     point does not read as a shoulder at this resolution.
//
//  2. A SQUARE CAP on each upper arm. The tube's radius grows to 2.2 at the
//     top, and on a 4-wide AUTHORED lattice everything from r = 1.06 upward is
//     the full 4x4 square — so the arm ends in a flat slab with four sharp
//     corners, and the 2x upscale turns each of those into a 2x2 block. That
//     one IS resolution: the art lattice has no cross-section between "square"
//     and "rounded square", which the limbTube note already says in as many
//     words.
//
// BOTH RUN AFTER THE UPSCALE, at the shipped lattice, because that is the only
// place either can be fixed. The authored lattice steps 4 cells to 12 to 16
// with nothing in between; the shipped one has the resolution to round.
//
// WHAT IS DELIBERATELY NOT COPIED: the hand edit is ASYMMETRIC (armU.L lost 23
// cells over five rows, armU.R lost 20 over two) and it stops part-way up the
// torso. That is where the sculptor's hand stopped, not a decision — and this
// generator asserts left/right mirroring on every body it makes, so copying the
// asymmetry would mean breaking an invariant to reproduce a slip. The rules
// below are symmetric and cover the whole shoulder.
// =============================================================================

/**
 * @param cells [x,y,z,col] at the SHIPPED lattice, local to the part
 * @param size  the part's shipped box
 * @param rule  'fin' | 'cap'
 * @returns a new cell list
 */
function roundShoulders(cells, size, rule) {
  const [sx, sy, sz] = size;
  const key = (x, y, z) => x + ',' + y + ',' + z;
  if (rule === 'fin') {
    // THE SHOULDER RAMP is the top 45% of the torso: the band from the chest
    // knot to the crest, which is where prof_rx is climbing toward its widest
    // and therefore the only place a marginal column can appear. Below it the
    // same ratio is the WAIST TAPER — a legitimate narrowing that the hand edit
    // left alone and so does this, because there the thin column is the
    // continuation of a thick one rather than a blade on its own.
    const bandFrom = Math.round(sz * 0.55);
    // A row's columns, by how many depth rows each is filled on.
    const byRow = new Map();
    for (const [x, y, z] of cells) {
      if (z < bandFrom) continue;
      let r = byRow.get(z);
      if (!r) byRow.set(z, r = new Map());
      r.set(x, (r.get(x) || 0) + 1);
      void y;
    }
    const drop = new Set();
    for (const [z, cols] of byRow) {
      const xs = [...cols.keys()].sort((a, b) => a - b);
      if (xs.length < 3) continue;
      const widest = Math.max(...cols.values());
      // Only the OUTERMOST column on each side is a candidate: one step in is
      // by definition supported by the column outside it.
      for (const x of [xs[0], xs[xs.length - 1]])
        if (cols.get(x) * 2 <= widest) drop.add(x + ',' + z);
    }
    if (!drop.size) return cells;
    return cells.filter(([x, y, z]) => !drop.has(x + ',' + z));
  }
  // rule === 'cap': RE-DERIVE THE CAP'S CROSS-SECTION AT THE SHIPPED LATTICE.
  //
  // That is the whole fix, and it is why this is a resolution problem and not a
  // taste one. The tube's radius grows to 2.2 at the shoulder, and on the
  // 4-wide AUTHORED lattice every radius from 1.06 up is the SAME full 4x4
  // square — so the arm ends in a flat slab, and the 2x upscale faithfully
  // turns each of its corners into a 2x2 block. The shipped lattice has the
  // resolution the authored one does not, so asking the same ellipse question
  // again here rounds the cap with no new parameter and nothing to tune.
  //
  // The reference radius is MEASURED off the rows below the cap rather than
  // taken from the builder's own r0/r1, so this knows nothing about how any
  // particular limb chose its radii and works unchanged on a thicker arm.
  const rows = Math.max(2, Math.round(sz * 0.09));   // ~1 authored row
  const from = sz - rows;
  let rx = 0, ry = 0;
  for (const [x, y, z] of cells) {
    if (z >= from) continue;
    rx = Math.max(rx, Math.abs(x + 0.5 - sx * 0.5));
    ry = Math.max(ry, Math.abs(y + 0.5 - sy * 0.5));
  }
  rx += 0.5; ry += 0.5;                      // the outer face, not the centre
  if (!(rx > 0 && ry > 0)) return cells;
  const out = [];
  const dropped = [];
  for (const c of cells) {
    const [x, y, z] = c;
    if (z < from) { out.push(c); continue; }
    const dx = (x + 0.5 - sx * 0.5) / rx;
    const dy = (y + 0.5 - sy * 0.5) / ry;
    if (dx * dx + dy * dy <= 1.0) out.push(c); else dropped.push(c);
  }
  // NEVER DISCONNECT A LIMB. This only ever removes, so the one hazard is
  // emptying a whole row and leaving what is above it floating. Cheap to rule
  // out, and the alternative is a floating scrap of flesh in the world.
  for (let z = from; z < sz; z++) {
    if (out.some(c => c[2] === z)) continue;
    for (const c of dropped) if (c[2] === z) out.push(c);
  }
  void key;
  return out;
}

/** Every voxel becomes a factor^3 block — the generator's twin of the
 *  editor's upscale2x. Shape is preserved exactly; only the sampling changes,
 *  which is what keeps an upscale from being a redraw. */
function upscaleCells(cells, factor) {
  if (factor === 1) return cells;
  const out = [];
  for (const [x, y, z, c] of cells) {
    const bx = x * factor, by = y * factor, bz = z * factor;
    for (let dz = 0; dz < factor; dz++)
      for (let dy = 0; dy < factor; dy++)
        for (let dx = 0; dx < factor; dx++)
          out.push([bx + dx, by + dy, bz + dz, c]);
  }
  return out;
}

// =============================================================================
// sidecar
// =============================================================================

const qround = v => pyRound(v, 4);
const qx = deg => { const h = radians(deg) * 0.5;
  return [qround(Math.sin(h)), 0.0, 0.0, qround(Math.cos(h))]; };
const qy = deg => { const h = radians(deg) * 0.5;
  return [0.0, qround(Math.sin(h)), 0.0, qround(Math.cos(h))]; };
const qz = deg => { const h = radians(deg) * 0.5;
  return [0.0, 0.0, qround(Math.sin(h)), qround(Math.cos(h))]; };

/** hp and severImpactSpeed per limb: extremities come off easily and the
 *  spine does not. hp is SCALED BY VOLUME below — a heavier build really is
 *  harder to take apart — but these are the numbers at the drawn figure.
 *
 *  THE ABSOLUTE SCALE IS THE SHIPPED HUMAN'S, and it is not this file's to
 *  pick. Commit 6b8623f made wound damage depth-weighted (materials.json
 *  `woundHp`) and gave the head a `brain` core that costs gore.brainHpPerVoxel
 *  FLAT per voxel destroyed, on top of the volume damage; the limb bars were
 *  tripled in the same commit to pay for it. At head 135 that reads "about
 *  five voxels of brain is fatal", which is the intended lethality. A
 *  generated character carrying the pre-6b8623f third of these numbers is not
 *  a lighter build — it is a paper doll that dies to a scratch, and it was
 *  measurably so: jujunud and newcomer shipped with head 22. */
const LIMB_BASE = {
  hips:  { hp: 180, severable: false, tag: 'spine' },
  torso: { hp: 180, severable: false, vital: true, tag: 'spine' },
  head:  { hp: 135, severable: true, vital: true, tag: 'head', sever: 20.0 },
  armU:  { hp: 48, severable: true, tag: 'arm', sever: 15.0 },
  armL:  { hp: 39, severable: true, tag: 'arm', sever: 13.0 },
  hand:  { hp: 27, severable: true, tag: 'hand', sever: 9.0 },
  legU:  { hp: 66, severable: true, tag: 'leg', sever: 18.0 },
  legL:  { hp: 54, severable: true, tag: 'leg', sever: 16.0 },
  foot:  { hp: 36, severable: true, tag: 'foot', sever: 12.0 },
};

/** The anatomy recipe, baked into the .vox by assets/editor/anatomy.js (or
 *  `node scripts/anatomize_mob.mjs <mob>`). Every generated body needs it and
 *  the peel needs a SOLID volume, which these builders produce. Head override
 *  is keyed by the limb NAME `head`. */
export function anatomyRecipe() {
  return {
    '//': 'What is under the skin, by depth from the surface, baked into the ' +
          '.vox by `node scripts/anatomize_mob.mjs <mob>` or the tuner\'s ' +
          'Apply recipe button (assets/editor/anatomy.js documents the ' +
          'format). Materials by NAME. `keep` leaves the painted surface ' +
          'alone; `garments` are surface voxels that are clothes, so the ' +
          'voxel under them is skin. Head override: a bone skull two deep ' +
          'around a `brain` core.',
    layers: [
      { material: 'skin', depth: 1, keep: true },
      { material: 'flesh', depth: 1 },
      { material: 'muscle', depth: 1, speckle: { material: 'blood', fraction: 0.06 } },
      { material: 'bone' },
    ],
    garments: ['linen'],
    limbs: {
      // THE SKULL IS THE SHIPPED HUMAN'S: skin / FLESH / bone 2 / brain. The
      // flesh ring is not decoration — it is the scalp, and dropping it (as
      // this recipe did) puts bone one voxel under the paint, so a graze that
      // should have opened a scalp rings off the skull instead and the wound
      // model's depth weights (skin/flesh 1, muscle 2, bone 3) start at 3 for
      // every head hit. The bone is two deep because the rot chews it at half
      // rate (bone.rotRate), so breaching takes time and what is behind goes
      // fast; `brain` is open-ended, so it is by definition everything the
      // shell encloses — there is no geometric brain test anywhere in the
      // engine and this recipe is the only thing that decides what is one.
      head: {
        layers: [
          { material: 'skin', depth: 1, keep: true },
          { material: 'flesh', depth: 1 },
          { material: 'bone', depth: 2 },
          { material: 'brain' },
        ],
      },
    },
  };
}

/**
 * The gait numbers that MUST agree with the body, derived from it.
 *
 * THE ARM CLIPS PLAY AT A FIXED RATE WHILE THE LEGS STEP AT A SPEED-DEPENDENT
 * ONE, so the only way they read as one motion is to author each period at the
 * step cycle for the speed that clip is selected at (avatar.cpp switches to
 * `run` past 0.80 * speed). One arm cycle spans TWO steps, and a cycle that
 * does not divide the step cycle is exactly what makes arms visibly drift in
 * and out of phase with the feet.
 *
 * The model is the runtime's, with the runtime's own constants:
 *   stance = stepThreshold * legLength / v
 *   swing  = clamp(stepDuration / speedFactor), capped at the TRAVEL BUDGET
 *            kSwingTravelFrac * kMaxLeadLegLengths * legLength / v
 * legLength is measured from the RIG, for the same reason the runtime measures
 * it: the art moves and a hand-tuned constant rots silently when it does.
 */
function gaitNumbers(table, opts) {
  const av = { ...AVATAR_CONSTANTS, ...(opts.avatar || {}) };
  const pl = { ...PLAYER_CONSTANTS, ...(opts.player || {}) };
  const L = table.limbs;
  const hipZ = L['legU.L'].mn[2] + L['legU.L'].size[2];
  const ankleZ = L['foot.L'].mn[2] + 3.0;
  const legLen = (hipZ - ankleZ) / ART_SCALE;               // world voxels
  const refSpeed = pl.sprintSpeed * SIDECAR_VOXELS_PER_METRE;

  const armCycleMs = vMps => {
    const v = vMps * SIDECAR_VOXELS_PER_METRE;
    const speedFactor = clamp(v / refSpeed, 0.0, 1.5);
    let dur = Math.max(STEP_DURATION / Math.max(speedFactor, av.minSwingScale),
                       av.minSwingSeconds);
    dur = Math.max(Math.min(dur, av.swingTravelFrac * av.maxLeadLegLengths *
                                 legLen / v), av.minSwingSeconds);
    const stance = STEP_THRESHOLD * legLen / v;
    return pyRound((dur + stance) * 2000.0) | 0;
  };

  // rideHeight is a stance trim ABOUT the authored rest pose (1.0 = stand
  // exactly as modelled), expressed in LEG LENGTHS. It exists because
  // restSoleY_ lands on the ANKLE PIVOT, not the bottom of the foot, so every
  // rig sits low by its own ankle-to-sole overhang plus whatever the foot IK's
  // plant target contributes. The residual in WORLD VOXELS is a property of
  // this foot geometry, so converting through the absolute is the only
  // transfer between figures that is actually valid.
  const REF_LEG = 5.75, REF_RIDE = 1.22;
  const rideHeight = pyRound(1.0 + (REF_RIDE - 1.0) * REF_LEG / legLen, 3);

  return {
    legLen, refSpeed, rideHeight,
    walkMs: armCycleMs(pl.walkSpeed),
    runMs: armCycleMs(pl.sprintSpeed),
  };
}

/** THE ONLY CLIPS A CHARACTER OWNS ARE THE TWO THAT ARE DERIVED FROM ITS BODY.
 *
 * `idle jump fall land hang cast limp hop crawl squirm onearm headless` used to
 * be emitted here, byte-identical into every generated sidecar, which is
 * exactly the frozen copy this generator's characters are not supposed to be.
 * They live in `assets/anims/` now — the shared library `LoadMobDefs` compiles
 * onto every rig whose part names it fits — so editing the crawl edits every
 * body's crawl, with no re-bake of anything.
 *
 * walk and run stay here because their PERIOD is derived from this figure's leg
 * (gaitNumbers, armCycleMs): a short character does not walk on the human's
 * clock. Everything else about them is shared, and a later pass can move the
 * keys to the library too and leave only `durationMs` behind (the loader
 * retimes a duration-only clip patch).
 */
function buildClips(gait) {
  const { walkMs, runMs } = gait;
  const swing = (deg, period = 900, phase = 0, ease = 'quadInOut') => {
    const a = phase === 0 ? deg : -deg, b = phase === 0 ? -deg : deg;
    const half = Math.floor(period / 2);
    return { rot: [{ t: 0, q: qx(a), ease }, { t: half, q: qx(b), ease },
                   { t: period, q: qx(a) }] };
  };
  const clips = {};


  // walk/run arm swing: additive over the gait, masked to the arms and spine
  // so the IK-driven legs are untouched. Periods derived above.
  //
  // A walking body's arm sweeps roughly HALF what its leg does — the arm is a
  // passive counterweight to the pelvis, not a second stride, and at parity
  // the figure reads as MARCHING. Kept as a trim on the leg-derived number
  // rather than folded into it, so the relationship stays legible.
  const ARM_SWING = 0.6;
  // The forearm holds a fixed CARRY ANGLE with a swing about it. Only the
  // swing is trimmed: scaling the whole track would also straighten the elbow.
  const foreMid = -18.0, foreAmp = 8.0 * ARM_SWING;
  for (const [nm, deg, period] of [['walk', 22 * ARM_SWING, walkMs],
                                   ['run', 30 * ARM_SWING, runMs]]) {
    const half = Math.floor(period / 2);
    clips[nm] = {
      durationMs: period, loop: true, mode: 'additive',
      blendInMs: 180, blendOutMs: 180,
      mask: ['armU.L', 'armL.L', 'armU.R', 'armL.R', 'torso'],
      tracks: {
        'armU.L': swing(deg, period, 0),
        'armU.R': swing(deg, period, 1),
        'armL.L': { rot: [{ t: 0, q: qx(foreMid + foreAmp), ease: 'quadInOut' },
                          { t: half, q: qx(foreMid - foreAmp), ease: 'quadInOut' },
                          { t: period, q: qx(foreMid + foreAmp) }] },
        'armL.R': { rot: [{ t: 0, q: qx(foreMid - foreAmp), ease: 'quadInOut' },
                          { t: half, q: qx(foreMid + foreAmp), ease: 'quadInOut' },
                          { t: period, q: qx(foreMid - foreAmp) }] },
        torso: { rot: [{ t: 0, q: qy(-deg * 0.16), ease: 'quadInOut' },
                       { t: half, q: qy(deg * 0.16), ease: 'quadInOut' },
                       { t: period, q: qy(-deg * 0.16) }] },
      },
    };
  }

  return clips;
}

/** FIRST MATCH WINS, so this runs most-maimed to least; predicate fields are
 *  AND-ed and an empty predicate never matches. These name LEG PARTS rather
 *  than using minChainsLost because AnimSelectState counts EVERY IK chain and
 *  this rig has arm chains too — `minChainsLost: 2` would fire "crawl" when
 *  both ARMS came off and the legs were fine.
 *
 *  The prone states carry `groundAlign` and NO bodyYOffset: the engine fits a
 *  plane through the ground under the body's own length and lowers the posed
 *  body onto it, so there is nothing here to scale between figures. */
function buildStates() {
  return [
    { name: 'squirm', missing: ['legU.L', 'legU.R'],
      missingAny: ['armU.L', 'armU.R'],
      clip: 'squirm', speedScale: 0.12, disableGait: true, groundAlign: 1.0 },
    { name: 'crawl', missing: ['legU.L', 'legU.R'],
      clip: 'crawl', speedScale: 0.3, disableGait: true, groundAlign: 1.0 },
    { name: 'crawl.shins', missing: ['legL.L', 'legL.R'],
      clip: 'crawl', speedScale: 0.3, disableGait: true, groundAlign: 1.0 },
    // BOTH FEET GONE IS A CRAWL, NOT A HOP. There used to be a `hop` state
    // here that kept the body UPRIGHT on two ankle stumps and bounced it, and
    // it looked exactly as silly as that reads (96c86d5). A body with no feet
    // has nothing to stand on and pulls itself along by its arms, so it takes
    // the crawl clip with `groundAlign` like the other prone states — a little
    // faster than a legless crawl, because the shins are still there to push
    // with. The `hop` CLIP stays in the library: nothing selects it by damage
    // now, and something else may yet want it.
    //
    // ONE foot gone is deliberately NOT here. That case belongs to the derived
    // stump-drag layer (AnimFindStumpLeg / AnimApplyStumpDrag in anim.cpp),
    // whose predicate is exactly one stump and one whole leg — an authored
    // state matching a single foot would shadow it. This rule requires BOTH,
    // so the two can never both fire. Do not widen it to `missingAny`.
    { name: 'crawl.feet', missing: ['foot.L', 'foot.R'],
      clip: 'crawl', speedScale: 0.35, disableGait: true,
      bodyYOffset: 0.0, groundAlign: 1.0 },
    { name: 'limp',
      missingAny: ['legU.L', 'legL.L', 'foot.L', 'legU.R', 'legL.R', 'foot.R'],
      clip: 'limp', speedScale: 0.55 },
    { name: 'onearm',
      missingAny: ['armU.L', 'armL.L', 'hand.L', 'armU.R', 'armL.R', 'hand.R'],
      clip: 'onearm', speedScale: 0.92 },
    { name: 'headless', missing: ['head'], clip: 'headless', speedScale: 0.7 },
  ];
}

/** Clip `pos` keys are MICRO units and the literals are authored at ART_SCALE,
 *  so they take the same multiplier the limb table got. `rot` keys are
 *  quaternions and must NOT be touched. */
function scaleClipPositions(clips, factor) {
  if (factor === 1) return clips;
  const seen = new Set();
  for (const c of Object.values(clips)) {
    if (seen.has(c)) continue;          // two names may share one clip object
    seen.add(c);
    for (const tr of Object.values(c.tracks || {}))
      for (const k of tr.pos || []) k.v = k.v.map(v => pyRound(v * factor, 4));
  }
  return clips;
}

// =============================================================================
// generateMob
// =============================================================================

/**
 * Build a character.
 *
 * @param {object} genome           see defaultGenome()
 * @param {number} seed             reserved for per-instance jitter; the body
 *                                  is a pure function of the genome today, and
 *                                  the argument is here so a future freckle
 *                                  pass does not change every call site.
 * @param {object} opts
 *   materials  the engine material list in ID order (assets/materials/
 *              materials.json .materials) — needed for the .vox palette and to
 *              resolve FLESH_ID by name. Required to write a file; omit for a
 *              geometry-only preview.
 *   player     { sprintSpeed, walkSpeed, halfHeight, eyeOffset } from
 *              tuning.json. Defaults to PLAYER_CONSTANTS.
 *   avatar     the four gait constants from avatar.cpp. Defaults to
 *              AVATAR_CONSTANTS.
 *   name       asset stem; defaults to genome.name.
 * @returns {{name, genome, table, parts, prefab, sidecar, vox, stats}}
 *   `vox` is a Uint8Array only when `materials` was supplied.
 */
export function generateMob(genome, seed = 0, opts = {}) {
  const g = normalizeGenome(genome);
  const name = opts.name || g.name || 'untitled';
  const table = limbTable(g);
  const L = table.limbs;
  void seed;

  // ---- the height contract, asserted --------------------------------------
  const bodyNames = ARCHETYPE.order.filter(n => !ARCHETYPE.props.includes(n));
  const top = Math.max(...bodyNames.map(n => L[n].mn[2] + L[n].size[2]));
  const bottom = Math.min(...bodyNames.map(n => L[n].mn[2]));
  if (bottom !== 0)
    throw new Error(`figure does not stand on z=0 (lowest part at ${bottom})`);
  if (top !== table.microH)
    throw new Error(`figure is ${top} art micro (${top / ART_SCALE} world ` +
                    `voxels) tall, expected ${table.microH} (${table.worldH})`);

  const fleshId = opts.materials
    ? opts.materials.findIndex(m => m.id === FLESH_ID) + 1
    : 51;
  if (opts.materials && fleshId <= 0)
    throw new Error(`materials.json has no "${FLESH_ID}" material — the ` +
                    `palette index every voxel of this figure carries has ` +
                    `moved, and the whole body would load as another material`);
  if (fleshId > 127)
    throw new Error(`"${FLESH_ID}" is material ${fleshId}; mob voxel material ` +
                    `ids must stay <= 127 (128..255 is the art palette)`);

  // ---- geometry: each limb emitted TWICE ----------------------------------
  // "<name>" carries FLESH in every cell; "<name>.col" carries the art slot
  // over the SAME cells, at the SAME size and the SAME translation. The loader
  // matches the two by ABSOLUTE cell, so identical placement is not a
  // convenience here, it is the join condition.
  const parts = [];
  let painted = 0;
  // Voxels the shoulder round took off, reported in stats so the Characters
  // page can say the sculpting ran rather than leaving you to squint for it.
  let rounded = 0;
  for (const nm of ARCHETYPE.order) {
    const artSize = L[nm].size;
    const size = artSize.map(v => v * SKIN_UPSCALE);
    const mn = L[nm].mn.map(v => v * SKIN_UPSCALE);
    // The int8 bound is about the DERIVED COLLIDER, not this lattice: the
    // engine picks physScale as the finest of {8,4,2,1} whose limb extents fit
    // +-120, so what must hold is that SOME collider resolution fits.
    const worldExtent = Math.max(...size) / SCALE;
    if (worldExtent > 120)
      throw new Error(`${nm} is ${worldExtent} world voxels; even a 1:1 ` +
                      `collider exceeds the DebrisVoxel int8 bound`);
    let cells = SHAPES[nm](g, artSize, { ...table, limbs: L, name: nm });
    if (!cells.length) throw new Error(`${nm} generated no voxels`);
    cells = upscaleCells(cells, SKIN_UPSCALE);
    // THE SHOULDER ROUND, after the upscale because that is the only lattice
    // fine enough to round on (see roundShoulders). `roundShoulders: false`
    // gives gen_human.py's geometry exactly, which is what the port pin in
    // scripts/test_mobgen.mjs builds with — the pin proves the TRANSLATION, and
    // this is a deliberate improvement on top of it rather than part of it.
    if (opts.roundShoulders !== false) {
      const before = cells.length;
      if (ARCHETYPE.shoulders.fin.includes(nm))
        cells = roundShoulders(cells, size, 'fin');
      else if (ARCHETYPE.shoulders.cap.includes(nm))
        cells = roundShoulders(cells, size, 'cap');
      rounded += before - cells.length;
    }
    const seen = new Set();
    const uniq = [];
    for (const [x, y, z, c] of cells) {
      if (!(x >= 0 && x < size[0] && y >= 0 && y < size[1] &&
            z >= 0 && z < size[2]))
        throw new Error(`${nm} voxel (${x},${y},${z}) outside declared size ` +
                        `${size}`);
      const k = x + ',' + y + ',' + z;
      if (seen.has(k)) continue;                       // first write wins
      seen.add(k);
      if (!(c >= 128 && c <= 255))
        throw new Error(`${nm} cell (${x},${y},${z}) has colour ${c}, which ` +
                        `is not an art palette slot — a builder returned a ` +
                        `material by mistake`);
      uniq.push([x, y, z, c]);
    }
    painted += uniq.length;
    parts.push({ name: nm, size, mn, cells: uniq });
  }

  // ---- .vox ---------------------------------------------------------------
  let vox = null;
  if (opts.materials) {
    const models = [];
    for (const p of parts) {
      const size = { x: p.size[0], y: p.size[1], z: p.size[2] };
      // Translation stores the box CENTRE (vox.js sceneChunks documents the
      // layout); pivot is the integer half-size, exactly as MagicaVoxel writes.
      const t = { x: p.mn[0] + (p.size[0] >> 1), y: p.mn[1] + (p.size[1] >> 1),
                  z: p.mn[2] + (p.size[2] >> 1) };
      models.push({ name: p.name, size, t,
                    voxels: p.cells.map(([x, y, z]) => ({ x, y, z, c: fleshId })) });
      models.push({ name: p.name + '.col', size, t,
                    voxels: p.cells.map(([x, y, z, c]) => ({ x, y, z, c })) });
    }
    vox = writeVox(models, paletteBytes(opts.materials, g.colors), { scene: true });
  }

  const sidecar = buildSidecar(g, table, opts, name);
  return {
    name, genome: g, table, parts, vox, sidecar,
    stats: { painted, rounded, worldH: table.worldH, microH: table.microH,
             eyeZ: table.eyeZ, parts: parts.length },
  };
}

/**
 * The 256-entry palette. ENTRY i IS PALETTE INDEX i+1 — index 0 is "empty" and
 * has no entry, and getting that off by one shifts every colour by one slot,
 * which looks almost right. voxload.cpp reads it with the same bias.
 *
 * The low end is filled with the material colours so the file opens sensibly
 * in MagicaVoxel and the editor; the ENGINE ignores it there (material colours
 * come from materials.json). Only the art range is read back, and only for
 * models that have a ".col" layer.
 *
 * Unused art slots are left at 0 deliberately: MicroBodyMergeArt walks all 128
 * slots and dedupes by RGB, so leaving them black costs exactly ONE merged
 * slot for the whole run rather than one each.
 */
/**
 * Bake the sidecar's `anatomy` recipe into a built body's `.vox`.
 *
 * WHY THIS IS HERE AND NOT ONLY IN scripts/anatomize_mob.mjs. A body straight
 * out of `generateMob` is solid `skin` all the way through: no flesh, no
 * muscle, no blood, no bone, no brain. It walks and it burns, but a sword
 * through it finds skin at every depth, the wound materials the gore system is
 * authored against are absent, and the head has no skull. So an un-baked body
 * is not a finished character, and a save that leaves the bake to a command
 * line leaves the tuner producing unfinished assets — which defeats the point
 * of the page.
 *
 * `anatomy.js` is a pure module (the Models tab's Apply-recipe button already
 * runs it in the browser), so there is nothing CLI-only about the step: it runs
 * here for the Characters page, for `scripts/gen_mobs.mjs` and for the gate,
 * out of one implementation. `scripts/anatomize_mob.mjs` stays as the way to
 * (re-)bake a mob that is ALREADY on disk, including hand-authored ones.
 *
 * SEPARATE FROM generateMob ON PURPOSE. The port pin (`test_mobgen.mjs`) is
 * against `gen_human.py`, which has no anatomy pass, so the generator's own
 * output has to stay the un-baked figure. Baking is a second, named step.
 *
 * @param built  the object generateMob returned; `vox` is REPLACED in place
 * @param materials the engine material list in ID order
 * @returns {{total:number, census:object, perLimb:object}} what it rewrote
 */
export function bakeAnatomy(built, materials) {
  if (!built || !built.vox)
    throw new Error('bakeAnatomy: nothing built (generateMob needs `materials`)');
  const recipe = built.sidecar.anatomy;
  if (!recipe) throw new Error('bakeAnatomy: the sidecar has no `anatomy` block');
  const parsed = readVox(built.vox);
  if (!parsed.prefab) throw new Error('bakeAnatomy: no models in the .vox');
  const matId = ANA.materialIds(materials);
  const plan = ANA.planAnatomy(parsed.prefab, recipe, matId);
  if (plan.report.unresolved.length)
    throw new Error('the anatomy recipe names materials that do not exist: ' +
                    plan.report.unresolved.join(', '));
  ANA.applyPlan(parsed.prefab, plan);

  // THE TIGHT REBASE MUST BE A NO-OP. Only MATERIALS change in a bake, so if
  // re-tightening moves content or drops a model then the input was not in the
  // engine's canonical form and every anchor in the sidecar would drift against
  // the art. Refusing is the same check scripts/anatomize_mob.mjs makes, and it
  // is the one that stops a bake from silently becoming a limb move.
  const { prefab: tight, shift } = tightenPrefab(parsed.prefab);
  if (shift.x || shift.y || shift.z ||
      tight.models.length !== parsed.prefab.models.length)
    throw new Error(`refusing to bake: the tight rebase moved content by ` +
      `${shift.x},${shift.y},${shift.z} (or dropped a model) — the sidecar ` +
      `anchors would drift`);
  const rt = prefabRoundTripTest(tight, parsed.palette);
  if (!rt.ok) throw new Error('anatomy round-trip failed: ' + rt.error);
  built.vox = writeVox(prefabToVoxModels(tight), parsed.palette, { scene: true });

  const name = id => (materials[id - 1] && materials[id - 1].id) || ('#' + id);
  const census = {};
  for (const m of tight.models)
    for (const v of m.grid.data) if (v) census[name(v)] = (census[name(v)] || 0) + 1;
  built.anatomyBaked = { total: plan.report.total, census,
                         perLimb: plan.report.perLimb };
  return built.anatomyBaked;
}

export function paletteBytes(materials, colors) {
  const pal = new Uint8Array(1024);
  const put = (idx, hex) => {
    const n = parseInt(String(hex).replace('#', ''), 16);
    const o = (idx - 1) * 4;
    pal[o] = (n >> 16) & 255; pal[o + 1] = (n >> 8) & 255;
    pal[o + 2] = n & 255; pal[o + 3] = 255;
  };
  (materials || []).forEach((m, i) => {
    const hex = m && (m.colors ? m.colors[0] : m.color0);
    if (hex) put(i + 1, hex);
  });
  for (const [k, slot] of Object.entries(COLOR_SLOTS))
    if (colors[k]) put(slot, colors[k]);
  return pal;
}

function buildSidecar(g, table, opts, name) {
  const L = table.limbs;
  const U = SKIN_UPSCALE;
  // SHIPPED (upscaled) boxes. Anchors, sockets and natural-weapon edges are
  // all in these units; NOTHING should read them to recover an authored
  // number — that is what gen_mina.py did and why her face was never carved.
  const S = {};
  for (const [n, v] of Object.entries(L))
    S[n] = { size: v.size.map(x => x * U), mn: v.mn.map(x => x * U) };

  // Prefab extents in scene space. PROPS ARE EXCLUDED and that MIRRORS THE
  // LOADER (mob.cpp skips tag == "prop" when it measures worldSize): anchors
  // are rebased against this origin, so measuring a box the engine does not
  // shifts every anchor and moves the gait pivot off the body.
  const bodyNames = Object.keys(S).filter(n => !ARCHETYPE.props.includes(n));
  const minX = Math.min(...bodyNames.map(n => S[n].mn[0]));
  const maxY = Math.max(...bodyNames.map(n => S[n].mn[1] + S[n].size[1]));
  /** Scene (Z-up) -> prefab-local engine coords (Y-up, min corner 0), matching
   *  voxload.cpp: engine = (x, z, -y), rebased so the min corner is 0. */
  const anchor = ([x, y, z]) =>
    [pyRound(x - minX, 1), pyRound(z, 1), pyRound(maxY - y, 1)];

  // A joint sits at the SEAM between a part and its parent, so restating its
  // height as a literal is a second source of truth that rots the moment a
  // limb is resized. These take offsets in AUTHORED micro and scale here.
  const jointTop = (part, { dx = 0, dy = 0, inset = 0 } = {}) =>
    anchor([S[part].mn[0] + S[part].size[0] * 0.5 + dx * U,
            S[part].mn[1] + S[part].size[1] * 0.5 + dy * U,
            S[part].mn[2] + S[part].size[2] - inset * U]);
  const jointBottom = (part, { dx = 0, dy = 0, rise = 0 } = {}) =>
    anchor([S[part].mn[0] + S[part].size[0] * 0.5 + dx * U,
            S[part].mn[1] + S[part].size[1] * 0.5 + dy * U,
            S[part].mn[2] + rise * U]);
  const jointAt = (part, z, { dx = 0, dy = 0 } = {}) =>
    anchor([S[part].mn[0] + S[part].size[0] * 0.5 + dx * U,
            S[part].mn[1] + S[part].size[1] * 0.5 + dy * U, z]);

  // hp SCALES WITH THE LIMB'S VOLUME against the drawn figure. It is derived
  // and not a gene for the reason in the banner: a bigger arm that is no
  // harder to cut off is the kind of wrong you only notice in combat, long
  // after the thumbnail said the character was fine.
  const hpOf = (nm, base) => {
    const d = DEF[nm];
    const vol = L[nm].size[0] * L[nm].size[1] * L[nm].size[2];
    const dvol = d.size[0] * d.size[1] * d.size[2];
    return Math.max(1, pyRound(base * vol / dvol) | 0);
  };
  const baseOf = nm => LIMB_BASE[nm.split('.')[0]] || LIMB_BASE[nm];

  const hipsTop = S.hips.mn[2] + S.hips.size[2];
  const limbs = [
    { name: 'hips', hp: hpOf('hips', LIMB_BASE.hips.hp), severable: false,
      tag: 'spine' },
    { name: 'torso', parent: 'hips', joint: 'ball',
      hp: hpOf('torso', LIMB_BASE.torso.hp), severable: false, vital: true,
      tag: 'spine', anchor: jointAt('hips', hipsTop - 2 * U) },
    // neck: the top of the torso. This head model's origin IS its base (the
    // neck stub is the bottom rows), so this is the seam.
    { name: 'head', parent: 'torso', joint: 'ball',
      hp: hpOf('head', LIMB_BASE.head.hp), severable: true, vital: true,
      tag: 'head', anchor: jointTop('torso', { inset: 1 }),
      severImpactSpeed: LIMB_BASE.head.sever,
      // `gain` is read against velocity NORMALIZED by `speed` (kSpringVelScale
      // in game/anim.h), so it means "radians of lag at full sprint" — a small
      // number, or the head sits pegged at maxAngle every step. maxAngle is a
      // safety rail for impacts, not the working range.
      spring: { halflife: 0.12, gain: 0.18, maxAngle: 0.3 } },
  ];
  for (const side of ARCHETYPE.sides) {
    // Which way is "across the chest" for THIS arm. Model +X is the
    // character's LEFT on these rigs, so the left arm reaches toward -X.
    const inward = side === 'L' ? [-1, 0, 0] : [1, 0, 0];
    limbs.push({
      name: `armU.${side}`, parent: 'torso', joint: 'ball',
      hp: hpOf(`armU.${side}`, LIMB_BASE.armU.hp), severable: true, tag: 'arm',
      anchor: jointTop(`armU.${side}`),                      // shoulder
      // THE SHOULDER, and why the one-axis form used by the hip and knee is
      // not enough for it: that form bounds one component and leaves the other
      // two free, which is fine for a joint the solver only drives in one
      // plane. The weapon arm is driven by the MOUSE through the same IK chain
      // at any direction in the sphere, and would happily rake the arm back or
      // fold it through the ribs with a legal X component the whole time.
      //
      //   - no more than 50 deg behind the frontal plane. Measured: the run
      //     clip's arm swing reaches -42, so anything under ~45 puts an
      //     ordinary sprint on its stop every stride.
      //   - no more than 30 deg past the midline. Enough for a cross-body cut;
      //     past that the upper arm is inside the ribcage.
      // Both are half-spaces on where the bone POINTS, so they hold at every
      // elevation — the failure a flexion-only limit cannot see.
      //
      // The twist bound is not decoration: the shoulder's roll aims the
      // ELBOW's hinge plane, so leaving it free lets a correctly-hinged
      // forearm swing medially into the torso.
      poseLimit: { bone: [0, -1, 0],
                   reach: [{ normal: [0, 0, -1], max: 50 },
                           { normal: inward, max: 30 }],
                   twist: { min: -75, max: 75 } },
      severImpactSpeed: LIMB_BASE.armU.sever });
    limbs.push({
      name: `armL.${side}`, parent: `armU.${side}`, joint: 'hinge',
      hp: hpOf(`armL.${side}`, LIMB_BASE.armL.hp), severable: true, tag: 'arm',
      axis: [1, 0, 0], minAngle: -2.4, maxAngle: 0.05,
      anchor: jointTop(`armL.${side}`),                      // elbow
      // AN ELBOW IS A HINGE, and `hinge: true` is what makes that true of the
      // POSE and not just the ragdoll: it discards the off-axis swing instead
      // of merely bounding the on-axis part. The two-bone IK picks its bend
      // axis from the chain's pole, which coincides with the forearm's hinge
      // axis under pure flexion and pure abduction but not at a blend — and at
      // a blend it opens the elbow sideways, which is most of what "the arm
      // clips through the character" looks like.
      poseLimit: { axis: [-1, 0, 0], min: 0, max: 130, hinge: true },
      severImpactSpeed: LIMB_BASE.armL.sever });
    limbs.push({
      name: `hand.${side}`, parent: `armL.${side}`, joint: 'ball',
      hp: hpOf(`hand.${side}`, LIMB_BASE.hand.hp), severable: true,
      tag: 'hand', anchor: jointTop(`hand.${side}`),         // wrist
      severImpactSpeed: LIMB_BASE.hand.sever });
  }
  for (const side of ARCHETYPE.sides) {
    limbs.push({
      name: `legU.${side}`, parent: 'hips', joint: 'ball',
      hp: hpOf(`legU.${side}`, LIMB_BASE.legU.hp), severable: true, tag: 'leg',
      anchor: jointTop(`legU.${side}`),                      // hip
      // RANGE OF MOTION FOR THE ANIMATION, which minAngle/maxAngle do NOT
      // provide: those are Jolt constraints and Jolt only enforces them on a
      // dynamic body, while a live limb is kinematic and re-posed every tick.
      //
      // The axis is NEGATED X so positive reads FORWARD, the way a human would
      // describe a hip: a positive rotation about model +X swings a hanging
      // limb BACKWARD. MEASURED: a healthy walk uses -44..+16 and the ramp
      // fixture reaches -61..+16, so 20 back / 85 forward leaves honest margin
      // and still makes "raked out behind" unrepresentable.
      poseLimit: { axis: [-1, 0, 0], min: -20, max: 85 },
      severImpactSpeed: LIMB_BASE.legU.sever });
    limbs.push({
      name: `legL.${side}`, parent: `legU.${side}`, joint: 'hinge',
      hp: hpOf(`legL.${side}`, LIMB_BASE.legL.hp), severable: true, tag: 'leg',
      axis: [1, 0, 0], minAngle: -2.4, maxAngle: 0.05,
      anchor: jointTop(`legL.${side}`),                      // knee
      // A knee flexes one way and does not hyperextend, so `min: 0` is exactly
      // "never straighter than it was drawn". Measured at 82 on the flat and
      // 88 climbing; 90 sat on top of that and the knee spent whole strides
      // pinned to it, hence 100.
      poseLimit: { axis: [1, 0, 0], min: 0, max: 100 },
      severImpactSpeed: LIMB_BASE.legL.sever });
    limbs.push({
      name: `foot.${side}`, parent: `legL.${side}`, joint: 'hinge',
      hp: hpOf(`foot.${side}`, LIMB_BASE.foot.hp), severable: true,
      tag: 'foot', axis: [1, 0, 0], minAngle: -0.6, maxAngle: 0.6,
      // ankle: above the sole, at the BACK of the foot (the toes reach forward
      // from here) so it pivots the way an ankle does
      anchor: jointBottom(`foot.${side}`, { dy: -1.5, rise: 3.0 }),
      severImpactSpeed: LIMB_BASE.foot.sever });
  }

  // ---- sockets ------------------------------------------------------------
  // A socket is named for the CONTEXT an item asks to be held in, not for the
  // limb it rides; `part` is which limb carries that context on THIS rig, so a
  // left-handed creature moves one line and every item still hangs correctly.
  // No rotation: the socket frame IS the hand's frame, and an item's own
  // orientation is its business (its `grip` block).
  const sockets = [{
    name: 'held_right', part: 'hand.R',
    offset: anchor([S['hand.R'].mn[0] + S['hand.R'].size[0] * 0.5,
                    S['hand.R'].mn[1] + S['hand.R'].size[1] * 0.5,
                    S['hand.R'].mn[2] + S['hand.R'].size[2] * 0.5]),
    rotation: [0, 0, 0],
  }];

  // ---- natural weapons: the parts that ARE weapons ------------------------
  // A fist and a set of jaws are weapons nobody handed the creature: an
  // authored edge segment and a StrikeProfile, exactly as an item has, with
  // the segment on a part of the rig instead of a borrowed slot.
  //
  // THE FRAME IS THE PART'S OWN, origin at its model min corner, Y-up, in the
  // same skin units as the anchors. Authored as FRACTIONS of the part's box so
  // the numbers survive re-proportioning the figure — which is exactly what
  // this generator does to them.
  const partLocal = (part, fx, fy, fz) =>
    [pyRound(fx * S[part].size[0], 1), pyRound(fy * S[part].size[2], 1),
     pyRound(fz * S[part].size[1], 1)];
  // halfWidths are carve RADII in skin units, so they scale with the part.
  const wr = L['hand.R'].size[0] / DEF['hand.R'].size[0];
  const natural = [];
  for (const side of ARCHETYPE.sides) {
    // THE KNUCKLES, and the segment SPANS THE HAND rather than sticking out
    // of it: straight down the middle from just under the wrist to just under
    // the fist. It used to end at 1.05 of the box depth so the tip stood a
    // hair proud of the knuckles, which mattered while the probe rays were
    // cast along the EDGE's own axis. They are not any more — a fist is
    // `EdgeSweep::selfMounted` and probes along its TRAVEL, because a natural
    // weapon is not wrist-steered and the axis it does have runs back up the
    // arm swinging it (d4ecbfa). With the direction coming from elsewhere, the
    // only job left for these two points is to cover the striking part.
    natural.push({
      name: `fist.${side}`, part: `hand.${side}`,
      edge: { from: partLocal(`hand.${side}`, 0.50, 0.95, 0.50),
              to: partLocal(`hand.${side}`, 0.50, 0.05, 0.50),
              halfWidth: pyRound(4.0 * wr, 2) },
      // MOSTLY TRAUMA. `bluntCarve` 0 is "bruise, never dent" (impact.h), and
      // that was the fist's profile until 96c86d5: a bare punch left a mark on
      // the health bar and never a mark in the SURFACE, so a fistfight could
      // not scrape a knuckle or split a lip. 0.3 is a token dent — an iron
      // gauntlet worn over this same fist REPLACES the profile with a real
      // one, which is why the large number still lives on the item.
      strike: { blunt: 4.0, bluntCarve: 0.3 },
    });
  }
  // THE FRONT OF THE FACE, from inside the skull level with the mouth, out
  // PAST the front face so the teeth lead. The probe rays tile the segment and
  // reach about their own half-width beyond the tip, so a bite's REACH is this
  // number — jaws that stopped at the skin of the face could not touch a
  // victim the biter's own chest was already pressed against. The CARVE stays
  // bounded by gore.biteRadius, so this lengthens the reach and not the wound.
  const hr = L.head.size[0] / DEF.head.size[0];
  natural.push({
    name: 'jaws', part: 'head',
    // halfWidth is HOW WIDE A MOUTH IS, and it sizes the BITE rather than the
    // teeth: the sweep probes a capsule of this radius about the segment, so
    // it is the one number that says how close the jaws have to come to count
    // as closing on something. 12 (1.5 world voxels) was narrower than the
    // head it sits on and made a bite a needle threaded at arm's length —
    // against a moving leg, most swings that looked like contact were not. 19
    // is 2.4 voxels, about the width of the skull, which is the honest answer
    // to "what did the mouth cover", and it is what the shipped human carries.
    edge: { from: partLocal('head', 0.50, 0.38, 0.40),
            to: partLocal('head', 0.50, 0.28, 1.70),
            halfWidth: pyRound(19.0 * hr, 2) },
    // A BITE IS MOSTLY A TEAR. The small blunt part is the head-butt a set of
    // jaws arrives attached to. What the tear CARRIES — rot, ichor — is the
    // creature's (`bite` block), not the weapon's: a zombie inherits these
    // jaws untouched and infects with them.
    strike: { blunt: 1.5, bite: 7.0 },
  });

  // ---- IK chains ----------------------------------------------------------
  const chains = [];
  for (const side of ARCHETYPE.sides)
    chains.push({ tag: 'leg',
                  parts: [`legU.${side}`, `legL.${side}`, `foot.${side}`],
                  effector: `foot.${side}`, pole: [0, 0, 1], solver: 'twobone' });
  for (const side of ARCHETYPE.sides)
    chains.push({ tag: 'arm',
                  parts: [`armU.${side}`, `armL.${side}`, `hand.${side}`],
                  effector: `hand.${side}`,
                  // elbows point BACK, so the pole is behind the figure
                  pole: [0, 0, -1], solver: 'twobone' });

  const gait = gaitNumbers(table, opts);
  const clips = scaleClipPositions(buildClips(gait), SKIN_UPSCALE);

  return {
    root: ARCHETYPE.root,
    // `artVoxelsPerMetre` is the authored fact; the loader DERIVES skinScale
    // from it and the live kVoxelsPerMetre. Authoring skinScale directly
    // encoded "and the world is 10 cm" into every sidecar, which is how the
    // human halved in metres the moment kVoxelMeters did.
    artVoxelsPerMetre: ART_VOXELS_PER_METRE,
    // The scale the WORLD-space rows below (speed, severImpactSpeed,
    // rideHeight, bodyYOffset, clip pos) are written at. Stated rather than
    // defaulted: a modded asset silently assumed to be 10 vox/m is the exact
    // failure mob.cpp's legacy path warns about.
    sidecarVoxelsPerMetre: SIDECAR_VOXELS_PER_METRE,
    bleed: { material: 'blood', perDamage: 2.5 },
    anatomy: anatomyRecipe(),
    speed: pyRound(gait.refSpeed, 4),
    gait: {
      // Biped: two SINGLETON groups, so only one foot may swing at a time.
      // That single constraint is the whole gait state machine.
      groups: [['legU.L'], ['legU.R']],
      // stepDuration and stepThreshold are the two the arm-cycle derivation
      // READS — changing one without the other is what puts the arms out of
      // phase with the feet.
      cadence: 8.0, strideBias: 0.42, leadTime: 0.10,
      stepThreshold: STEP_THRESHOLD, stepDuration: STEP_DURATION,
      // A LOW ARC, because this rig stands with near-straight legs: the knee
      // has to fold hard for even a small lift, and a taller arc put the
      // mid-swing knee on its limit through the whole swing. 0.08 leg lengths
      // is about 5 cm on a 1.7 m body, which is what a walking foot clears.
      stepHeight: 0.08,
      rideHeight: gait.rideHeight,
      bobAmp: 0.045, bobFreqMul: 2.0, swayAmp: 0.03,
      rollAmp: 0.06, spineCounter: 0.75, phaseLag: 0.05,
    },
    limbs, sockets, natural, chains,
    states: buildStates(),
    clips,
    flipbooks: {},
    // What rolled this body. Not read by the engine; it is what lets the
    // Characters page load a saved character back into its sliders and breed
    // from it, which is the difference between a pool and a folder of files.
    genome: { ...g, name, displayName: opts.displayName || g.displayName },
  };
}

// =============================================================================
// presets — starting points, not a catalogue
// =============================================================================

export const PRESETS = {
  human: {},
  lanky: {
    body: { heightM: 1.82, shoulderWidth: 10, hipWidth: 8, bodyDepth: 6,
            stack: { shin: 15, thigh: 16, torso: 17, head: 15 },
            armStack: { upper: 12, fore: 12, hand: 5 } },
    shape: { waist: 0.85, chest: 0.88, shoulder: 0.92, armGirth: 0.85,
             legGirth: 0.85 },
    head: { jaw: 0.85, cheek: 0.9 },
    hair: { style: 'cropped', ...HAIR_STYLES.cropped },
  },
  stocky: {
    body: { heightM: 1.58, shoulderWidth: 14, hipWidth: 12, bodyDepth: 10,
            limbWidth: 6, stack: { shin: 11, thigh: 12, torso: 19, head: 16 } },
    shape: { waist: 1.15, chest: 1.12, shoulder: 1.1, chestDepth: 1.15,
             seat: 1.1, armGirth: 1.1, legGirth: 1.1 },
    head: { jaw: 1.2, cheek: 1.1 },
    hair: { style: 'swept', ...HAIR_STYLES.swept },
    colors: { ...COMPLEXIONS.tan, ...HAIR_COLORS.black },
  },
  waif: {
    body: { heightM: 1.60, shoulderWidth: 10, hipWidth: 10, bodyDepth: 6,
            headWidth: 10, stack: { shin: 13, thigh: 14, torso: 17, head: 17 } },
    shape: { waist: 0.82, chest: 0.85, shoulder: 0.85, chestDepth: 0.85,
             armGirth: 0.82, legGirth: 0.85 },
    head: { jaw: 0.82, cheek: 0.95, crown: 1.05 },
    face: { eyeCols: 2 },
    hair: { style: 'long', ...HAIR_STYLES.long },
    colors: { ...COMPLEXIONS.pale, ...HAIR_COLORS.sandy, eye: EYE_COLORS.green },
  },
  elder: {
    body: { heightM: 1.64, shoulderWidth: 12, hipWidth: 10, bodyDepth: 8,
            stack: { shin: 12, thigh: 13, torso: 18, head: 17 } },
    shape: { waist: 1.08, chest: 0.95, shoulder: 0.92, seat: 1.05,
             armGirth: 0.9 },
    head: { jaw: 0.95, cheek: 0.92, crown: 0.95 },
    hair: { style: 'fringe', ...HAIR_STYLES.fringe },
    colors: { ...COMPLEXIONS.fair, ...HAIR_COLORS.grey, ...CLOTH_COLORS.umber },
  },
  brute: {
    body: { heightM: 1.85, shoulderWidth: 16, hipWidth: 12, bodyDepth: 10,
            limbWidth: 6, headWidth: 12, headDepth: 12,
            stack: { shin: 13, thigh: 14, torso: 20, head: 15 },
            armStack: { upper: 12, fore: 11, hand: 6 } },
    shape: { waist: 1.1, chest: 1.2, shoulder: 1.18, chestDepth: 1.15,
             armGirth: 1.2, legGirth: 1.15 },
    head: { jaw: 1.3, cheek: 1.12, crown: 0.95 },
    hair: { style: 'topknot', ...HAIR_STYLES.topknot },
    colors: { ...COMPLEXIONS.brown, ...HAIR_COLORS.black,
              ...CLOTH_COLORS.madder },
  },
};
export const PRESET_ORDER = ['human', 'lanky', 'stocky', 'waif', 'elder', 'brute'];

/** A preset as a full genome. Presets are PARTIAL on purpose: a preset that
 *  restated every gene would silently freeze at whatever the defaults were the
 *  day it was written. */
export function presetGenome(key) {
  const p = PRESETS[key];
  if (!p) return defaultGenome();
  const g = defaultGenome();
  const merge = (dst, src) => {
    for (const [k, v] of Object.entries(src || {})) {
      if (v && typeof v === 'object' && !Array.isArray(v)) merge(dst[k] ??= {}, v);
      else dst[k] = v;
    }
  };
  merge(g, p);
  g.name = key;
  g.displayName = key[0].toUpperCase() + key.slice(1);
  return normalizeGenome(g);
}

/**
 * The structural asserts every rolled body must pass, as a list of complaints.
 * Empty means the body is sound. Used by the gate over random genomes and by
 * the Characters page to grey out a roll it should not let you save.
 *
 * These are exactly the failures a THUMBNAIL CANNOT SHOW: an anchor outside
 * the limb it drives, an eye row off the face, a leg the gait cannot reach
 * with. The ugly ones are the human's job.
 */
export function validateMob(built) {
  const out = [];
  const { table, sidecar, parts } = built;
  const S = {};
  for (const p of parts) S[p.name] = p;
  const minX = Math.min(...parts.map(p => p.mn[0]));
  const maxY = Math.max(...parts.map(p => p.mn[1] + p.size[1]));
  for (const lm of sidecar.limbs) {
    if (!lm.anchor) continue;
    const p = S[lm.name];
    // The anchor is in prefab-local engine coords; the limb's own box in the
    // same frame is (x-minX, z, maxY-y-dim). A joint on the SEAM may sit
    // exactly on a face, so the test is a closed interval with one cell of
    // slack for the ankle's authored rise.
    const lo = [p.mn[0] - minX, p.mn[2], maxY - (p.mn[1] + p.size[1])];
    const hi = [lo[0] + p.size[0], lo[1] + p.size[2], lo[2] + p.size[1]];
    for (let i = 0; i < 3; i++)
      if (lm.anchor[i] < lo[i] - 1 || lm.anchor[i] > hi[i] + 1)
        out.push(`${lm.name}: anchor ${lm.anchor} is outside its own box ` +
                 `${lo}..${hi} on axis ${'xyz'[i]}`);
  }
  const head = S.head;
  const eyeLocal = table.eyeZ * SKIN_UPSCALE - head.mn[2];
  if (eyeLocal < 0 || eyeLocal >= head.size[2])
    out.push(`eye row ${eyeLocal} is outside the head model`);
  // No floating parts: every limb box must touch its parent's.
  const parent = new Map(sidecar.limbs.map(l => [l.name, l.parent]));
  for (const [n, par] of parent) {
    if (!par) continue;
    const a = S[n], b = S[par];
    const gap = (i) => Math.max(a.mn[i] - (b.mn[i] + b.size[i]),
                                b.mn[i] - (a.mn[i] + a.size[i]));
    if (gap(0) > 0 || gap(1) > 0 || gap(2) > 0)
      out.push(`${n} does not touch its parent ${par}`);
  }
  if (table.worldH < 8 || table.worldH > 40)
    out.push(`figure is ${table.worldH} world voxels tall`);
  return out;
}
