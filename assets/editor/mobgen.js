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
 * THE STANDARD IS assets/mobs/human.json. The port was pinned cell for cell
 * against gen_human.py's own output until 2026-09-24, which is why every
 * default below is the exact literal the Python carried rather than a tidier
 * number nearby. That pin and the Python are DELETED (rule-unification W1-E):
 * the Python had stopped reproducing the shipped human, so the pin held this
 * file to a stale rig. `scripts/test_mobgen.mjs` §L now holds the default
 * genome's rig contract to human.json and §M every bred character to its
 * genome; `scripts/generator_parity.mjs` runs it after every edit here.
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
import * as SC from './sidecar.js';
import * as SY from './sylvan.js';

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
  /** What KIND of surface each part is, for a race that dresses the surface
   *  (sylvan.js barkPass): the trunk, the head, a limb, a hand, a foot. */
  roles: { hips: 'trunk', torso: 'trunk', head: 'head',
           'armU.L': 'limb', 'armL.L': 'limb', 'hand.L': 'hand',
           'armU.R': 'limb', 'armL.R': 'limb', 'hand.R': 'hand',
           'legU.L': 'limb', 'legL.L': 'limb', 'foot.L': 'foot',
           'legU.R': 'limb', 'legL.R': 'limb', 'foot.R': 'foot' },
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
  // THE WISPS: the hair colour at three coverages, for the thinning ends of
  // hanging hair. Not genome colours -- they are DERIVED from `hair` at
  // palette-write time (wispColors) and carry an ALPHA, which microbody.wgsl
  // draws as screen-door coverage. Heaviest first.
  HAIR_WISP1: 246,
  HAIR_WISP2: 245,
  HAIR_WISP3: 244,
  // THE SYLVAN'S OWN SLOTS (sylvan.js): roots, moss, branches, flowers. Never
  // painted on a human and never written into a human's palette.
  ...SY.SLOTS,
};

/** Coverage of each wisp tier, 0..255. The steps are even in how THIN they
 *  read rather than in the number: the eye barely separates 200 from 255, so
 *  the first tier already drops a quarter. */
export const WISP_ALPHA = { [ART.HAIR_WISP1]: 190, [ART.HAIR_WISP2]: 120,
                            [ART.HAIR_WISP3]: 60 };

/** Wisp slot -> `#rrggbbaa` (vox.js hexAlpha). The tips of a strand are the
 *  hair colour a touch toward the light -- sun through thin hair -- at their
 *  tier's coverage. */
export function wispColors(colors) {
  const n = parseInt(String(colors.hair || '#4a3728').slice(1, 7), 16);
  const rgb = [(n >> 16) & 255, (n >> 8) & 255, n & 255]
    .map(v => Math.min(255, Math.round(v * 1.08 + 6)));
  const hex = v => v.toString(16).padStart(2, '0');
  const out = {};
  for (const [slot, a] of Object.entries(WISP_ALPHA))
    out[slot] = '#' + rgb.map(hex).join('') + hex(a);
  return out;
}

/** What hair is made of once it leaves the scalp. The painted cap on the
 *  skull stays FLESH -- it is the scalp. The MASS (volume, crests, buns,
 *  anything that hangs) is `hair_white`, which catches fast, burns out to ash
 *  and does not bleed. Its own white is never seen while the art layer holds:
 *  the name is the material's, not the character's. By NAME, for FLESH_ID's
 *  reason. */
export const HAIR_MAT_ID = 'hair_white';

/** The parts hairMass may add, and what each rides. `hair` turns with the
 *  head; `mane` is everything below the neck and rides the TORSO, because a
 *  head that turns sixty degrees to look would otherwise swing a curtain of
 *  hair through the shoulders. Neither exists on a body whose style has no
 *  mass, so every pre-existing character builds exactly as it did. */
export const HAIR_PARTS = { hair: 'head', mane: 'torso', snout: 'head',
                            bough: 'torso' };

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
  ...SY.COLOR_SLOTS,
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
      /** 'human' | 'sylvan' (sylvan.js). The SAME rig either way -- the race
       *  is the surface, the materials and what grows off the head, never a
       *  limb box, so armour and weapons fit both. Switched with applyRace,
       *  which moves the build by SY.BUILD_DELTA the way applySex moves it. */
      race: 'human',
      /** 'male' | 'female'. Not a switch in the generator so much as a
       *  PRESET OF OFFSETS over the build genes (applySex): the toggle moves
       *  the sliders, the sliders build the body. Two things read it directly:
       *  the bust and the bra (torsoVox), which exist only on a woman. */
      sex: 'male',
      /** Chest forward of the torso, 0..1. Female only. */
      bust: 0,
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
      /** Shoulder to fingertip: segment LENGTHS, whose sum against the stock
       *  27 sets how far the arm reaches (see limbTable). */
      armStack: { upper: 11, fore: 11, hand: 5 },
      /** Marks a genome written since armStack became lengths. A stored
       *  genome without it had WEIGHTS there -- the arm always spanned
       *  shoulder to pelvis -- and normalizeGenome rescales it to the stock
       *  sum, which keeps its ratios and so rebuilds it cell-identical. */
      armLengths: 1,
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
      // THE ARM'S MUSCLE (armSection). Unlike armGirth these may WIDEN the
      // arm's boxes, which is the only way a 4-wide limb can show a shape at
      // all. At these defaults the arm is cell-identical to the stock figure.
      upperArm: 1.0,     // upper arm thickness
      bicep: 0,          // front bulge, mid upper arm
      deltoid: 0,        // shoulder cap, top of the upper arm
      foreArm: 1.0,      // forearm thickness
      forearmMuscle: 0,  // mass under the elbow, tapering to the wrist
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
      beard: 0,         // painted facial hair: stubble .. moustache .. full
      // ---- 2026-09-25: the finer face. EVERY DEFAULT IS THE FACE AS IT WAS
      // DRAWN BEFORE THESE EXISTED, so a genome written earlier builds the
      // same head cell for cell. Units are authored micro (one painted cell
      // ships as a 2x2 block) unless a comment says otherwise.
      // Eyebrows (only drawn when `brow` is on).
      browHeight: 0,    // rows above the default brow row
      browInner: 1,     // cells past the eye toward the nose (3 meets: unibrow)
      browOuter: 0,     // cells past the eye toward the ear
      browThick: 1,     // rows
      browTilt: 0,      // -1 inner ends down (stern) .. 1 inner ends up (worried)
      browArch: 0,      // lifts the middle of each brow
      browSparse: 0,    // share of brow cells left bare
      browTone: 'shade',
      browProud: false, // a brow ridge one cell proud of the face
      // Mouth.
      mouthCurve: 0,    // -1 frown .. 1 smile: corners turn down or up
      mouthSkew: 0,     // -1..1 lifts one corner: a smirk
      mouthShift: 0,    // cells sideways
      mouthOpen: 0,     // rows of open mouth under the line
      mouthTone: 'shade',
      lips: 0,          // 0 none, 1 a lit lower lip, 2 both lips
      // Other features.
      nose: 0,          // rows of nose standing one cell proud, below the eyes
      noseWidth: 1,     // half-width: 1 is two cells, 2 is four
      earSize: 1,       // 0 small, 1 the drawn ear, 2 large
      earPoint: false,  // an elf point on top of each ear
      eyeWhites: false, // a lit cell outward of each pupil
      eyeBags: 0,       // shade under the eyes
      cheeks: 0,        // -1 hollow (shade) .. 1 high cheekbones (lit)
      freckles: 0,      // speckle across the nose and cheeks
      wrinkles: 0,      // crow's feet, then a forehead line, then smile lines
      scar: 0,          // -1..1: a slash over the left/right eye, this long
      // Beard, around `beard` (its reach). Every modifier is 0 = as drawn.
      beardStyle: 'clean',
      beardLip: 0,      // moustache: -1 shaved .. 1 wide, drooping (horseshoe)
      beardChin: 0,     // chin under the mouth: -1 shaved .. 1 wide
      beardJaw: 0,      // rows added to (or taken off) the jaw line
      beardCheek: 0,    // -1 bare cheeks .. 1 cheeks filled to the cheekbone
      beardSide: 0,     // sideburns: 0 none .. 1 down to the jaw
      beardWidth: 0,    // -1..1 scales how far round the face it spreads
      beardDensity: 1,  // below 1 the beard goes patchy (stubble, shadow)
      beardRagged: 0.45, // share of edge cells left bare (0 = razor-neat)
      beardTone: 'shade',
      beardLen: 0,      // real hair hanging off the chin: 1 reaches the waist
      beardPoint: 0,    // the hanging beard tapers to a point
      beardFork: false, // ...to two points
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
      // ---- the hair MASS (hairMass): real voxels off the scalp, built on the
      // shipped lattice into their own `hair` / `mane` parts. Every default is
      // "none", so a genome written before these genes existed builds the
      // same body it always did.
      side: 0,          // parting: -1..1 tilts the fringe line to one side
      band: 1,          // fraction of the skull's width that grows hair
      volume: 0,        // shipped voxels of hair standing off the scalp
      crest: 0,         // mohawk fin height, shipped voxels
      spikes: 0,        // spike length, shipped voxels
      buns: false,      // two buns on the crown
      drop: 0,          // loose hanging length: 0 none .. 1 to the waist
      tail: 0,          // ponytail length, on the same scale as drop
      braid: false,     // the tail is plaited
      twin: false,      // two tails behind the ears instead of one
      wisp: 0.35,       // share of a hanging strand that thins to nothing
      ragged: 0.3,      // how uneven the ends are
      // ---- bangs and REGIONAL THINNING (2026-09-24). `wisp` thins only the
      // ends of what hangs, which is all at the back; these thin by WHERE a
      // cell is. The mass fades through the wisp slots (see thinMass); the
      // painted cap cannot go see-through (it is the skull's own surface, and
      // under it is the inside of the head), so on the cap "thin" means scalp
      // showing through: SKIN_LIGHT speckle. All 0 = the body it always was.
      bangs: 0,         // fringe hanging over the forehead: 1 reaches the eyes
      faceThin: 0,      // mass near the face (bangs included) fades
      hairlineSoft: 0,  // the cap's edge breaks up into scalp
      sideThin: 0,      // temples and the sides of the head and curtain
      topThin: 0,       // the crown
      fluff: 0,         // the outer surface of any mass breaks into wisps
      // ---- THE SHORT HAIR LAYER (2026-09-25): a coat of very short,
      // see-through hair over the skull -- over bare skin as well as over the
      // cap -- so an undercut's shaved sides can carry a fade, a bald head a
      // buzz. NOT PART OF ANY STYLE: HAIR_NONE leaves these out, so picking a
      // style keeps the layer you built. Lengths are shipped voxels.
      fuzz: 0,          // 0 off .. 1 densest (still see-through)
      fuzzTop: 1,       // length on top of the head
      fuzzSide: 1,      // length over the sides and temples
      fuzzBack: 1,      // length at the back
      fuzzLow: 0.5,     // how far down it reaches: 0 the eye row, 1 the nape
      fuzzFray: 0.5,    // how uneven the lengths are
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
      // Sylvan-only colours (sylvan.js). Carried by every genome so a race
      // switch has somewhere to put them; a human never paints them.
      root: SY.DEFAULT_COLORS.root,
      moss: SY.DEFAULT_COLORS.moss,
      branch: SY.DEFAULT_COLORS.branch,
      flower: SY.DEFAULT_COLORS.flower,
      flowerEye: SY.DEFAULT_COLORS.flowerEye,
      leaf2: SY.DEFAULT_COLORS.leaf2,
      leaf3: SY.DEFAULT_COLORS.leaf3,
      leaf4: SY.DEFAULT_COLORS.leaf4,
    },

    /** The sylvan's genes: face, bark and crown (sylvan.js). Inert on a
     *  human, and skipped by a human's mutation, roll and cross loops without
     *  consuming a random draw. */
    sylvan: SY.defaultSylvan(),
  };
}

/** Hairstyles, as PRESETS of the hair genes. A style is never a separate
 *  code path, so a hand-dragged slider and a picked style are the same thing
 *  to the generator and mutation does not have to know which one produced the
 *  body. Cap numbers (`back`/`front`/`length`) are authored micro at the
 *  default 16-micro head and scale with it; the mass numbers are shipped
 *  voxels and fractions (see the genome).
 *
 *  EVERY STYLE STATES EVERY GENE, through HAIR_NONE. applyHairStyle is an
 *  Object.assign, so a style that left `tail` out would keep the ponytail of
 *  whatever was picked before it. */
const HAIR_NONE = { back: 99, front: 99, length: 0, knot: false, side: 0,
                    band: 1, volume: 0, crest: 0, spikes: 0, buns: false,
                    drop: 0, tail: 0, braid: false, twin: false, wisp: 0.35,
                    ragged: 0.3, bangs: 0, faceThin: 0, hairlineSoft: 0,
                    sideThin: 0, topThin: 0, fluff: 0 };
const hs = o => ({ ...HAIR_NONE, ...o });
export const HAIR_STYLES = {
  // ---- short: paint on the skull, and at most a little mass
  bald:     hs({}),
  cropped:  hs({ back: 8.5, front: 8.5 }),
  swept:    hs({ back: 5.5, front: 10.0 }),
  fringe:   hs({ back: 9.5, front: 5.0, bangs: 0.45, faceThin: 0.5,
                 hairlineSoft: 0.25 }),
  thinning: hs({ back: 6.0, front: 10.5, hairlineSoft: 0.7, topThin: 0.8,
                 sideThin: 0.3 }),
  sidepart: hs({ back: 5.5, front: 8.0, side: 0.8, volume: 1 }),
  crew:     hs({ back: 7.0, front: 9.5, volume: 1 }),
  bowl:     hs({ back: 6.0, front: 6.0, volume: 2, ragged: 0 }),
  undercut: hs({ back: 9.5, front: 9.0, band: 0.55, volume: 2, side: 0.5 }),
  mohawk:   hs({ back: 5.0, front: 9.0, band: 0.22, crest: 5, ragged: 0.2 }),
  spiky:    hs({ back: 6.5, front: 9.0, volume: 1, spikes: 5 }),
  afro:     hs({ back: 5.0, front: 9.5, volume: 4, wisp: 0, fluff: 0.3 }),
  topknot:  hs({ back: 10.5, front: 11.5, knot: true }),
  buns:     hs({ back: 5.5, front: 9.5, buns: true }),
  // ---- hanging: a `mane` part below the neck, thinning to wisps
  bob:      hs({ back: 4.5, front: 7.0, volume: 1, drop: 0.12, wisp: 0.25,
                 ragged: 0.1, bangs: 0.85, faceThin: 0.45 }),
  blunt:    hs({ back: 4.5, front: 7.5, volume: 1, drop: 0.3, wisp: 0.2,
                 ragged: 0, bangs: 1.0, faceThin: 0.6, sideThin: 0.2 }),
  long:     hs({ back: 4.5, front: 9.5, length: 6, drop: 0.6, faceThin: 0.25,
                 sideThin: 0.2 }),
  mane:     hs({ back: 3.5, front: 8.5, length: 10, volume: 2, drop: 0.75,
                 ragged: 0.6, sideThin: 0.25 }),
  flowing:  hs({ back: 4.0, front: 8.0, side: -0.6, volume: 1, drop: 1.0,
                 wisp: 0.45, ragged: 0.45, bangs: 0.6, faceThin: 0.55,
                 sideThin: 0.3 }),
  ponytail: hs({ back: 5.0, front: 10.0, tail: 0.7 }),
  braid:    hs({ back: 5.0, front: 10.0, tail: 0.95, braid: true,
                 wisp: 0.15 }),
  pigtails: hs({ back: 5.0, front: 7.0, tail: 0.5, twin: true }),
  wild:     hs({ back: 3.5, front: 6.0, volume: 3, spikes: 3, drop: 0.45,
                 ragged: 0.9, wisp: 0.5, fluff: 0.35, faceThin: 0.3 }),
};
export const HAIR_STYLE_ORDER = Object.keys(HAIR_STYLES);

/** Facial hair, as PRESETS of the beard genes -- the same contract as
 *  HAIR_STYLES: a style is data, not a code path, and every style states
 *  every beard gene through BEARD_NONE so a pick never inherits half of the
 *  previous one. Ordered light to heavy, so stepping the slider walks from
 *  clean-shaven through the moustaches and chin styles to the long beards. */
const BEARD_NONE = { beard: 0, beardLip: 0, beardChin: 0, beardJaw: 0,
                     beardCheek: 0, beardSide: 0, beardWidth: 0,
                     beardDensity: 1, beardRagged: 0.45, beardTone: 'shade',
                     beardLen: 0, beardPoint: 0, beardFork: false };
const bs = o => ({ ...BEARD_NONE, ...o });
// No moustache, no chin, no jaw: the building blocks the styles combine.
const NO_JAW = { beardJaw: -4 };
export const BEARD_STYLES = {
  clean:      bs({}),
  shadow:     bs({ beard: 1, beardDensity: 0.2, beardRagged: 0.8,
                   beardTone: 'hair' }),
  stubble:    bs({ beard: 1, beardDensity: 0.45, beardRagged: 0.7 }),
  pencil:     bs({ beard: 0.1, beardLip: -0.2, beardChin: -1, ...NO_JAW,
                   beardRagged: 0 }),
  moustache:  bs({ beard: 0.15, beardChin: -1, ...NO_JAW, beardRagged: 0.15 }),
  walrus:     bs({ beard: 0.2, beardLip: 0.5, beardChin: -1, ...NO_JAW,
                   beardRagged: 0.7 }),
  handlebar:  bs({ beard: 0.15, beardLip: 0.7, beardChin: -1, ...NO_JAW,
                   beardRagged: 0.1 }),
  horseshoe:  bs({ beard: 0.2, beardLip: 1, beardChin: -1, ...NO_JAW }),
  soulpatch:  bs({ beard: 0.1, beardLip: -1, beardChin: -0.55, ...NO_JAW,
                   beardRagged: 0 }),
  goatee:     bs({ beard: 0.2, beardLip: -1, ...NO_JAW, beardRagged: 0.1 }),
  circle:     bs({ beard: 0.2, ...NO_JAW, beardRagged: 0.2 }),
  vandyke:    bs({ beard: 0.2, beardLip: 0.3, beardChin: -0.3, ...NO_JAW,
                   beardLen: 0.08, beardPoint: 1 }),
  chinstrap:  bs({ beard: 0.6, beardLip: -1, beardJaw: -1, beardRagged: 0.1 }),
  curtain:    bs({ beard: 0.8, beardLip: -1, beardCheek: -1 }),
  jawline:    bs({ beard: 0.5 }),
  // Trimmed: a row lower up the sides than a full beard. At a ten-cell face
  // every amount past ~0.7 already covers the lower face, so `short` has to
  // differ in shape, not in amount.
  short:      bs({ beard: 0.7, beardJaw: -1, beardRagged: 0.1 }),
  full:       bs({ beard: 1 }),
  bushy:      bs({ beard: 1, beardWidth: 0.6, beardRagged: 0.8, beardCheek: 1,
                   beardSide: 0.8, beardLen: 0.1 }),
  muttonchops: bs({ beard: 0.9, beardLip: -1, beardChin: -1, beardCheek: 1,
                    beardSide: 1 }),
  friendly:   bs({ beard: 0.9, beardLip: 0.3, beardChin: -1, beardCheek: 1,
                   beardSide: 1 }),
  sideburns:  bs({ beardSide: 0.6 }),
  long:       bs({ beard: 1, beardLen: 0.35, beardPoint: 0.3 }),
  forked:     bs({ beard: 1, beardLen: 0.45, beardPoint: 0.8, beardFork: true }),
  wizard:     bs({ beard: 1, beardSide: 1, beardLen: 0.85, beardPoint: 0.7,
                   beardRagged: 0.6 }),
};
export const BEARD_STYLE_ORDER = Object.keys(BEARD_STYLES);
const BEARD_KEYS = Object.keys(BEARD_NONE);

function weightedPick(weights, r) {
  const ks = Object.keys(weights);
  let t = r() * ks.reduce((a, k) => a + weights[k], 0);
  for (const k of ks) { t -= weights[k]; if (t < 0) return k; }
  return ks[ks.length - 1];
}

/** Snap the beard genes to a named style (see applyHairStyle). */
export function applyBeardStyle(genome, style) {
  const s = BEARD_STYLES[style];
  if (!s) return genome;
  genome.face.beardStyle = style;
  Object.assign(genome.face, s);
  return genome;
}

/** Which beard styles a random male roll picks from, and how often. Clean-
 *  shaven is the most common single choice; the long ones are rare. */
const BEARD_ROLL = { clean: 10, shadow: 3, stubble: 4, moustache: 2,
                     walrus: 1, handlebar: 0.5, horseshoe: 1, goatee: 2,
                     circle: 2, vandyke: 1, soulpatch: 0.5, chinstrap: 1,
                     curtain: 0.5, jawline: 2, short: 3, full: 3, bushy: 1,
                     muttonchops: 0.7, friendly: 0.5, sideburns: 1, long: 1,
                     forked: 0.4, wizard: 0.3, pencil: 0.5 };

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
  { key: 'arms', title: 'arms and muscle',
    note: 'The three lengths ADD UP to the arm (the stock arm hangs to the ' +
          'hip line; longer reaches down the thigh). Size and muscle widen ' +
          'the arm itself, so a big arm hangs a little further out.' },
  { key: 'face', title: 'head and face',
    note: 'A face here is about twenty voxels across, so small moves read ' +
          'large. The side view is where the skull shape shows.' },
  { key: 'brows', title: 'eyebrows',
    note: 'Painted over the eyes in the hair colour (or the tone picked ' +
          'here). Every row below needs the eyebrows box ticked; one cell ' +
          'here ships as a 2x2 block, so a brow is two or three cells long.' },
  { key: 'mouth', title: 'mouth',
    note: 'A line of paint a couple of cells long, so expression is corners: ' +
          'curve turns both ends, skew lifts one. Opening it paints dark rows ' +
          'under the line.' },
  { key: 'details', title: 'face details',
    note: 'Nose and ears are real geometry (a cell standing proud of the ' +
          'face); the rest is paint in the skin tones -- shade for hollows, ' +
          'lines and freckles, the lit tone for highlights and scars.' },
  { key: 'beard', title: 'facial hair',
    note: 'Pick a style to set everything below, then drag. The beard is ' +
          'paint on the lower face built from regions -- moustache, chin, ' +
          'jaw, cheeks, sideburns -- and the hanging length is real hair ' +
          'off the chin that thins to wisps at the end.' },
  { key: 'hair', title: 'hair',
    note: 'Two layers. The CAP is paint on the skull: how far down the back ' +
          'and the forehead it reaches. The MASS is real hair off the scalp ' +
          '-- volume, crests, buns, and anything that hangs, which thins to ' +
          'see-through wisps at the ends.' },
  { key: 'fuzz', title: 'short hair layer',
    note: 'A coat of very short see-through hair over the whole skull, bare ' +
          'skin included -- a fade on shaved sides, a buzz on a bald head, ' +
          'stubble growing back. Separate from the style above: picking a ' +
          'style keeps it. Lengths are in voxels.' },
  // The sylvan's groups (sylvan.js). `race` hides them on a human's page.
  ...SY.GENE_GROUPS,
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
 *
 * `race` (sylvan.js) marks a gene only that race has: the page hides it on
 * any other, and mutate / randomGenome / cross skip it WITHOUT a random draw,
 * which is what keeps every human litter's sequence exactly as it was.
 * `fixed` marks a gene no roll ever moves (the race itself: a litter is the
 * race you are working on; switching is applyRace's job).
 */
export const GENE_SPECS = [
  { path: 'body.race', label: 'race', group: 'body', fixed: true,
    hint: 'Human, or sylvan: a wood spirit on the same frame -- bark over ' +
          'sapwood, roots wound round the limbs, a crown of leaves, ' +
          'branches and flowers, glowing eyes. The same limbs and ' +
          'proportions, so every armour piece and weapon fits both.',
    kind: 'enum', choices: SY.RACES, sigma: 0 },
  { path: 'body.sex', label: 'sex', group: 'body',
    hint: 'Male or female. Switching moves the build sliders by the ' +
          'typical difference (shorter, narrower shoulders, narrower waist, ' +
          'fuller hips, softer jaw, slimmer arms) and adds a bust and a bra; ' +
          'every slider still moves freely afterwards.',
    kind: 'enum', choices: ['male', 'female'], sigma: 0 },
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
  { path: 'body.bust', label: 'bust', group: 'body',
    hint: 'How far the chest stands forward of the ribs. Female bodies ' +
          'only; always covered by the bra.',
    min: 0, max: 1, step: 0.05, sigma: 0.12 },
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

  { path: 'body.armStack.upper', label: 'upper arm', group: 'arms',
    hint: 'Shoulder to elbow. A LENGTH: raising it moves the elbow and the ' +
          'hand down together, the forearm keeps its own length.',
    min: 8, max: 15, step: 0.5, sigma: 0.8 },
  { path: 'body.armStack.fore', label: 'forearm', group: 'arms',
    hint: 'Elbow to wrist. A length, like the upper arm.',
    min: 8, max: 15, step: 0.5, sigma: 0.8 },
  { path: 'body.armStack.hand', label: 'hand', group: 'arms',
    hint: 'Wrist to fingertip. This is also where a held weapon’s grip ' +
          'sits, so a tiny hand holds a sword close in.',
    // 3, not 4: a pre-lengths genome is rescaled to the stock sum on load
    // (normalizeGenome), and a long-armed one's 4-micro hand lands at ~3.4.
    // Clamping it back to 4 would change the arm it rebuilds.
    min: 3, max: 8, step: 0.5, sigma: 0.4 },
  { path: 'shape.upperArm', label: 'upper arm size', group: 'arms',
    hint: 'How thick the upper arm is, on top of arm girth. Past about 1.25 ' +
          'the arm gets a wider box and starts to show real shape.',
    min: 0.8, max: 2.0, step: 0.05, sigma: 0.06 },
  { path: 'shape.bicep', label: 'bicep', group: 'arms',
    hint: 'A bulge on the front of the upper arm, peaking a little above ' +
          'the elbow, with a smaller triceps swell behind it.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'shape.deltoid', label: 'deltoid', group: 'arms',
    hint: 'The cap of muscle over the top of the upper arm. Broadens the ' +
          'shoulder line outward without widening the torso.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'shape.foreArm', label: 'forearm size', group: 'arms',
    hint: 'How thick the forearm is, on top of arm girth, independent of ' +
          'the upper arm.',
    min: 0.8, max: 2.0, step: 0.05, sigma: 0.06 },
  { path: 'shape.forearmMuscle', label: 'forearm muscle', group: 'arms',
    hint: 'Mass just under the elbow that tapers toward the wrist -- the ' +
          'Popeye forearm at the top of the range.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },

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
  { path: 'face.eyeWhites', label: 'eye whites', group: 'face', kind: 'bool',
    hint: 'A lit cell outward of each pupil, so the eye reads as an eye ' +
          'rather than a dot.',
    roll: 0.3, sigma: 0.1 },
  { path: 'face.eyeBags', label: 'under-eyes', group: 'face',
    hint: 'Shade under the eyes: tired, old, or hard-living. Past halfway ' +
          'it spreads to the outer corner.',
    min: 0, max: 1, step: 0.05, roll: 0.2, sigma: 0.1 },

  // ---- eyebrows ----
  { path: 'face.brow', label: 'eyebrows', group: 'brows', kind: 'bool',
    hint: 'Paints a brow over each eye. Everything else in this group ' +
          'shapes it.',
    sigma: 0.25 },
  { path: 'face.browHeight', label: 'height', group: 'brows',
    hint: 'Rows above where the brow normally sits. Down is a heavy, ' +
          'hooded look; up is surprise.',
    min: -1, max: 2, step: 1, int: true, sigma: 0.4 },
  { path: 'face.browThick', label: 'thickness', group: 'brows',
    hint: 'Rows of brow. 1 is a line, 3 is bushy.',
    min: 1, max: 3, step: 1, int: true, sigma: 0.4 },
  { path: 'face.browInner', label: 'inner reach', group: 'brows',
    hint: 'Cells past the eye toward the nose. At 3 the two brows meet in ' +
          'the middle: a unibrow.',
    min: 0, max: 3, step: 1, int: true, sigma: 0.4 },
  { path: 'face.browOuter', label: 'outer reach', group: 'brows',
    hint: 'Cells past the eye toward the temple.',
    min: 0, max: 2, step: 1, int: true, sigma: 0.4 },
  { path: 'face.browTilt', label: 'tilt', group: 'brows',
    hint: 'Below 0 the inner ends drop into a frown (stern, angry); above ' +
          '0 they lift (worried, sad). A cell of slope needs a brow at ' +
          'least two cells long.',
    min: -1, max: 1, step: 0.1, sigma: 0.25 },
  { path: 'face.browArch', label: 'arch', group: 'brows',
    hint: 'Lifts the middle of each brow over the ends. Needs three or ' +
          'more cells of brow to show.',
    min: 0, max: 1, step: 0.05, sigma: 0.2 },
  { path: 'face.browSparse', label: 'sparseness', group: 'brows',
    hint: 'Leaves brow cells bare at random: thin, plucked or greying ' +
          'brows.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'face.browTone', label: 'colour', group: 'brows', kind: 'enum',
    hint: 'Which paint the brow uses: the hair shade (default), the hair ' +
          'itself, the eye colour (darkest), or the lit skin tone (pale or ' +
          'white brows).',
    choices: ['shade', 'hair', 'dark', 'light'], sigma: 0.1 },
  { path: 'face.browProud', label: 'brow ridge', group: 'brows', kind: 'bool',
    hint: 'The brow stands one cell proud of the face: a heavy ridge that ' +
          'shadows the eyes. Side view.',
    roll: 0.3, sigma: 0.1 },

  // ---- mouth ----
  { path: 'face.mouthWidth', label: 'width', group: 'mouth',
    hint: 'How far the mouth line runs. 1 is a dot, 4 spans the face.',
    min: 1, max: 4, step: 1, int: true, sigma: 0.6 },
  { path: 'face.mouthDrop', label: 'height', group: 'mouth',
    hint: 'How far below the eyes the mouth sits. Bigger drops lengthen the ' +
          'face and shorten the chin under it.',
    min: 2, max: 5, step: 1, int: true, sigma: 0.6 },
  { path: 'face.mouthShift', label: 'offset', group: 'mouth',
    hint: 'Moves the mouth a cell to one side. Off-centre reads as a ' +
          'crooked, lived-in face.',
    min: -1, max: 1, step: 1, int: true, roll: 0.25, sigma: 0.2 },
  { path: 'face.mouthCurve', label: 'smile', group: 'mouth',
    hint: 'Above 0 a corner cell turns up past each end of the mouth (a ' +
          'smile); below 0 it turns down (a frown). Past 0.7 on a wide ' +
          'mouth the ends themselves lift too.',
    min: -1, max: 1, step: 0.1, sigma: 0.25 },
  { path: 'face.mouthSkew', label: 'smirk', group: 'mouth',
    hint: 'Lifts one end of the mouth line: below 0 the left, above 0 the ' +
          'right.',
    min: -1, max: 1, step: 0.1, roll: 0.4, sigma: 0.25 },
  { path: 'face.mouthOpen', label: 'open', group: 'mouth',
    hint: 'Rows of open mouth under the line, painted dark.',
    min: 0, max: 2, step: 1, int: true, roll: 0.2, sigma: 0.3 },
  { path: 'face.mouthTone', label: 'line colour', group: 'mouth',
    kind: 'enum',
    hint: 'The mouth line in the skin shade (soft) or the eye colour (a ' +
          'hard dark line).',
    choices: ['shade', 'dark'], sigma: 0.1 },
  { path: 'face.lips', label: 'lips', group: 'mouth',
    hint: 'A lit row under the mouth for the lower lip; 2 lights the upper ' +
          'lip too. Hidden under a moustache.',
    min: 0, max: 2, step: 1, int: true, sigma: 0.3 },

  // ---- other features ----
  { path: 'face.nose', label: 'nose length', group: 'details',
    hint: 'Rows of nose standing a cell proud of the face, down from the ' +
          'eye row. It stops above the mouth however long you ask for.',
    min: 0, max: 3, step: 1, int: true, sigma: 0.4 },
  { path: 'face.noseWidth', label: 'nose width', group: 'details',
    hint: '1 is a narrow two-cell nose, 2 a broad four-cell one.',
    min: 1, max: 2, step: 1, int: true, sigma: 0.3 },
  { path: 'face.earSize', label: 'ears', group: 'details',
    hint: 'Rows of ear: 0 small, 1 as drawn, 2 large.',
    min: 0, max: 2, step: 1, int: true, sigma: 0.3 },
  { path: 'face.earPoint', label: 'pointed ears', group: 'details',
    kind: 'bool',
    hint: 'A point on top of each ear -- elf, goblin.',
    roll: 0.1, sigma: 0.05 },
  { path: 'face.cheeks', label: 'cheeks', group: 'details',
    hint: 'Below 0 shades a hollow under the cheekbones (gaunt); above 0 ' +
          'lights them (high cheekbones, a flush). Past 0.6 it takes two rows.',
    min: -1, max: 1, step: 0.05, roll: 0.3, sigma: 0.2 },
  { path: 'face.freckles', label: 'freckles', group: 'details',
    hint: 'Speckle of the skin shade across the nose and cheeks.',
    min: 0, max: 1, step: 0.05, roll: 0.15, sigma: 0.1 },
  { path: 'face.wrinkles', label: 'age lines', group: 'details',
    hint: 'Adds in steps: crow’s feet at the eyes, then a line across ' +
          'the forehead, then lines from the nose to the mouth corners.',
    min: 0, max: 1, step: 0.05, roll: 0.2, sigma: 0.1 },
  { path: 'face.scar', label: 'scar', group: 'details',
    hint: 'A pale slash down through one eye and brow: below 0 the left, ' +
          'above 0 the right, longer further out.',
    min: -1, max: 1, step: 0.05, roll: 0.08, sigma: 0.1 },

  // ---- facial hair ----
  { path: 'face.beardStyle', label: 'style', group: 'beard', kind: 'enum',
    hint: 'A preset of every setting below. Picking one overwrites them; ' +
          'drag them afterwards and you are off the preset, which is fine.',
    choices: BEARD_STYLE_ORDER, sigma: 0.2 },
  { path: 'face.beard', label: 'amount', group: 'beard',
    hint: 'How far the beard reaches: a little is a moustache and goatee, ' +
          'the middle runs along the jaw, the top is a full beard. 0 is ' +
          'none (sideburns are separate).',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'face.beardLip', label: 'moustache', group: 'beard',
    hint: 'The upper lip. -1 shaves it (chin beards without a moustache); ' +
          'above 0 it widens past the mouth, and from 0.75 droops down both ' +
          'sides of the chin (horseshoe).',
    min: -1, max: 1, step: 0.05, sigma: 0.2 },
  { path: 'face.beardChin', label: 'chin', group: 'beard',
    hint: 'The patch under the mouth. -1 shaves it (moustache only, mutton ' +
          'chops); about -0.5 leaves a narrow soul patch; above 0 widens it.',
    min: -1, max: 1, step: 0.05, sigma: 0.2 },
  { path: 'face.beardJaw', label: 'jaw line', group: 'beard',
    hint: 'Rows of beard along the jaw, added to or taken from what the ' +
          'amount gives. -4 clears the jaw: goatees and circle beards.',
    min: -4, max: 3, step: 1, int: true, sigma: 0.6 },
  { path: 'face.beardCheek', label: 'cheeks', group: 'beard',
    hint: 'How far up the cheeks it grows. 0 follows the amount (cheeks ' +
          'fill only on a full beard); 1 always fills to the cheekbone; -1 ' +
          'keeps them bare.',
    min: -1, max: 1, step: 0.05, sigma: 0.2 },
  { path: 'face.beardSide', label: 'sideburns', group: 'beard',
    hint: 'Hair down the side of the face in front of the ears, from the ' +
          'hairline: 1 reaches the jaw. Works with no beard at all.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'face.beardWidth', label: 'spread', group: 'beard',
    hint: 'Narrows or widens how far round the face the beard reaches, at ' +
          'the same amount.',
    min: -1, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'face.beardDensity', label: 'density', group: 'beard',
    hint: 'Below 1 cells go bare at random: 0.2 is a five-o’clock ' +
          'shadow, 0.5 stubble or a patchy young beard.',
    min: 0.05, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'face.beardRagged', label: 'ragged edge', group: 'beard',
    hint: 'Share of the cells along the beard’s edge left bare. 0 is ' +
          'razor-trimmed.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'face.beardTone', label: 'colour', group: 'beard', kind: 'enum',
    hint: 'The hair shade (default, darker) or the hair colour itself.',
    choices: ['shade', 'hair'], sigma: 0.1 },
  { path: 'face.beardLen', label: 'length', group: 'beard',
    hint: 'Real hair hanging from the chin, on the same scale as hair ' +
          'length: 0.25 is a hand below the chin, 1 the waist. Needs some ' +
          'beard on the chin to hang from.',
    min: 0, max: 1, step: 0.05, sigma: 0.1 },
  { path: 'face.beardPoint', label: 'taper', group: 'beard',
    hint: 'How much the hanging beard narrows to a point. 0 is cut square.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'face.beardFork', label: 'forked', group: 'beard', kind: 'bool',
    hint: 'The taper goes to two points instead of one.',
    sigma: 0.1 },

  { path: 'hair.style', label: 'style', group: 'hair',
    hint: 'A preset of every setting below. Picking one overwrites them; ' +
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
  { path: 'hair.side', label: 'parting', group: 'hair',
    hint: 'Tilts the fringe so it sweeps down over one eye and lifts off ' +
          'the other. 0 is a centre part.',
    min: -1, max: 1, step: 0.1, sigma: 0.3 },
  { path: 'hair.band', label: 'width on top', group: 'hair',
    hint: 'How much of the skull, side to side, grows it. Narrow is an ' +
          'undercut; narrower still is a mohawk strip.',
    min: 0.15, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.volume', label: 'volume', group: 'hair',
    hint: 'How far it stands off the scalp. Four is an afro.',
    min: 0, max: 4, step: 1, int: true, sigma: 0.8 },
  { path: 'hair.crest', label: 'crest', group: 'hair',
    hint: 'A fin standing up along the middle of the head, as tall as ' +
          'this many voxels.',
    min: 0, max: 8, step: 1, int: true, sigma: 1.2 },
  { path: 'hair.spikes', label: 'spikes', group: 'hair',
    hint: 'Stiff points radiating off the crown, this long.',
    min: 0, max: 8, step: 1, int: true, sigma: 1.2 },
  { path: 'hair.buns', label: 'buns', group: 'hair', kind: 'bool',
    hint: 'Two round knots on top, either side of the crown.',
    sigma: 0.15 },
  { path: 'hair.drop', label: 'hangs to', group: 'hair',
    hint: 'Loose hair falling from the nape: 0.25 reaches the shoulders, ' +
          '0.6 the middle of the back, 1 the waist. It lies on the back.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.tail', label: 'ponytail', group: 'hair',
    hint: 'Gathered and tied at the back of the head, hanging this far on ' +
          'the same scale as the loose length.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.braid', label: 'braided', group: 'hair', kind: 'bool',
    hint: 'Plaits the tail into crossing strands.',
    sigma: 0.2 },
  { path: 'hair.twin', label: 'twin tails', group: 'hair', kind: 'bool',
    hint: 'Two tails tied behind the ears instead of one at the back.',
    sigma: 0.2 },
  { path: 'hair.wisp', label: 'thinning ends', group: 'hair',
    hint: 'What share of each hanging strand fades toward see-through at ' +
          'its tip. 0 cuts off blunt.',
    min: 0, max: 0.8, step: 0.05, sigma: 0.1 },
  { path: 'hair.ragged', label: 'ragged ends', group: 'hair',
    hint: 'How uneven the strands are where they end. 0 is a clean cut.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.bangs', label: 'bangs', group: 'hair',
    hint: 'A fringe of real hair hanging from the hairline down over the ' +
          'forehead. 1 reaches the eyes; the parting tilts it to one side.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.faceThin', label: 'thin at face', group: 'hair',
    hint: 'Hair near the face turns see-through, most at the ends of the ' +
          'bangs where they meet the eyes. Needs bangs or hair standing ' +
          'off the head to have anything to thin.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.hairlineSoft', label: 'soft hairline', group: 'hair',
    hint: 'Breaks up the edge of the hair on the head into scalp, so the ' +
          'hairline fades out instead of ending on a ruled line.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.sideThin', label: 'thin at sides', group: 'hair',
    hint: 'Thins the temples and the sides of the head, and the outer ' +
          'edges of hair hanging down.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.topThin', label: 'thin on top', group: 'hair',
    hint: 'Thins the crown: scalp shows through on top and any hair ' +
          'standing there goes see-through. High is a thinning older head.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.fluff', label: 'wispy surface', group: 'hair',
    hint: 'Breaks the outer surface of any hair standing off the head or ' +
          'hanging into loose see-through wisps, all over.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },

  // ---- the short hair layer ----
  { path: 'hair.fuzz', label: 'amount', group: 'fuzz',
    hint: 'Turns the layer on and sets how thick it is: low is a faint ' +
          'shadow of regrowth, 1 the densest it goes (still see-through).',
    min: 0, max: 1, step: 0.05, roll: 0.15, sigma: 0.1 },
  { path: 'hair.fuzzTop', label: 'length on top', group: 'fuzz',
    hint: 'Voxels of length over the crown. 0 leaves the top bare.',
    min: 0, max: 4, step: 0.5, sigma: 0.4 },
  { path: 'hair.fuzzSide', label: 'length at sides', group: 'fuzz',
    hint: 'Voxels of length over the temples and the sides. Shorter than ' +
          'the top is a fade.',
    min: 0, max: 4, step: 0.5, sigma: 0.4 },
  { path: 'hair.fuzzBack', label: 'length at back', group: 'fuzz',
    hint: 'Voxels of length at the back of the head.',
    min: 0, max: 4, step: 0.5, sigma: 0.4 },
  { path: 'hair.fuzzLow', label: 'reaches down to', group: 'fuzz',
    hint: 'How low it grows: 0 stops level with the eyes, 1 goes down to ' +
          'the nape. The forehead stays clear either way.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },
  { path: 'hair.fuzzFray', label: 'fray', group: 'fuzz',
    hint: 'How uneven it is: tufts of different lengths and gaps. 0 is a ' +
          'clipper-even coat.',
    min: 0, max: 1, step: 0.05, sigma: 0.15 },

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
  // The sylvan's own colours, last, still in the colour group.
  ...SY.COLOR_SPECS.map(([k, label, hint]) => ({
    path: 'colors.' + k, label, hint, group: 'colour', kind: 'color',
    sigma: 0.10, race: 'sylvan',
  })),
];
// The sylvan's face / bark / crown rows go in BEFORE the colours, so the page
// stacks them under their own headings. Their place in the list moves no
// human draw: a race-gated row is skipped without one (see `race` above).
GENE_SPECS.splice(GENE_SPECS.findIndex(sp => sp.group === 'colour'), 0,
                  ...SY.GENE_SPECS);

/** Does this row apply to this genome? (race-gated rows, see GENE_SPECS) */
export const specApplies = (spec, genome) =>
  !spec.race || spec.race === ((genome && genome.body && genome.body.race) || 'human');

/** The label and hint a row shows for this genome: a sylvan's skin is bark. */
export function specText(spec, genome) {
  const race = (genome && genome.body && genome.body.race) || 'human';
  if (race === 'sylvan' && spec.path.startsWith('colors.')) {
    const t = SY.COLOR_LABELS[spec.path.slice(7)];
    if (t) return { label: t[0], hint: t[1] };
  }
  return { label: spec.label, hint: spec.hint };
}

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
    for (const k of ['body', 'shape', 'head', 'face', 'hair', 'colors',
                     'sylvan']) {
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
    // A PRE-LENGTHS GENOME (body.armLengths's note): its armStack was
    // weights over a fixed span, so rescale to the stock sum -- same ratios,
    // same arm -- before anything reads it as lengths.
    if (src.body && typeof src.body === 'object' && src.body.armStack &&
        !('armLengths' in src.body)) {
      const a = g.body.armStack;
      const sum = a.upper + a.fore + a.hand;
      if (sum > 0 && Number.isFinite(sum))
        for (const k of ['upper', 'fore', 'hand'])
          a[k] = a[k] * ARM_STOCK_SUM / sum;
    }
  }
  const def = defaultGenome();
  for (const spec of GENE_SPECS)
    setPath(g, spec.path, coerceGene(spec, getPath(g, spec.path),
                                     getPath(def, spec.path)));
  return g;
}

/**
 * THE SEXES, as offsets female-minus-male over the build genes. A toggle
 * ADDS or SUBTRACTS these rather than snapping to a stock body, so whatever
 * you did to the sliders survives the switch: a tall, broad woman switched to
 * male is a taller, broader man, not the default one. Roughly the average
 * adult difference, scaled to what the lattice can show -- shoulders are the
 * one that reads at a distance, so they move the most.
 */
export const SEX_FEMALE_DELTA = {
  'body.heightM': -0.08,
  'body.shoulderWidth': -2,
  'body.bust': 0.55,
  'shape.shoulder': -0.05,
  'shape.chest': -0.04,
  'shape.waist': -0.1,
  'shape.seat': 0.1,
  'shape.chestDepth': -0.08,
  'shape.armGirth': -0.1,
  'shape.legGirth': -0.03,
  'head.jaw': -0.1,
  'head.cheek': 0.03,
};

/** Switch a genome's sex, moving the build genes by SEX_FEMALE_DELTA. A no-op
 *  when it already is. Going female also clears the beard; going male clears
 *  nothing but the bust, which a male body never draws anyway. `locks`, when
 *  given, holds pinned genes still. */
export function applySex(genome, sex, locks) {
  if (sex !== 'male' && sex !== 'female') return genome;
  const was = genome.body.sex === 'female' ? 'female' : 'male';
  genome.body.sex = sex;
  if (was === sex) return genome;
  const sign = sex === 'female' ? 1 : -1;
  for (const [path, d] of Object.entries(SEX_FEMALE_DELTA)) {
    if (isLocked(locks, path)) continue;
    const spec = SPEC_BY_PATH.get(path);
    let v = getPath(genome, path) + sign * d;
    if (spec) v = clamp(v, spec.min, spec.max);
    setPath(genome, path, v);
  }
  if (sex === 'female' && !isLocked(locks, 'face.beard') &&
      !isLocked(locks, 'face.beardStyle')) {
    // The whole beard, not just its amount: sideburns and a hanging length
    // do not read `beard`, so zeroing that alone left them on.
    applyBeardStyle(genome, 'clean');
  }
  return genome;
}

/**
 * SWITCH RACE. Like applySex, the build moves by OFFSETS (SY.BUILD_DELTA: a
 * sylvan is slighter), so the sliders you set survive a round trip. Going
 * sylvan also puts on the race's look -- its colours, a face and a crown --
 * because a human's skin tone and hairstyle on bark are not a wood spirit, and
 * the page is where you then change them. Going human puts back the stock
 * complexion and a plain hairstyle. `locks` holds pinned genes still.
 */
export function applyRace(genome, race, locks) {
  if (!SY.RACES.includes(race)) return genome;
  const was = genome.body.race === 'sylvan' ? 'sylvan' : 'human';
  genome.body.race = race;
  if (was === race) return genome;
  const sign = race === 'sylvan' ? 1 : -1;
  for (const [path, d] of Object.entries(SY.BUILD_DELTA)) {
    if (isLocked(locks, path)) continue;
    const spec = SPEC_BY_PATH.get(path);
    let v = getPath(genome, path) + sign * d;
    if (spec) v = clamp(v, spec.min, spec.max);
    setPath(genome, path, v);
  }
  const setColors = set => {
    for (const [k, v] of Object.entries(set))
      if (!isLocked(locks, 'colors.' + k)) genome.colors[k] = v;
  };
  if (race === 'sylvan') {
    setColors(SY.DEFAULT_COLORS);
    if (!isLocked(locks, 'sylvan.face')) SY.applyFace(genome, genome.sylvan.face);
    if (!isLocked(locks, 'sylvan.crown')) applyCrown(genome, genome.sylvan.crown);
  } else {
    const def = defaultGenome().colors;
    setColors(Object.fromEntries(Object.entries(def)
      .filter(([k]) => !(k in SY.COLOR_SLOTS))));
    if (!isLocked(locks, 'hair.style'))
      applyHairStyle(genome, genome.body.sex === 'female' ? 'long' : 'swept');
  }
  return genome;
}

/** A crown is its own genes PLUS the hairstyle it sits on. */
export function applyCrown(genome, style) {
  const hs = SY.applyCrownGenes(genome, style);
  if (hs) {
    applyHairStyle(genome, hs);
    Object.assign(genome.hair, SY.crownHairOverrides(style));
  }
  return genome;
}
export const applyFace = (genome, style) => SY.applyFace(genome, style);
export const SYLVAN_FACES = SY.FACE_ORDER;
export const SYLVAN_CROWNS = SY.CROWN_ORDER;
export const SYLVAN_BRANCHES = SY.BRANCH_ORDER;
export const applyBranches = (genome, style) => SY.applyBranches(genome, style);
export const LEAF_STOCK = SY.LEAF_STOCK;
export const SYLVAN_GLOWS = SY.GLOWS;

/** Everything a random SYLVAN adds on top of a random human: a face and a
 *  crown picked whole, every sylvan number jittered about them, and a colour
 *  roll from the race's own sets. Runs AFTER the human roll, on the same rng,
 *  so a human roll's sequence is untouched. */
function sylvanize(g, r, locks) {
  applyRace(g, 'sylvan', locks);
  if (!isLocked(locks, 'sylvan.face')) SY.applyFace(g, r.pick(SY.FACE_ORDER));
  if (!isLocked(locks, 'sylvan.crown')) applyCrown(g, r.pick(SY.CROWN_ORDER));
  if (!isLocked(locks, 'sylvan.branchStyle'))
    SY.applyBranches(g, r.pick(SY.BRANCH_ORDER));
  for (const spec of SY.GENE_SPECS) {
    if (spec.kind || isLocked(locks, spec.path)) continue;
    const v = getPath(g, spec.path) + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    setPath(g, spec.path, clamp(v, spec.min, spec.max));
  }
  if (!isLocked(locks, 'sylvan.glow')) g.sylvan.glow = r() < 0.85;
  return g;
}

/** Which hairstyles a random roll favours for each sex: three times as likely
 *  as the rest, never exclusive. A roll that could not give a man a braid or a
 *  woman a crop would be a narrower generator, not a more realistic one. */
export const SEX_STYLE_BIAS = {
  male: ['cropped', 'swept', 'sidepart', 'crew', 'undercut', 'spiky',
         'thinning', 'bowl', 'mohawk'],
  female: ['long', 'flowing', 'bob', 'blunt', 'ponytail', 'braid', 'pigtails',
           'buns', 'fringe', 'mane', 'topknot'],
};

function biasedStyle(sex, r) {
  const fav = new Set(SEX_STYLE_BIAS[sex] || []);
  const w = HAIR_STYLE_ORDER.map(k => (k === 'bald' ? 0.4 : fav.has(k) ? 3 : 1));
  let t = r() * w.reduce((a, b) => a + b, 0);
  for (let i = 0; i < w.length; i++) {
    t -= w[i];
    if (t < 0) return HAIR_STYLE_ORDER[i];
  }
  return HAIR_STYLE_ORDER[HAIR_STYLE_ORDER.length - 1];
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
    if (spec.fixed || !specApplies(spec, g)) continue;   // no draw: see `race`
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
  if (!isLocked(locks, 'face.beardStyle') &&
      g.face.beardStyle !== (genome?.face?.beardStyle ?? 'clean'))
    applyBeardStyle(g, g.face.beardStyle);
  // ...and a sylvan face or crown pick, likewise.
  if (g.body.race === 'sylvan') {
    if (!isLocked(locks, 'sylvan.face') &&
        g.sylvan.face !== genome?.sylvan?.face)
      SY.applyFace(g, g.sylvan.face);
    if (!isLocked(locks, 'sylvan.crown') &&
        g.sylvan.crown !== genome?.sylvan?.crown)
      applyCrown(g, g.sylvan.crown);
    if (!isLocked(locks, 'sylvan.branchStyle') &&
        g.sylvan.branchStyle !== genome?.sylvan?.branchStyle)
      SY.applyBranches(g, g.sylvan.branchStyle);
  }
  // ...and so does a sex flip (sigma 0 on the gene: mutation never flips it
  // today, but a genome edited to flip must still move the body).
  const sexWas = genome?.body?.sex === 'female' ? 'female' : 'male';
  if (g.body.sex !== sexWas) {
    const to = g.body.sex;
    g.body.sex = sexWas;
    applySex(g, to, locks);
  }
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
    if (spec.fixed || !specApplies(spec, g)) continue;   // no draw: see `race`
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
  // ...and the beard genes from whichever parent supplied the beard style.
  const bdonor = ps.find(p => p.face.beardStyle === g.face.beardStyle) || ps[0];
  if (!isLocked(opts.locks, 'face.beardStyle'))
    for (const k of BEARD_KEYS)
      if (!isLocked(opts.locks, 'face.' + k)) g.face[k] = bdonor.face[k];
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
export function randomGenome(rng, locks, opts = {}) {
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  const g = defaultGenome();
  const was = { ...g.colors };
  for (const spec of GENE_SPECS) {
    if (isLocked(locks, spec.path)) continue;
    if (spec.fixed || !specApplies(spec, g)) continue;   // no draw: see `race`
    // A RARE FEATURE stays at its default unless its own draw comes up: a
    // scar, freckles or pointed ears on every other roll is not variety.
    if (spec.roll !== undefined && r() >= spec.roll) continue;
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
  // THE SEX IS ROLLED ON THE MALE-CENTRED BUILD, then applied as offsets, so a
  // woman is the same spread of builds shifted -- not a second distribution.
  // Drawn from the rng AFTER the loop, so every gene above sees the sequence
  // it always did.
  if (!isLocked(locks, 'body.sex')) {
    const sex = r() < 0.5 ? 'female' : 'male';
    g.body.sex = 'male';
    applySex(g, sex, locks);
    if (!isLocked(locks, 'hair.style')) g.hair.style = biasedStyle(sex, r);
    // A BEARD IS A STYLE, then a jitter of its amount: independent draws of
    // a dozen beard genes are a scribble, not facial hair.
    if (!isLocked(locks, 'face.beardStyle')) {
      applyBeardStyle(g, sex === 'male' ? weightedPick(BEARD_ROLL, r)
                                        : 'clean');
      if (g.face.beard > 0 && !isLocked(locks, 'face.beard'))
        g.face.beard = clamp(g.face.beard * (0.8 + r() * 0.4), 0.05, 1);
    }
    if (!isLocked(locks, 'body.bust') && sex === 'female')
      g.body.bust = clamp(0.25 + r() * 0.6, 0, 1);
  }
  applyHairStyle(g, g.hair.style);
  // Hair length and the knot belong to the style; re-roll length within it so
  // a `long` is not always exactly 6 micro.
  if (g.hair.length > 0) g.hair.length = Math.round(g.hair.length * (0.6 + r()));
  reshadeFamily(g, was, locks);
  // THE RACE IS NOT ROLLED: a litter is the race you are working on. A sylvan
  // is a random human, then the race's own roll on top (sylvanize).
  if (opts.race === 'sylvan') {
    sylvanize(g, r, locks);
    rollColors(g, r);
  }
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
/** The Characters page's STOCK hair colours, darkest to lightest, for the
 *  step-through slider -- a menu, not a constraint: the swatch still takes any
 *  colour. Separate from HAIR_COLORS on purpose: rollColors draws from that
 *  table by key, so growing it would change every seeded litter's colouring. */
export const HAIR_STOCK = [
  ['jet',        { hair: '#121010', hairShade: '#080707' }],
  ['black',      HAIR_COLORS.black],
  ['dark brown', HAIR_COLORS.dark],
  ['chestnut',   { hair: '#5e3a24', hairShade: '#402616' }],
  ['brown',      HAIR_COLORS.brown],
  ['ash brown',  { hair: '#6e6152', hairShade: '#4e4439' }],
  ['auburn',     HAIR_COLORS.auburn],
  ['copper',     { hair: '#a4502a', hairShade: '#76361a' }],
  ['ginger',     { hair: '#c0672e', hairShade: '#8c4a1f' }],
  ['strawberry', { hair: '#c98a5e', hairShade: '#9a633f' }],
  ['sandy',      HAIR_COLORS.sandy],
  ['blond',      HAIR_COLORS.blond],
  ['platinum',   { hair: '#e3d6b4', hairShade: '#b3a684' }],
  ['grey',       HAIR_COLORS.grey],
  ['white',      HAIR_COLORS.white],
];
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

/** A palette roll that picks SETS, then jitters inside them.
 *
 *  `opts.jitter` 0 keeps the sets EXACT and draws hair from HAIR_STOCK, which
 *  is what the engine's random-human pool is baked with (bake_human_pool.mjs):
 *  the engine merges every loaded body's colours into ONE 255-entry art
 *  palette, and exact stock colours are shared between bodies where jittered
 *  ones would each spend a dozen fresh slots. */
export function rollColors(genome, rng, opts = {}) {
  const r = typeof rng === 'function' ? rng : makeRng(rng | 0);
  const pick = obj => obj[r.pick(Object.keys(obj))];
  // A SYLVAN picks from its own sets: a bark, a foliage, a glow and a
  // flower, each whole. No jitter: the sets are the look, and every exact
  // colour is one the engine's shared art palette can dedupe across bodies.
  if (genome.body && genome.body.race === 'sylvan') {
    Object.assign(genome.colors, pick(SY.BARKS), pick(SY.LEAVES),
                  pick(SY.FLOWERS), { eye: pick(SY.GLOWS) });
    return genome;
  }
  const jitter = opts.jitter ?? 0.02;
  const hair = jitter === 0 ? r.pick(HAIR_STOCK)[1] : pick(HAIR_COLORS);
  const set = Object.assign({}, pick(COMPLEXIONS), hair,
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
  if (jitter === 0) return genome;
  const beforeJitter = { ...genome.colors };
  for (const k of Object.keys(COLOR_SLOTS))
    if (!COLOR_BASE_OF[k])
      genome.colors[k] = jitterColor(genome.colors[k], jitter, r);
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

// =============================================================================
// the arm's cross-section -- ONE function, read twice
//
// limbTable asks it how big a box the arm needs, the voxel builders ask it
// which cells to fill, so the box always fits what is drawn in it.
//
// WHY THE BOX HAS TO GROW. On the stock 4-wide arm (limbTube's note) every
// radius from ~1.6 up is the same rounded square, so no multiplier on the
// radius can show a muscle -- arm girth across its whole range moves four
// corner cells. The muscle genes therefore widen the box: a section whose
// radius needs a third cell either side gets a 6-wide box, and there a bicep
// is a visible bulge. The cross-section is an ellipse with its FRONT and BACK
// half-depths set apart (ryF / ryB; front is +y in the authored frame), which
// is what lets a bicep swell forward instead of the whole tube inflating.
//
// At the stock genes every term below is the old limbTube call exactly --
// upper arm 2.0 -> 2.2, forearm 1.8 -> 2.0, times armGirth x limbWidth / 4 --
// so an arm nobody touched is cell-identical.
// =============================================================================

/** Sum of the stock `armStack` (11 + 11 + 5): the arm length the fixed
 *  shoulder-to-pelvis span means. See the LENGTHS note in limbTable. */
const ARM_STOCK_SUM = 27;
/** The stock figure's limbWidth, which the drawn radii are authored against. */
const ARM_REF_WIDTH = 4;

/** A raised-cosine bump: 1 at `c`, 0 beyond `w` either side. */
function bump(t, c, w) {
  const d = Math.abs(t - c) / w;
  return d >= 1 ? 0 : 0.5 * (1 + Math.cos(Math.PI * d));
}
function smooth01(a, b, t) {
  const u = Math.min(1, Math.max(0, (t - a) / (b - a)));
  return u * u * (3 - 2 * u);
}

/** The section of arm segment `seg` ('upper' | 'fore') at height t (0 = its
 *  bottom, 1 = its top), in authored micro: { rx, ryF, ryB }. */
function armSection(g, seg, t) {
  const s = g.shape.armGirth * g.body.limbWidth / ARM_REF_WIDTH;
  const sh = g.shape;
  if (seg === 'upper') {
    const r = s * sh.upperArm * (2.0 + 0.2 * t);
    // Biceps peak a little below the middle; a smaller triceps swell behind.
    const bi = s * sh.bicep * 1.6 * bump(t, 0.42, 0.38);
    // The deltoid caps the top 40%, mostly OUTWARD: it is what broadens a
    // shoulder line seen from the front.
    const de = s * sh.deltoid * 1.3 * smooth01(0.55, 0.9, t);
    return { rx: r + de + 0.3 * bi, ryF: r + 0.6 * de + bi,
             ryB: r + 0.6 * de + 0.45 * bi };
  }
  const r = s * sh.foreArm * (1.8 + 0.2 * t);
  // Forearm mass sits just under the elbow and is gone by the wrist.
  const fm = s * sh.forearmMuscle * 1.4 * bump(t, 0.75, 0.45);
  return { rx: r + 0.8 * fm, ryF: r + fm, ryB: r + 0.7 * fm };
}

/** [sx, sy] for arm segment `seg` of height `sz`: the stock limbWidth square,
 *  or bigger where the section needs it. Always even (the mirror contract). */
function armBox(g, seg, sz) {
  // A cell centred k + 0.5 off the axis is inside radius r when k + 0.5 <= r,
  // so a half-width of floor(r + 0.5) cells holds every filled one.
  let hx = g.body.limbWidth / 2, hy = hx;
  for (let z = 0; z < sz; z++) {
    const q = armSection(g, seg, z / Math.max(sz - 1, 1));
    hx = Math.max(hx, Math.floor(q.rx + 0.5));
    hy = Math.max(hy, Math.floor(Math.max(q.ryF, q.ryB) + 0.5));
  }
  return [2 * hx, 2 * hy];
}

/** The arm segment's cells: limbTube with armSection's egg-shaped section. */
function armTube(g, seg, size, colFor) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const out = [];
  for (let z = 0; z < sz; z++) {
    const q = armSection(g, seg, z / Math.max(sz - 1, 1));
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++) {
        const dx = (x + 0.5 - cx) / q.rx;
        const dyr = y + 0.5 - cy;
        const dy = dyr / (dyr >= 0 ? q.ryF : q.ryB);
        if (dx * dx + dy * dy <= 1.0)
          out.push([x, y, z, colFor(x, y, z, sx, sy, sz)]);
      }
  }
  return out;
}

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
  //
  // THE SEGMENT GENES ARE LENGTHS (2026-09-25). They used to be WEIGHTS over
  // that fixed span, so "upper arm" and "forearm" could only trade rows: a
  // longer upper arm was a shorter forearm, the hand never moved, and both
  // sliders read as "moves the elbow a bit". Now the span scales with their
  // SUM against the stock arm's, so at the stock sum (27, which is also the
  // stock budget in micro) nothing moves and every existing proportion is
  // kept; only the total is new.
  const armOverlap = ARCHETYPE.armStack.reduce((s, e) => s + e.overlap, 0);
  const armSpan = rows.torso.top - rows.hips.base;
  const armSum = Object.values(b.armStack).reduce((s, v) => s + v, 0);
  // Floored so no segment vanishes; capped so the fingertips stop at the
  // ground (a long arm on short legs would otherwise break the z = 0
  // contract generateMob asserts).
  const armBudget = Math.min(
      rows.torso.top + armOverlap,
      Math.max(armOverlap + ARCHETYPE.armStack.length,
               Math.round((armSpan + armOverlap) * armSum / ARM_STOCK_SUM)));
  const armH = spendBudget(b.armStack, armBudget);
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
  // THE ARM'S BOXES FIT ITS MUSCLE (armSection). Every segment is centred on
  // ONE axis, so the shoulder, elbow and wrist anchors (box centres) stay in
  // line, and the axis sits where the WIDEST segment's inner edge touches the
  // shoulder -- a big arm hangs further out rather than into the ribs.
  const boxU = armBox(g, 'upper', armRows.upper.h);
  const boxF = armBox(g, 'fore', armRows.fore.h);
  const armAxis = armX + Math.max(lw, boxU[0], boxF[0]) / 2;
  for (const side of ARCHETYPE.sides) {
    const put = (name, x, sy, sz, z) => {
      limbs[name] = { size: [lw, sy, sz],
                      mn: [side === 'L' ? x : -x - lw, mnY(sy), z] };
    };
    const putArm = (name, [sx, sy], sz, z) => {
      limbs[name] = { size: [sx, sy, sz],
                      mn: [side === 'L' ? armAxis - sx / 2 : -armAxis - sx / 2,
                           mnY(sy), z] };
    };
    putArm(`armU.${side}`, boxU, armRows.upper.h, armRows.upper.base);
    putArm(`armL.${side}`, boxF, armRows.fore.h, armRows.fore.base);
    putArm(`hand.${side}`, [lw, lw], armRows.hand.h, armRows.hand.base);
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

/**
 * The torso. On a woman, a BUST and a BRA.
 *
 * THE BUST LIVES INSIDE THE TORSO'S OWN BOX. The front of the ribcage stops
 * short of the box's front face (torso depth < 1 on a female build, see
 * SEX_FEMALE_DELTA), and the bust fills the rows in between. Growing the box
 * instead would move every joint centred on it -- the neck, the shoulders --
 * and resample every cuirass fitted to it. Two lobes, each a bump on the
 * FRONT radius only, peaked a third of the way down from the shoulders.
 *
 * THE BRA IS PAINT, exactly as the shorts are (hipsVox): CLOTH cells on a
 * body that is skin all the way through. Cups over the bust, a band right
 * round the ribs under it, and a strap up each side of the chest to the top
 * of the shoulder, front and back. Pre-flip, high y is the FRONT.
 */
function torsoVox(g, size) {
  const [sx, sy, sz] = size;
  const cx = sx * 0.5, cy = sy * 0.5;
  const { rx: prx, ry: pry } = torsoProfiles(g, size);
  const female = g.body.sex === 'female';
  const bust = female ? g.body.bust : 0;
  // Band of the bust in u (0 = waist, 1 = shoulder top), and where the bra's
  // under-band sits.
  const uPeak = 0.66, uHalf = 0.13;
  const bandLo = uPeak - uHalf - 0.03, bandHi = uPeak + uHalf;
  const bumpAt = (x, u, rx) => {
    if (bust <= 0) return 0;
    const bu = 1 - Math.abs(u - uPeak) / uHalf;
    if (bu <= 0) return 0;
    const off = rx * 0.45, w = Math.max(rx * 0.32, 1.1);
    const dx = Math.min(Math.abs(x + 0.5 - (cx - off)),
                        Math.abs(x + 0.5 - (cx + off)));
    const bx = 1 - (dx / w) * (dx / w);
    if (bx <= 0) return 0;
    return bust * 2.2 * Math.sqrt(bx) * Math.sqrt(bu);
  };
  const out = [];
  for (let z = 0; z < sz; z++) {
    const u = z / Math.max(sz - 1, 1);
    const rx = profile(u, prx), ry = profile(u, pry);
    const strapX = rx * 0.45;
    for (let y = 0; y < sy; y++)
      for (let x = 0; x < sx; x++) {
        const front = y + 0.5 > cy;
        const bump = front ? bumpAt(x, u, rx) : 0;
        // The front radius grows by the bump but never past the box.
        const ryHere = Math.min(ry + bump, sy - cy - 0.02);
        if (!ellipseMask(x, y, cx, cy, rx, front ? ryHere : ry)) continue;
        let col = shadeBack(y, cy, ART.SKIN_BASE, ART.SKIN_SHADE);
        if (female) {
          const cup = bump > 0.25;
          const band = u >= bandLo && u < bandLo + 0.09;
          // Straps: one column pair each side, from the cups to the top.
          const strap = u > bandHi - 0.02 &&
            Math.abs(Math.abs(x + 0.5 - cx) - strapX) < 0.75;
          if (cup || band || strap)
            col = front || !cup ? shadeBack(y, cy, ART.CLOTH, ART.CLOTH_SHADE)
                                : col;
        }
        out.push([x, y, z, col]);
      }
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
  const hairT = y => clamp((y + 0.5 - (hcy - 4.0 * hy)) / (8.0 * hy), 0.0, 1.0);
  const hairLine = y => hBack + (hFront - hBack) * hairT(y);
  // THE PARTING tilts the line side to side, weighted toward the FRONT (a
  // part is a fringe that falls one way; the nape does not care). THE BAND is
  // how wide a strip across the skull grows hair at all -- measured against
  // the skull's WIDEST radius, not the row's own, so a mohawk strip is the
  // same width at the brow as at the crown instead of pinching to one cell on
  // top. At side 0 / band 1 both are exact no-ops, which is what keeps every
  // existing character's head bit-identical.
  const skullR = Math.max(...prx.map(k => k[1]));
  const bandHalf = g.hair.band * skullR;
  const lineAt = (x, y) => {
    const dx = x + 0.5 - cx;
    let line = hairLine(y);
    if (g.hair.side)
      line += g.hair.side * (dx / Math.max(bandHalf, 1)) * 3.0 * hz * hairT(y);
    return line;
  };
  const isHair = (x, y, z) => {
    if (z < skullLo) return false;
    if (Math.abs(x + 0.5 - cx) > bandHalf) return false;
    return z >= lineAt(x, y);
  };
  // SCALP SHOWING THROUGH: the cap's version of thin hair. The cap is the
  // skull's own surface -- a see-through voxel there would open onto the
  // inside of the head -- so a thin patch is painted as scalp instead, in
  // SKIN_LIGHT, which hairMass still reads as scalp to root on. A hashed
  // speckle whose density falls off with distance from the thin region, so
  // it reads as a gradient and not a stencil. Every weight is 0 by default.
  const hg = g.hair;
  const thinCap = hg.hairlineSoft > 0 || hg.topThin > 0 || hg.sideThin > 0;
  const scalpShows = (x, y, z) => {
    const ax = Math.abs(x + 0.5 - cx) / Math.max(skullR, 1);
    let p = 0;
    if (hg.hairlineSoft > 0) {
      // Ragged on the edge itself (up to half the edge row goes), fading out
      // over three and a half rows above it.
      const dz = z + 0.5 - lineAt(x, y);
      p = Math.max(p, hg.hairlineSoft * 0.85 * clamp(1 - dz / (3.5 * hz), 0, 1));
      if (Math.abs(x + 0.5 - cx) > bandHalf - 1.5)
        p = Math.max(p, hg.hairlineSoft * 0.5);
    }
    if (hg.topThin > 0) {
      const u = clamp((z + 0.5 - sz * 0.62) / (sz * 0.38), 0, 1);
      p = Math.max(p, hg.topThin * 0.75 * u * (1 - 0.5 * clamp(ax, 0, 1)));
    }
    if (hg.sideThin > 0) {
      const w = clamp((ax - 0.55) / 0.4, 0, 1) * (z < sz * 0.8 ? 1 : 0.4);
      p = Math.max(p, hg.sideThin * 0.6 * w);
    }
    return p > 0 && hashN(x, y, z, 71) / 4294967296 < p;
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
        if (isHair(x, y, z))
          col = thinCap && scalpShows(x, y, z) ? ART.SKIN_LIGHT
              : back ? ART.HAIR_SHADE : ART.HAIR;
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
  const f = g.face;
  const earSet = new Set();                   // out indices: ears and points
  const earRows = f.earSize <= 0 ? [eyeZ]
                : f.earSize >= 2 ? [eyeZ - 2, eyeZ - 1, eyeZ] : [eyeZ - 1, eyeZ];
  const earTop = [];                          // [x, z] of each ear's top cell
  for (const z of earRows) {
    if (z < 0) continue;
    let loX = -1, hiX = -1;
    for (let x = 0; x < sx; x++)
      if (has.has(x + ',' + earY + ',' + z)) { if (loX < 0) loX = x; hiX = x; }
    if (loX < 0) continue;                    // no skull on this row: no ears
    if (loX - 1 >= 0) {
      earSet.add(out.length); out.push([loX - 1, earY, z, ART.SKIN_SHADE]);
      earTop[0] = [loX - 1, z];
    }
    if (hiX + 1 < sx) {
      earSet.add(out.length); out.push([hiX + 1, earY, z, ART.SKIN_SHADE]);
      earTop[1] = [hiX + 1, z];
    }
  }
  // AN ELF POINT: two cells straight up off the top of each ear, only where
  // the skull left the space empty -- stacked on the ear, so it is connected
  // by construction (the note above is why that matters).
  if (f.earPoint)
    for (const t of earTop) {
      if (!t) continue;
      for (let dz = 1; dz <= 2; dz++) {
        const z = t[1] + dz, k = t[0] + ',' + earY + ',' + z;
        if (z >= sz || has.has(k)) break;
        has.add(k);
        earSet.add(out.length);
        out.push([t[0], earY, z, ART.SKIN_SHADE]);
      }
    }

  // ---- the face's own paint helpers ----------------------------------------
  // The FRONTMOST cell of column x on row z, by index, or -1. A scan rather
  // than a cached map because the nose and brow ridge below add geometry
  // between paints; a head is ~1000 cells, so it costs nothing.
  const frontAt = (x, z) => {
    let best = -1;
    for (let i = 0; i < out.length; i++) {
      const c = out[i];
      if (c[0] === x && c[2] === z && (best < 0 || c[1] > out[best][1])) best = i;
    }
    return best;
  };
  const SKIN = new Set([ART.SKIN_BASE, ART.SKIN_SHADE, ART.SKIN_LIGHT]);
  // Paint the front of (x, z). `skinOnly` leaves hair, eyes and beard alone,
  // which is how a mark ends at a hairline and hides under a moustache.
  const paintAt = (x, z, col, skinOnly) => {
    if (x < 0 || x >= sx || z < 0 || z >= sz) return false;
    const i = frontAt(x, z);
    if (i < 0 || (skinOnly && !SKIN.has(out[i][3]))) return false;
    out[i] = [out[i][0], out[i][1], out[i][2], col];
    return true;
  };
  // Stand a new cell one proud of the front of (x, z), if the box has room.
  const proud = (x, z, col) => {
    const i = frontAt(x, z);
    if (i < 0) return -1;
    const y = out[i][1] + 1, k = x + ',' + y + ',' + z;
    if (y >= sy || has.has(k)) return -1;
    has.add(k);
    out.push([x, y, z, col]);
    return out.length - 1;
  };
  // Symmetric rounding: Math.round takes -0.5 to 0 but 0.5 to 1, which would
  // make every mirrored left/right gene lopsided by a cell.
  const sround = v => Math.sign(v) * Math.floor(Math.abs(v) + 0.5);
  const rnd01 = (...v) => hashN(...v) / 4294967296;

  // The eyes' columns, needed by everything placed relative to them.
  const eyeDx = g.face.eyeDx * (sx / DEF.head.size[0]);
  const eyeCols = new Set();
  for (let k = 0; k < g.face.eyeCols; k++) {
    eyeCols.add(Math.trunc(cx - eyeDx) + k);
    eyeCols.add(Math.trunc(cx + eyeDx) - k);
  }
  // Per side: dir is the way OUT (toward the ear); outer/inner are the eye's
  // outermost and innermost columns.
  const eyeSides = [-1, 1].map(dir => {
    const E = [...eyeCols].filter(c => (dir < 0 ? c < cx : c >= cx));
    if (!E.length) return null;
    const outer = dir < 0 ? Math.min(...E) : Math.max(...E);
    const inner = dir < 0 ? Math.max(...E) : Math.min(...E);
    return { dir, outer, inner, E };
  }).filter(Boolean);

  // ---- nose: geometry, one cell proud, stopping above the mouth ------------
  // One cell is all the box allows: at the eye row the face already sits a
  // cell short of the head's front face, so a bulbous tip has nowhere to go.
  const mouthZ = eyeZ - g.face.mouthDrop;
  const noseSet = new Set();
  const noseRows = Math.min(f.nose, g.face.mouthDrop - 1);
  const noseCols = [];
  for (let k = 0; k < 2 * f.noseWidth; k++)
    noseCols.push(Math.trunc(cx) - f.noseWidth + k);
  for (let r = 1; r <= noseRows; r++)
    for (const x of noseCols) {
      const i = proud(x, eyeZ - r, r === noseRows ? ART.SKIN_LIGHT
                                                  : ART.SKIN_BASE);
      if (i >= 0) noseSet.add(i);
    }

  // ---- eyebrow cells: computed now (the ridge is geometry), painted last ----
  const browCells = [];                       // [x, z, lowestRow]
  if (f.brow) {
    const base = eyeZ + 2 + f.browHeight;
    for (const s of eyeSides) {
      // Outer to inner. The inner reach is NOT clamped at the midline: the
      // drawn brow already stepped one cell in from each eye column, and
      // brows meeting in the middle is the unibrow the slider promises.
      const cols = [];
      for (let c = s.outer + s.dir * f.browOuter; ; c -= s.dir) {
        cols.push(c);
        if (c === s.inner - s.dir * f.browInner) break;
      }
      cols.forEach((x, n) => {
        const t = cols.length > 1 ? n / (cols.length - 1) : 0.5;
        const off = sround(f.browTilt * t * 1.5) +
                    sround(f.browArch * (1 - Math.abs(2 * t - 1)) * 1.5);
        const z0 = Math.min(sz - 1, Math.max(eyeZ + 1, base + off));
        for (let r = 0; r < f.browThick; r++)
          if (z0 + r < sz) browCells.push([x, z0 + r, r === 0]);
      });
    }
    if (f.browProud)
      for (const [x, z, low] of browCells) if (low) proud(x, z, ART.SKIN_BASE);
  }

  // ---- skin marks: painted first, so beard, eyes, mouth and brows lie over
  const scarKeys = new Set();
  const shade = (x, z) => paintAt(x, z, ART.SKIN_SHADE, true);
  const light = (x, z) => paintAt(x, z, ART.SKIN_LIGHT, true);
  if (Math.abs(f.cheeks) >= 0.25) {
    const two = Math.abs(f.cheeks) >= 0.6;
    const rows = f.cheeks > 0 ? [eyeZ - 1, eyeZ - 2] : [eyeZ - 2, eyeZ - 3];
    for (const s of eyeSides)
      for (const x of [s.outer, s.outer + s.dir])
        for (const z of two ? rows : [rows[0]])
          (f.cheeks > 0 ? light : shade)(x, z);
  }
  if (f.freckles > 0)
    for (let z = eyeZ - 2; z <= eyeZ - 1; z++)
      for (let x = 0; x < sx; x++)
        if (Math.abs(x + 0.5 - cx) < 3.6 &&
            rnd01(x, z, 151) < f.freckles * 0.55) shade(x, z);
  if (noseRows > 0) {                         // nostril shadow beside the tip
    shade(noseCols[0] - 1, eyeZ - noseRows);
    shade(noseCols[noseCols.length - 1] + 1, eyeZ - noseRows);
  }
  if (f.wrinkles >= 0.2)                      // crow's feet
    for (const s of eyeSides)
      shade(s.outer + s.dir * (f.eyeWhites ? 2 : 1), eyeZ - 1);
  if (f.wrinkles >= 0.45)                     // a forehead line
    for (let x = Math.trunc(cx) - 2; x <= Math.trunc(cx) + 1; x++)
      shade(x, eyeZ + 3 + Math.max(0, f.browHeight) +
               (f.brow ? f.browThick - 1 : 0));
  if (f.scar !== 0) {
    const s = eyeSides.find(e => e.dir === Math.sign(f.scar));
    if (s) {
      const n = Math.max(2, Math.round(Math.abs(f.scar) * 6));
      const z0 = eyeZ + 2 + (n >= 4 ? 1 : 0);
      for (let i = 0; i < n; i++) {
        // Through hair too: a scar is a bald line in a brow or a cap. The
        // eye it crosses is painted after, so the eye survives it.
        const x = s.inner + s.dir * (Math.floor(i / 2) - 1), z = z0 - i;
        if (paintAt(x, z, ART.SKIN_LIGHT)) scarKeys.add(x + ',' + z);
      }
    }
  }

  // THE BEARD: paint on the lower face in the hair shade, laid down before the
  // mouth is repainted over it (so a moustache never swallows the mouth).
  // The amount is its REACH, not its density: a moustache and goatee, then
  // the jaw, then the cheeks up to the ears. Solid, with a broken outer edge.
  // A density ramp (stubble) was tried first and read as random blotches --
  // one painted cell is a 2x2 block once shipped, which is not a stubble grain.
  // Front half only, and never above the cheekbone row.
  //
  // REGIONS (2026-09-25), so the styles can be built from parts: the CENTRE
  // strip is the moustache at and above the mouth row and the chin below it,
  // each with its own width; the SIDES (past 2.2 cells out) are the jaw up to
  // `jawTop` and the cheeks up to `cheekTop`. With every modifier at 0 the
  // union is exactly the old rule -- dx <= reach and (centre, jaw row or a
  // full beard) -- so no existing face moved. `beardDensity` brings the
  // stubble back as a choice for the styles that want it, not as a default.
  const beardTone = f.beardTone === 'hair' ? ART.HAIR : ART.HAIR_SHADE;
  if (f.beard > 0) {
    const bd = f.beard;
    const topZ = eyeZ - 2;
    const reach = 1.5 + bd * (sx * 0.5) * (1 + f.beardWidth * 0.6);
    const jawTop = skullLo + 1 + Math.round(bd * 3) + f.beardJaw;
    const auto = bd >= 0.75 ? 1 : 0;
    const cheekF = f.beardCheek >= 0 ? Math.max(auto, f.beardCheek)
                                     : auto * (1 + f.beardCheek);
    const cheekTop = jawTop + (topZ - jawTop) * cheekF;
    const lipHalf = f.beardLip >= 0 ? 2.2 + f.beardLip * 2
                                    : 2.2 * (1 + f.beardLip);
    const chinHalf = f.beardChin >= 0 ? 2.2 + f.beardChin * 1.5
                                      : 2.2 * (1 + f.beardChin);
    const lipW = f.beardLip > 0 ? Math.max(reach, lipHalf)
                                : Math.min(reach, lipHalf);
    const chinW = f.beardChin > 0 ? Math.max(reach, chinHalf)
                                  : Math.min(reach, chinHalf);
    const mouthHalf = g.face.mouthWidth / 2;
    const droop = f.beardLip >= 0.75;
    const keep = 1 - f.beardRagged;
    for (let i = 0; i < out.length; i++) {
      const [x, y, z, c] = out[i];
      if (z > topZ || z < skullLo) continue;
      if (c === ART.EYE) continue;
      if (y + 0.5 < hcy - 0.5) continue;        // the back of the head
      if (noseSet.has(i) || earSet.has(i)) continue;
      const dx = Math.abs(x + 0.5 - cx);
      const jawRow = z <= jawTop;
      const lipRow = z >= mouthZ;
      const cW = lipRow ? lipW : chinW;
      const centre = dx <= cW;
      const side = dx > 2.2 && dx <= reach && (jawRow || z <= cheekTop);
      const hang = droop && !lipRow && dx > mouthHalf && dx <= lipW;
      if (!centre && !side && !hang) continue;
      let edge = dx > reach - 1 || (!jawRow && dx > 1.2);
      if (centre && !side && (lipRow ? f.beardLip : f.beardChin) !== 0 &&
          dx > cW - 1) edge = true;
      if (f.beardDensity < 1 && rnd01(x, y, z, 131) >= f.beardDensity) continue;
      if (!edge || hashN(x, y, z, 97) / 4294967296 < keep)
        out[i] = [x, y, z, beardTone];
    }
  }
  // SIDEBURNS: the outermost two columns just in front of the ear, from the
  // hairline down toward the jaw. Independent of `beard`, so they exist on a
  // clean-shaven face.
  if (f.beardSide > 0) {
    const zTop = eyeZ + 1;
    const zBot = Math.round(zTop - f.beardSide * (zTop - skullLo));
    const band = (i, y) => !earSet.has(i) && y >= earY && y <= earY + 2;
    for (let z = zBot; z <= zTop; z++) {
      let maxDx = -1;
      for (let i = 0; i < out.length; i++)
        if (out[i][2] === z && band(i, out[i][1]))
          maxDx = Math.max(maxDx, Math.abs(out[i][0] + 0.5 - cx));
      if (maxDx < 0) continue;
      for (let i = 0; i < out.length; i++) {
        const [x, y, zz, c] = out[i];
        if (zz !== z || !band(i, y) || !SKIN.has(c)) continue;
        if (Math.abs(x + 0.5 - cx) < maxDx - 1) continue;
        if (f.beardDensity < 1 && rnd01(x, y, z, 131) >= f.beardDensity) continue;
        out[i] = [x, y, z, beardTone];
      }
    }
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
  paintFront(eyeCols, eyeZ, ART.EYE);
  if (f.eyeWhites)
    for (const s of eyeSides) light(s.outer + s.dir, eyeZ);
  if (f.eyeBags > 0.2)
    for (const s of eyeSides) {
      for (const x of s.E) shade(x, eyeZ - 1);
      if (f.eyeBags > 0.6) shade(s.outer + s.dir, eyeZ - 1);
    }

  // ---- the mouth ------------------------------------------------------------
  const mw = g.face.mouthWidth;
  const mcols = [];
  for (let k = 0; k < mw; k++)
    mcols.push(Math.trunc(cx) - 1 + k - ((mw - 2) >> 1) + f.mouthShift);
  const mtone = f.mouthTone === 'dark' ? ART.EYE : ART.SKIN_SHADE;
  const mz = mouthZ;
  const curveUp = Math.sign(f.mouthCurve);
  const moff = mcols.map((_, n) => {
    const t = mw > 1 ? n / (mw - 1) : 0.5;
    let off = mw > 1 ? sround(Math.abs(f.mouthSkew) *
                              (f.mouthSkew > 0 ? t : 1 - t)) : 0;
    if (Math.abs(f.mouthCurve) >= 0.7 && mw >= 3 && (n === 0 || n === mw - 1))
      off += curveUp;
    return off;
  });
  mcols.forEach((x, n) => paintAt(x, mz + moff[n], mtone));
  if (Math.abs(f.mouthCurve) >= 0.3) {
    paintAt(mcols[0] - 1, mz + moff[0] + curveUp, mtone);
    paintAt(mcols[mw - 1] + 1, mz + moff[mw - 1] + curveUp, mtone);
  }
  for (let r = 1; r <= f.mouthOpen; r++)
    mcols.forEach((x, n) => paintAt(x, mz + moff[n] - r, ART.EYE));
  if (f.lips >= 1)
    for (const x of mcols) light(x, mz + Math.min(...moff) - f.mouthOpen - 1);
  if (f.lips >= 2)
    for (const x of mcols) light(x, mz + Math.max(...moff) + 1);
  if (f.wrinkles >= 0.7) {                    // nose-to-mouth lines
    shade(mcols[0] - 1, mz + moff[0] + 1);
    shade(mcols[mw - 1] + 1, mz + moff[mw - 1] + 1);
  }

  // ---- brows, last: over the hair cap and any mark except the scar ---------
  if (browCells.length) {
    const tone = { hair: ART.HAIR, dark: ART.EYE, light: ART.SKIN_LIGHT }[
      f.browTone] ?? ART.HAIR_SHADE;
    for (const [x, z] of browCells) {
      if (scarKeys.has(x + ',' + z)) continue;
      if (f.browSparse > 0 && rnd01(x, z, 163) < f.browSparse) continue;
      paintAt(x, z, tone);
    }
  }
  return flipY(size, out);
}

function upperArmVox(g, size) {
  return flipY(size, armTube(g, 'upper', size,
    (x, y, z, sx, sy) => shadeBack(y, sy * 0.5, ART.SKIN_BASE, ART.SKIN_SHADE)));
}

function foreArmVox(g, size) {
  return flipY(size, armTube(g, 'fore', size,
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

// =============================================================================
// THE HAIR MASS
//
// Everything hair does that is not paint on the skull: volume standing off the
// scalp, a mohawk crest, spikes, buns, a ponytail or two, and loose hair that
// hangs and drapes over the back. It cannot live in the head brick. The head's
// box is the skull's box, and every worn helm and hood is FIT by resampling to
// that box (mob.cpp cover.fitBox), so growing it to hold an afro would stretch
// every hat in the game. So the mass gets its own parts:
//
//   `hair` rides the HEAD (joint fixed) -- everything at or above the neck;
//   `mane` rides the TORSO (joint ball + spring) -- everything below it.
//
// The split is at the head's own joint. A curtain hung off the head would turn
// with it, and the head turns up to sixty degrees to follow the camera: hair
// down the back would swing through the shoulders. Below the neck, real hair
// lies on the back and stays there.
//
// BUILT ON THE SHIPPED LATTICE, after the upscale, because hair is the one
// thing on the figure that wants to be fine: a strand is one shipped voxel.
// The body's own cells are the obstacle field, so hair is never placed inside
// flesh, and it DRAPES: a strand that meets the back slides outward over it
// rather than stopping or passing through.
//
// ALWAYS AT LEAST ONE CELL OF SHELL when there is any mass at all. It is what
// makes everything rooted on the scalp (a bun, a spike, a tail's knot) one
// connected piece -- the engine splits a disconnected limb into fragments on
// its first carve -- and a real head of hair has thickness anyway.
//
// THE ENDS THIN. The last `wisp` share of a hanging strand steps down through
// three see-through art slots (ART.HAIR_WISP1..3), so long hair ends in wisps
// instead of a sawn-off plank. microbody.wgsl draws them as screen-door
// coverage; the collider does not care.
//
// Returns [{name, parent, cells:[[X,Y,Z,slot]] in SCENE shipped coords}] --
// one entry per connected piece, named hair, hair.2, mane, mane.2 ... in
// order of size, so twin tails are two limbs rather than one limb in two
// pieces. Empty when the style has no mass, which is every character that
// existed before this.
// =============================================================================

/** Longest a hair part may be, shipped cells. The collider resolution is
 *  picked off the LARGEST model (mob.cpp), 120 collider units at 1:1; a mane
 *  that outgrew the torso past that would coarsen the whole body's collider. */
const HAIR_MAX_EXTENT = 110;

export function hairWants(h) {
  return h.volume > 0 || h.crest > 0 || h.spikes > 0 || !!h.buns ||
         h.drop > 0 || h.tail > 0 || h.bangs > 0;
}

function hairMass(g, parts, extra = {}) {
  const h = g.hair;
  // THREE REASONS TO BUILD: the mass proper, the short-hair layer and a
  // hanging beard. The last two root on skin as well as scalp, so a bald
  // head can carry either; only the first grows the shell over the cap.
  const massOn = hairWants(h);
  const fuzzOn = h.fuzz > 0 &&
                 (h.fuzzTop > 0 || h.fuzzSide > 0 || h.fuzzBack > 0);
  const beardOn = g.face.beardLen > 0 && g.face.beard > 0;
  // A FOURTH: the sylvan crown (sylvan.js crownExtras), which roots on bare
  // bark as happily as on leaf hair.
  const crownOn = g.body.race === 'sylvan' && SY.crownWants(g);
  if (!massOn && !fuzzOn && !beardOn && !crownOn) return [];
  const U = SKIN_UPSCALE;
  const K = (x, y, z) => ((z + 512) * 2048 + (y + 1024)) * 2048 + (x + 1024);
  const P = {};
  for (const p of parts) P[p.name] = p;
  const head = P.head;
  const body = new Set();
  for (const p of parts)
    for (const [x, y, z] of p.cells) body.add(K(p.mn[0] + x, p.mn[1] + y, p.mn[2] + z));

  // The skull: its hair cells (the scalp the mass roots on), its skin cells
  // (which hair must never touch but at the hairline), and a centre taken
  // over the skull rather than the neck stub.
  const hz0 = head.mn[2], hz1 = head.mn[2] + head.size[2];
  const scalp = [], skin = new Set();
  let c0 = [1e9, 1e9, 1e9], c1 = [-1e9, -1e9, -1e9];
  for (const [x, y, z, c] of head.cells) {
    const X = head.mn[0] + x, Y = head.mn[1] + y, Z = head.mn[2] + z;
    if (Z >= hz0 + (hz1 - hz0) * 0.25) {
      c0 = [Math.min(c0[0], X), Math.min(c0[1], Y), Math.min(c0[2], Z)];
      c1 = [Math.max(c1[0], X), Math.max(c1[1], Y), Math.max(c1[2], Z)];
    }
    // SKIN_LIGHT on the skull is scalp showing through thin hair (headVox's
    // scalpShows): still scalp, so the mass roots there and is not pushed off
    // it the way it is pushed off the face.
    // (On a sylvan the cap's leaves come in more colours than two: SY.SLOTS.)
    if (c === ART.HAIR || c === ART.HAIR_SHADE || c === ART.SKIN_LIGHT ||
        c === ART.LEAF2 || c === ART.LEAF3 || c === ART.LEAF4)
      scalp.push([X, Y, Z]);
    else skin.add(K(X, Y, Z));
  }
  if (!scalp.length && !fuzzOn && !beardOn && !crownOn) return [];  // bald: no roots
  const C = [(c0[0] + c1[0] + 1) / 2, (c0[1] + c1[1] + 1) / 2,
             (c0[2] + c1[2] + 1) / 2];
  const halfW = (c1[0] - c0[0] + 1) / 2, halfD = (c1[1] - c0[1] + 1) / 2;
  const skullH = c1[2] - c0[2] + 1;
  const rnd = (...v) => hashN(...v) / 4294967296;
  const top = p => p.mn[2] + p.size[2];
  const hipsTop = top(P.hips);
  const splitZ = top(P.torso) - U;              // the head's joint (buildSidecar)
  const shadeOf = Y => (Y > C[1] + 1 ? ART.HAIR_SHADE : ART.HAIR);

  // THE BACK, per (column x, row z): the largest y the trunk and head reach
  // there. Arms are left out -- they swing, so they are something to avoid,
  // never something to lie on.
  const backMap = new Map();
  for (const nm of ['hips', 'torso', 'head']) {
    const p = P[nm];
    for (const [x, y, z] of p.cells) {
      const k = (p.mn[0] + x) * 4096 + (p.mn[2] + z), Y = p.mn[1] + y;
      if (!backMap.has(k) || Y > backMap.get(k)) backMap.set(k, Y);
    }
  }
  const backOf = (X, Z) => backMap.has(X * 4096 + Z) ? backMap.get(X * 4096 + Z)
                                                       : null;
  const tailCells = new Set();                  // keys: always ride the head

  const mass = new Map();                       // key -> [X, Y, Z, slot]
  const add = (X, Y, Z, slot) => {
    const k = K(X, Y, Z);
    if (body.has(k) || mass.has(k)) return false;
    mass.set(k, [X, Y, Z, slot]);
    return true;
  };
  const free = (X, Y, Z) => !body.has(K(X, Y, Z));
  const touchesSkin = (X, Y, Z) =>
    skin.has(K(X + 1, Y, Z)) || skin.has(K(X - 1, Y, Z)) ||
    skin.has(K(X, Y + 1, Z)) || skin.has(K(X, Y - 1, Z)) ||
    skin.has(K(X, Y, Z + 1)) || skin.has(K(X, Y, Z - 1));

  // ---- the shell: volume off the scalp ------------------------------------
  // A BFS that carries each cell's SEED (the scalp cell it grew from), so the
  // distance test is to the scalp, not along the path -- a cheap Euclidean
  // distance transform. Growth must point AWAY from the skull centre, never
  // touches skin (the face and ears stay clear) and never drops below a FRONT
  // seed, or the fringe grows a visor over the eyes.
  const V = Math.max(1, h.volume * U);
  const shell = [];
  if (massOn) {
    const seen = new Set(scalp.map(([X, Y, Z]) => K(X, Y, Z)));
    const q = scalp.map(([X, Y, Z]) => [X, Y, Z, X, Y, Z]);
    for (let qi = 0; qi < q.length; qi++) {
      const [X, Y, Z, sx, sy, sz] = q[qi];
      const ox = sx - C[0], oy = sy - C[1], oz = sz - C[2];
      const front = sy < C[1] - halfD * 0.3;
      for (let dz = -1; dz <= 1; dz++)
        for (let dy = -1; dy <= 1; dy++)
          for (let dx = -1; dx <= 1; dx++) {
            const x = X + dx, y = Y + dy, z = Z + dz;
            const k = K(x, y, z);
            if (seen.has(k) || body.has(k)) continue;
            const ex = x - sx, ey = y - sy, ez = z - sz;
            const d2 = ex * ex + ey * ey + ez * ez;
            if (d2 > (V + 0.45) * (V + 0.45)) continue;
            if (ex * ox + ey * oy + ez * oz <= 0) continue;
            if (front && z < sz) continue;
            if (touchesSkin(x, y, z)) continue;
            seen.add(k);
            const d = Math.sqrt(d2);
            // The outermost layer of a big volume is FLUFF: a broken edge of
            // the lightest wisp rather than a hard shell.
            const fluff = h.volume >= 3 && h.wisp > 0 && d > V - 1.2 &&
                          rnd(x, y, z, 11) < 0.55;
            add(x, y, z, fluff ? ART.HAIR_WISP1 : shadeOf(y));
            shell.push([x, y, z]);
            q.push([x, y, z, sx, sy, sz]);
          }
    }
  }
  const outer = scalp.concat(shell);

  // ---- a crest: the mohawk fin ---------------------------------------------
  // Rays out of the skull centre through every hair cell in a strip along the
  // midline, upper half only. Many seeds per ray line make the fin solid; the
  // `ragged` gene knocks a little off each ray so the top edge is not a ruler.
  if (h.crest > 0) {
    const H = h.crest * U;
    for (const [X, Y, Z] of outer) {
      // Upper skull only, and the ray LEANS UP: a crest seeded down at the
      // brow and fired radially fanned forward over the face.
      if (Math.abs(X + 0.5 - C[0]) > 1.6 || Z < C[2] + skullH * 0.12) continue;
      const r0 = [X + 0.5 - C[0], Y + 0.5 - C[1], Z + 0.5 - C[2]];
      const rn = Math.hypot(...r0) || 1;
      const d = [r0[0] / rn, r0[1] / rn * 0.6, r0[2] / rn + 0.8];
      const n = Math.hypot(...d);
      const len = Math.round(H * (1 - h.ragged * 0.35 * rnd(X, Y, Z, 23)));
      for (let t = 1; t <= len; t++) {
        const x = Math.floor(X + 0.5 + d[0] / n * t);
        const y = Math.floor(Y + 0.5 + d[1] / n * t);
        const z = Math.floor(Z + 0.5 + d[2] / n * t);
        add(x, y, z, t >= len - 1 && h.wisp > 0 ? ART.HAIR_WISP1 : shadeOf(y));
      }
    }
  }

  // ---- spikes ----------------------------------------------------------------
  // Points picked off the outer hair surface by hash, kept apart so they read
  // as separate spikes, each a cone that leans UP from its radial direction.
  if (h.spikes > 0) {
    const L = h.spikes * U;
    const cand = outer
      .filter(([X, Y, Z]) => Z >= C[2] + skullH * 0.08 &&
                             Y > C[1] - halfD * 0.75)
      .map(c => [rnd(c[0], c[1], c[2], 37), c]).sort((a, b) => a[0] - b[0]);
    const picked = [];
    const sep2 = Math.max(16, (L * 0.6) ** 2);
    for (const [, c] of cand) {
      if (picked.length >= 22) break;
      if (picked.some(p => (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 +
                           (p[2] - c[2]) ** 2 < sep2)) continue;
      picked.push(c);
    }
    for (const [X, Y, Z] of picked) {
      const r = [X + 0.5 - C[0], Y + 0.5 - C[1], Z + 0.5 - C[2]];
      const rn = Math.hypot(...r) || 1;
      const d = [r[0] / rn, r[1] / rn, r[2] / rn + 0.9];
      const dn = Math.hypot(...d);
      const len = L * (1 - h.ragged * 0.4 * rnd(X, Y, Z, 41));
      for (let t = 0; t <= len; t += 0.5) {
        const rad = 1.25 * (1 - t / len);
        const px = X + 0.5 + d[0] / dn * t, py = Y + 0.5 + d[1] / dn * t,
              pz = Z + 0.5 + d[2] / dn * t;
        const tip = t > len - 1.5;
        for (let oz = -1; oz <= 1; oz++)
          for (let oy = -1; oy <= 1; oy++)
            for (let ox = -1; ox <= 1; ox++) {
              if (ox * ox + oy * oy + oz * oz > rad * rad + 0.3) continue;
              const x = Math.floor(px) + ox, y = Math.floor(py) + oy,
                    z = Math.floor(pz) + oz;
              add(x, y, z, tip && h.wisp > 0 ? ART.HAIR_WISP1 : shadeOf(y));
            }
      }
    }
  }

  // Where a ray from the skull centre leaves the head and its hair: the root
  // of a bun or a tail. Marches until two consecutive free, hairless cells.
  const surfaceAlong = dir => {
    const n = Math.hypot(...dir);
    const u = dir.map(v => v / n);
    let last = null;
    for (let t = 0; t < 80; t += 0.5) {
      const X = Math.floor(C[0] + u[0] * t), Y = Math.floor(C[1] + u[1] * t),
            Z = Math.floor(C[2] + u[2] * t);
      const k = K(X, Y, Z);
      if (body.has(k) || mass.has(k)) last = [X, Y, Z];
      else if (last && t > 2) return { at: last, u };
    }
    return { at: last || C.map(Math.floor), u };
  };

  // ---- buns ------------------------------------------------------------------
  if (h.buns) {
    const R = Math.max(2.5, halfW * 0.42);
    for (const sgn of [-1, 1]) {
      const { at, u } = surfaceAlong([sgn * 0.55, 0.2, 0.85]);
      const c = [at[0] + 0.5 + u[0] * (R - 1), at[1] + 0.5 + u[1] * (R - 1),
                 at[2] + 0.5 + u[2] * (R - 1)];
      const r = Math.ceil(R);
      for (let dz = -r; dz <= r; dz++)
        for (let dy = -r; dy <= r; dy++)
          for (let dx = -r; dx <= r; dx++) {
            const x = Math.floor(c[0]) + dx, y = Math.floor(c[1]) + dy,
                  z = Math.floor(c[2]) + dz;
            const e = [x + 0.5 - c[0], y + 0.5 - c[1], z + 0.5 - c[2]];
            if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] > R * R) continue;
            // A wound knot, not a ball: a spiral band of shade round it.
            const band = ((Math.atan2(e[1], e[0]) / (2 * Math.PI) + 1) * 3 +
                          e[2] * 0.35) % 1 < 0.33;
            add(x, y, z, band ? ART.HAIR_SHADE : ART.HAIR);
          }
    }
  }

  // The back of the body at height Z around column X (the largest Y any
  // body cell reaches there), for draping a tail over it.
  const backAt = (X, Z, r) => {
    let b = -1e9;
    for (let x = Math.floor(X - r); x <= Math.ceil(X + r); x++) {
      const y = backOf(x, Z);
      if (y !== null && y > b) b = y;
    }
    return b;
  };
  // How far a hanging thing reaches, from a start height, on the one scale
  // `drop` and `tail` share: 0 nothing, 1 the waist. Clamped so no part can
  // outgrow HAIR_MAX_EXTENT.
  const reach = (z0, frac) => {
    const want = Math.round(z0 - frac * (z0 - hipsTop));
    return Math.max(want, z0 - HAIR_MAX_EXTENT + 8);
  };
  // Tier of a hanging cell by its distance from its strand's end.
  const tierOf = (fromEnd, len) => {
    const wl = Math.round(h.wisp * len);
    if (wl < 1 || fromEnd >= wl) return 0;
    const f = fromEnd / wl;
    return f < 1 / 3 ? 3 : f < 2 / 3 ? 2 : 1;
  };
  const WISP = [0, ART.HAIR_WISP1, ART.HAIR_WISP2, ART.HAIR_WISP3];

  // ---- tails: gathered, tied, hanging ---------------------------------------
  if (h.tail > 0) {
    const roots = h.twin
      ? [[-1, 0.55, -0.15], [1, 0.55, -0.15]]
      : [[0, 1, 0.25]];
    for (const dir of roots) {
      const { at, u } = surfaceAlong(dir);
      const cx = at[0] + 0.5 + u[0] * 1.5;
      let cy = at[1] + 0.5 + u[1] * 1.5;
      const z0 = at[2];
      const zEnd = reach(z0, h.tail);
      const len = z0 - zEnd;
      if (len < 2) continue;
      const r0 = 1.4, r1 = h.braid ? 2.0 : 2.6;
      for (let z = z0; z >= zEnd; z--) {
        const t = (z0 - z) / len;
        let r = t < 0.06 ? r0 : t < 0.25
          ? r0 + (r1 - r0) * (t - 0.06) / 0.19
          : r1 - (r1 - 1.0) * (t - 0.25) / 0.75;
        const phase = z0 - z;
        if (h.braid) r *= 1 + 0.18 * Math.cos(phase * 2 * Math.PI / 4);
        // DRAPE. Below the skull a tail falls in toward the back a cell every
        // other row until it lies on it, and never passes into it. Hanging
        // straight down from the back of the skull left it floating a hand's
        // breadth behind the shoulder blades on every rig, because the
        // skull sits behind the torso's back plane.
        const b = backAt(cx, z, r);
        if (b > -1e9) {
          const rest = b + 0.5 + r;
          if (cy < rest) cy = rest;
          else if (z < c0[2] && (z0 - z) % 2 === 0) cy = Math.max(rest, cy - 1);
        }
        const tier = tierOf(z - zEnd, len);
        const rr = Math.ceil(r);
        for (let dy = -rr; dy <= rr; dy++)
          for (let dx = -rr; dx <= rr; dx++) {
            const x = Math.floor(cx) + dx, y = Math.floor(cy) + dy;
            const ex = x + 0.5 - cx, ey = y + 0.5 - cy;
            if (ex * ex + ey * ey > r * r + 0.25) continue;
            let slot;
            if (tier) slot = WISP[tier];
            else if (phase < 2) slot = ART.CLOTH_SHADE;         // the tie
            else if (h.braid) {
              // Three strands crossing: which one is on top turns with the
              // angle and advances a third of a turn every plait.
              const a = (Math.atan2(ey, ex) / (2 * Math.PI) + 1) * 3;
              slot = Math.floor(a + phase / 1.34) % 3 === 0
                ? ART.HAIR_SHADE : ART.HAIR;
            } else slot = rnd(x, y, 53) < 0.22 ? ART.HAIR_SHADE : ART.HAIR;
            if (add(x, y, z, slot)) tailCells.add(K(x, y, z));
          }
      }
    }
  }

  // ---- loose hair: the curtain ----------------------------------------------
  // Falls from the underside of the outer hair (scalp + shell) at the back and
  // the sides. A column only falls if nothing of the head is under its root --
  // that is what keeps it off the face and out of the skull. It then drops
  // straight; where it meets the body a BACK strand slides outward over it
  // (hair lying on the back and shoulders) and a SIDE strand stops (hair
  // resting on the shoulder, the way it frames a face).
  if (h.drop > 0) {
    const headLow = new Map();                  // column -> lowest head cell
    for (const [x, y, z] of head.cells) {
      const k = (head.mn[0] + x) * 4096 + (head.mn[1] + y);
      const Z = head.mn[2] + z;
      if (!headLow.has(k) || Z < headLow.get(k)) headLow.set(k, Z);
    }
    const root = new Map();                     // column -> [X, Y, lowest Z]
    for (const [X, Y, Z] of outer) {
      if (Y < C[1] - halfD * 0.3) continue;     // in front of the ears
      const k = X * 4096 + Y;
      if (!root.has(k) || Z < root.get(k)[2]) root.set(k, [X, Y, Z]);
    }
    let zNape = 1e9;
    for (const [, Y, Z] of scalp) if (Y > C[1] && Z < zNape) zNape = Z;
    if (zNape === 1e9) zNape = Math.round(C[2]);
    const zBase = reach(zNape, h.drop);
    const len0 = zNape - zBase;
    for (const [k, [X, Y, z0]] of root) {
      if (headLow.has(k) && headLow.get(k) < z0) continue;   // skull below
      // A soft U: the edges a little shorter than the middle, and `ragged`
      // lifting each strand's end by its own amount.
      const edge = Math.min(1, Math.abs(X + 0.5 - C[0]) / Math.max(halfW, 1));
      const zEnd = Math.round(zBase + edge * edge * 0.18 * len0 +
                              rnd(X, Y, 61) * h.ragged * 0.3 * len0);
      if (zEnd >= z0) continue;
      const len = z0 - zEnd;
      const back = Y >= C[1] - 1;
      const streak = rnd(X, Y, 67) < 0.18;
      let y = Y;
      for (let z = z0 - 1; z >= zEnd; z--) {
        // DRAPE: once below the skull, a back strand closes on the back by a
        // cell a row until it lies on it (see the tail's note for why).
        if (back && z < c0[2]) {
          const b = backOf(X, z);
          if (b !== null && y > b + 1 && free(X, y - 1, z)) y--;
        }
        if (!free(X, y, z)) {
          if (!back) break;
          let j = 0;
          while (j < 16 && !free(X, y + j, z)) j++;
          if (j >= 16) break;
          // Lay the strand over the top of what it met, so the column stays
          // one piece instead of jumping a gap.
          for (let i = 1; i <= j; i++) add(X, y + i, z + 1, streak ? ART.HAIR_SHADE : ART.HAIR);
          y += j;
        }
        const tier = tierOf(z - zEnd, len);
        add(X, y, z, tier ? WISP[tier] : streak ? ART.HAIR_SHADE : ART.HAIR);
      }
    }
    // Depth: a hanging cell with more hair behind it is in the shade of it.
    for (const v of mass.values())
      if (v[3] === ART.HAIR && v[2] < C[2] && mass.has(K(v[0], v[1] + 1, v[2])))
        v[3] = ART.HAIR_SHADE;
  }

  // ---- bangs: a fringe hanging over the forehead ----------------------------
  // One strand per column across the front hairline, lying ONE CELL PROUD of
  // the face all the way down (it follows the brow as it bulges), so it is
  // hair on the forehead rather than a visor standing off it. `bangs` 1 ends
  // on the eye row. The parting (`side`) lengthens it on the side the cap's
  // line drops toward, so a side part sweeps over one eye. The last
  // `faceThin` share of each strand steps down through the wisp tiers, which
  // is what lets bangs reach the eyes without hiding them.
  const bangsCells = new Set();
  // The face's front surface and the hairline seen from the front, shared by
  // the bangs and by the face term of the thinning below.
  const frontOf = new Map();                    // (X, Z) -> the face's front Y
  const lineZ = new Map();                      // column -> front hairline Z
  let eyeLo = 1e9;
  const eyeFront = new Map();                   // (X, Z) of an eye -> its Y
  if (h.bangs > 0 || h.faceThin > 0) {
    for (const [x, y, z, c] of head.cells) {
      const X = head.mn[0] + x, Y = head.mn[1] + y, Z = head.mn[2] + z;
      const k = X * 4096 + Z;
      if (!frontOf.has(k) || Y < frontOf.get(k)) frontOf.set(k, Y);
      if (c === ART.EYE) {
        if (Z < eyeLo) eyeLo = Z;
        if (!eyeFront.has(k) || Y < eyeFront.get(k)) eyeFront.set(k, Y);
      }
    }
    if (eyeLo === 1e9) eyeLo = Math.round(C[2] - skullH * 0.2);
    // The hairline AS SEEN FROM THE FRONT: per column, walk down the face's
    // front surface from the crown while it is still scalp. NOT the lowest
    // scalp cell in the front third of the head -- on a style swept low at
    // the nape that one sits beside the ear, below the eyes, and every column
    // then had "no forehead" and grew nothing.
    const scalpK = new Set(scalp.map(([X, Y, Z]) => K(X, Y, Z)));
    for (let X = Math.floor(c0[0]); X <= c1[0]; X++) {
      let z0 = null;
      for (let Z = c1[2]; Z > eyeLo; Z--) {
        const fy = frontOf.get(X * 4096 + Z);
        if (fy === undefined) continue;          // above the crown here
        if (!scalpK.has(K(X, fy, Z))) break;     // reached skin: the line
        z0 = Z;
      }
      if (z0 !== null) lineZ.set(X, z0);
    }
  }
  if (h.bangs > 0) {
    for (const [X, z0] of lineZ) {
      const ex = (X + 0.5 - C[0]) / Math.max(halfW, 1);
      if (Math.abs(ex) > 0.9 || z0 <= eyeLo) continue;
      const full = z0 - eyeLo + 1;
      const len = h.bangs * full * (1 - h.side * ex * 0.45) *
                  (1 - 0.15 * ex * ex) * (1 - h.ragged * 0.3 * rnd(X, z0, 83));
      const n = Math.max(1, Math.min(full, Math.round(len)));
      const wl = Math.round(h.faceThin * n);
      const streak = rnd(X, 87) < 0.2;
      // FORWARD ONLY. Hair hangs off the brow; it does not tuck back into the
      // eye socket. Following the face in there stacked three and four wisp
      // cells deep in front of each eye (the strand stepping in, the welder
      // filling the step), and stacked wisps are opaque again -- the eyes
      // vanished behind bangs meant to show them.
      let y = Infinity;
      for (let i = 0; i < n; i++) {
        const z = z0 - i;
        const fy = frontOf.get(X * 4096 + z);
        if (fy === undefined) break;
        y = Math.min(y, fy - 1);
        const fromEnd = n - 1 - i;
        let tier = 0;
        if (wl >= 1 && fromEnd < wl) {
          const f = fromEnd / wl;
          tier = f < 1 / 3 ? 3 : f < 2 / 3 ? 2 : 1;
        }
        const k = K(X, y, z);
        if (add(X, y, z, tier ? WISP[tier]
                              : streak ? ART.HAIR_SHADE : ART.HAIR))
          bangsCells.add(k);
      }
    }
  }

  // The eye rows in scene coords: the line the beard hangs below and the
  // forehead the short-hair layer stays above.
  let eyeZlo = 1e9, eyeZhi = -1e9;
  for (const [, , z, c] of head.cells)
    if (c === ART.EYE) {
      eyeZlo = Math.min(eyeZlo, head.mn[2] + z);
      eyeZhi = Math.max(eyeZhi, head.mn[2] + z);
    }
  if (eyeZhi < -1e8) { eyeZlo = eyeZhi = Math.round(C[2] - skullH * 0.2); }

  // ---- a hanging beard ------------------------------------------------------
  // Strands fall from the UNDERSIDE of the painted beard: per (x, y) column,
  // the lowest beard-coloured cell below the eyes with nothing of the head
  // under it -- so the chin hangs, the jaw's edges hang, and the columns over
  // the neck stub do not. A beard with its chin shaved has no such cell and
  // hangs nothing, which is what a moustache should do. It falls straight;
  // where it meets the chest it slides FORWARD over it (front is -Y), the
  // curtain's rule mirrored. Every cell rides the head, like a tail.
  const beardCells = new Set();
  if (beardOn) {
    const fc = g.face;
    const tone = fc.beardTone === 'hair' ? ART.HAIR : ART.HAIR_SHADE;
    const low = new Map(), root = new Map();
    for (const [x, y, z, c] of head.cells) {
      const X = head.mn[0] + x, Y = head.mn[1] + y, Z = head.mn[2] + z;
      const k = X * 4096 + Y;
      if (!low.has(k) || Z < low.get(k)) low.set(k, Z);
      if ((c !== ART.HAIR && c !== ART.HAIR_SHADE) || Z >= eyeZlo - U ||
          Y >= C[1]) continue;
      if (!root.has(k) || Z < root.get(k)[2]) root.set(k, [X, Y, Z]);
    }
    const roots = [...root.entries()]
      .filter(([k, r]) => !(low.get(k) < r[2])).map(([, r]) => r);
    if (roots.length) {
      const z0 = Math.min(...roots.map(r => r[2]));
      const len0 = z0 - reach(z0, fc.beardLen);
      const bx = Math.max(1, ...roots.map(r => Math.abs(r[0] + 0.5 - C[0])));
      for (const [X, Y, rz] of roots) {
        const ax = Math.abs(X + 0.5 - C[0]) / bx;
        // One point on the midline, or two either side of it.
        const a = fc.beardFork ? Math.abs(ax - 0.45) / 0.55 : ax;
        const len = Math.round(len0 * (1 - fc.beardPoint * Math.min(1, a) * 0.9) *
                               (1 - fc.beardRagged * 0.3 * rnd(X, Y, 181)));
        const zEnd = z0 - len;
        if (zEnd >= rz) continue;
        const n = rz - zEnd, wl = Math.round(0.25 * n);
        const streak = rnd(X, Y, 187) < 0.2;
        let y = Y;
        for (let z = rz - 1; z >= zEnd; z--) {
          if (!free(X, y, z)) {
            let j = 0;
            while (j < 16 && !free(X, y - j, z)) j++;
            if (j >= 16) break;
            // Lay it over what it met so the strand stays one piece.
            for (let i = 1; i <= j; i++)
              if (add(X, y - i, z + 1, tone)) {
                beardCells.add(K(X, y - i, z + 1));
                tailCells.add(K(X, y - i, z + 1));
              }
            y -= j;
          }
          const fromEnd = z - zEnd;
          let tier = 0;
          if (wl >= 1 && fromEnd < wl) {
            const f = fromEnd / wl;
            tier = f < 1 / 3 ? 3 : f < 2 / 3 ? 2 : 1;
          }
          if (add(X, y, z, tier ? WISP[tier]
                                : streak ? (tone === ART.HAIR ? ART.HAIR_SHADE
                                                              : ART.HAIR)
                                         : tone)) {
            beardCells.add(K(X, y, z));
            tailCells.add(K(X, y, z));
          }
        }
      }
    }
  }

  // ---- the short hair layer ---------------------------------------------------
  // A coat grown off every skull cell above a line -- skin or scalp alike --
  // except the face: in front of the ears and inside the temples it starts
  // only above the brows. The BASE is every free face-neighbour of a rooted
  // cell that points outward, which is what makes it one continuous coat (a
  // tuft per root with gaps between would be dozens of specks, and the piece
  // filter below drops specks); longer tufts then run out along the radius
  // from the skull centre. All of it is wisp-tier: the densest setting is
  // still see-through, which is what makes it read as short hair over skin
  // rather than a helmet.
  //
  // THE LENGTH IS BLENDED BY REGION: top (by height on the skull) over a mix
  // of back and sides (by which way the cell faces), so dragging the side
  // length below the top one is a fade and the seams between them are soft.
  if (fuzzOn) {
    const zLow = eyeZhi + 1 - h.fuzzLow * (eyeZhi + 1 - c0[2]);
    const foreZ = eyeZhi + 3 * U;
    const beardZ = eyeZlo;                      // beard paint lives below this
    const D6 = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1],
                [0, 0, -1]];
    const put = (x, y, z, tier) => {
      if (add(x, y, z, WISP[tier])) tailCells.add(K(x, y, z));
    };
    for (const [x, y, z, c] of head.cells) {
      const X = head.mn[0] + x, Y = head.mn[1] + y, Z = head.mn[2] + z;
      if (Z < c0[2] || c === ART.EYE) continue;
      if ((c === ART.HAIR || c === ART.HAIR_SHADE) && Z < beardZ) continue;
      const nx = (X + 0.5 - C[0]) / Math.max(halfW, 1);
      const ny = (Y + 0.5 - C[1]) / Math.max(halfD, 1);
      const face = ny < -0.35 && Math.abs(nx) < 0.75;
      if (Z < (face ? foreZ : zLow)) continue;
      const wt = clamp((Z - (C[2] + skullH * 0.1)) / (skullH * 0.3), 0, 1);
      const bk = Math.max(0, ny), sd = Math.abs(nx);
      const wb = bk / (bk + sd + 1e-6);
      const Lreg = wt * h.fuzzTop +
                   (1 - wt) * (wb * h.fuzzBack + (1 - wb) * h.fuzzSide);
      if (Lreg < 0.5) continue;
      const r = [X + 0.5 - C[0], Y + 0.5 - C[1], Z + 0.5 - C[2]];
      const rn = Math.hypot(...r) || 1;
      const tier0 = clamp(3 - Math.floor(h.fuzz * 3 + rnd(X, Y, Z, 171)), 1, 3);
      let base = false;
      for (const [dx, dy, dz] of D6) {
        if (dx * r[0] + dy * r[1] + dz * r[2] <= 0) continue;
        if (!free(X + dx, Y + dy, Z + dz)) continue;
        put(X + dx, Y + dy, Z + dz, tier0);
        base = true;
      }
      if (!base) continue;                      // buried: not a surface cell
      const L = Math.max(1, Math.round(Lreg * (1 - h.fuzzFray * 0.7 *
                                                   rnd(X, Y, Z, 173))));
      for (let t = 2; t <= L; t++) {
        const px = Math.floor(X + 0.5 + r[0] / rn * t);
        const py = Math.floor(Y + 0.5 + r[1] / rn * t);
        const pz = Math.floor(Z + 0.5 + r[2] / rn * t);
        if (!free(px, py, pz)) break;
        put(px, py, pz, Math.min(3, tier0 + (t === L ? 1 : 0)));
      }
    }
  }

  // ---- regional thinning ------------------------------------------------------
  // `wisp` thins the ENDS of what hangs; these thin by WHERE a cell is. Each
  // region gives a score 0..1, the largest wins, and the score picks a wisp
  // tier with a hashed dither (score 0.5 is a mix of tiers 1 and 2, not a
  // band), so a region fades in rather than switching on. A cell is only ever
  // made THINNER than it already is, and only hair slots are touched -- a
  // tail's tie stays cloth. The bangs carry their own ramp (above), so the
  // face term skips them.
  // THE EYE WINDOW. With face thinning on, whatever lies in front of an eye
  // keeps only its OUTERMOST cell and that cell is thinned hard; the layers
  // between it and the eye go. A shell with any volume wraps two or three
  // cells deep round the outer corner of the eye (it did before any of this:
  // sidepart and bowl hide an eye), and three wisps in a row cover as much
  // as one opaque cell -- thinning each layer could never open the eye. The
  // removed cells are the hidden ones; the silhouette is unchanged.
  const eyeWin = new Set();
  if (h.faceThin > 0 && eyeFront.size) {
    const cols = new Map();
    for (const [k, ey] of eyeFront) {
      const X = Math.floor(k / 4096), Z = k - X * 4096;
      for (let dz = -1; dz <= 1; dz++)
        for (let dx = -1; dx <= 1; dx++) {
          const kk = (X + dx) * 4096 + (Z + dz);
          if (!cols.has(kk) || ey > cols.get(kk)) cols.set(kk, ey);
        }
    }
    for (const [kk, ey] of cols) {
      const X = Math.floor(kk / 4096), Z = kk - X * 4096;
      let outer = null;
      for (let Y = ey - 1; Y > ey - 12; Y--)
        if (mass.has(K(X, Y, Z))) {
          if (outer !== null) mass.delete(K(X, outer, Z));
          outer = Y;
        }
      if (outer !== null) eyeWin.add(K(X, outer, Z));
    }
  }

  if (h.faceThin > 0 || h.sideThin > 0 || h.topThin > 0 || h.fluff > 0) {
    const TIER_OF = { [ART.HAIR_WISP1]: 1, [ART.HAIR_WISP2]: 2,
                      [ART.HAIR_WISP3]: 3 };
    const open = (X, Y, Z) => !body.has(K(X, Y, Z)) && !mass.has(K(X, Y, Z));
    for (const [k, v] of mass) {
      const [X, Y, Z, slot] = v;
      const cur = TIER_OF[slot] || 0;
      if (!cur && slot !== ART.HAIR && slot !== ART.HAIR_SHADE) continue;
      if (beardCells.has(k)) continue;          // face thinning is for bangs
      let s = 0;
      if (h.faceThin > 0 && !bangsCells.has(k)) {
        // Forward of the ears and below the crown: the hair that frames the
        // face, not the top of the head.
        // Where the column has a front hairline, only at or below it: the
        // hair on top running INTO a fringe is dense, and thinning it left an
        // opaque band at the fringe's root with see-through hair above it.
        const fwd = (C[1] - (Y + 0.5)) / Math.max(halfD, 1);
        const line = lineZ.get(X);
        const low = line !== undefined
          ? clamp((line + 1.5 - Z) / 3, 0, 1)
          : clamp((C[2] + skullH * 0.35 - Z) / (skullH * 0.3), 0, 1);
        s = Math.max(s, h.faceThin * clamp((fwd - 0.1) / 0.7, 0, 1) * low);
      }
      if (h.sideThin > 0) {
        const ax = Math.abs(X + 0.5 - C[0]) / Math.max(halfW, 1);
        s = Math.max(s, h.sideThin * clamp((ax - 0.6) / 0.5, 0, 1));
      }
      if (h.topThin > 0) {
        const u = clamp((Z + 0.5 - (C[2] + skullH * 0.1)) / (skullH * 0.4), 0, 1);
        s = Math.max(s, h.topThin * u);
      }
      if (eyeWin.has(k)) s = Math.max(s, 0.34 + h.faceThin * 0.66);
      if (h.fluff > 0 &&
          (open(X + 1, Y, Z) || open(X - 1, Y, Z) || open(X, Y + 1, Z) ||
           open(X, Y - 1, Z) || open(X, Y, Z + 1) || open(X, Y, Z - 1)))
        s = Math.max(s, h.fluff);
      if (s <= 0) continue;
      const t = Math.min(3, Math.floor(s * 3 + rnd(X, Y, Z, 89)));
      if (t > cur) v[3] = WISP[t];
    }
  }

  // ---- the sylvan crown ---------------------------------------------------------
  // After the mass is built AND thinned (the thinning only touches hair slots,
  // but it also deletes cells in front of the eyes, which the veil must see
  // gone), before the split. Leaves use the hair slots, so they are leaves.
  let snoutCells = [], boughCells = [];
  if (crownOn) {
    const got = SY.crownExtras(g, { ART, K, add, C, halfW, halfD, skullH,
                                    head, mass, body, face: extra.face,
                                    splitZ, torso: P.torso });
    snoutCells = got.snout;
    boughCells = got.boughs;
  }

  // ---- split by what it rides, then into connected pieces -------------------
  const groups = { hair: [], mane: [], snout: snoutCells, bough: boughCells };
  // A TAIL RIDES THE HEAD all the way down: it is tied to the head, and a
  // ponytail swinging when the head turns is what one does.
  for (const [k, v] of mass)
    groups[v[2] >= splitZ || tailCells.has(k) ? 'hair' : 'mane'].push(v);
  const out = [];
  // FACE-CONNECTED, because that is what the engine means by one piece: a
  // carve splits a limb into fragments by 6-connectivity, and the anatomy
  // peel and test_mobgen's "one connected piece" check do the same. The
  // generators above step diagonally all the time (a ray, a strand closing
  // on the back a cell a row), so pieces that only touch along an EDGE or a
  // CORNER are WELDED: the free cells between them, walked one axis at a
  // time, are filled with hair. Then whatever is still apart is its own limb.
  const pieces6 = cells => {
    const idx = new Map(cells.map((c, i) => [K(c[0], c[1], c[2]), i]));
    const comp = new Int32Array(cells.length).fill(-1);
    const pieces = [];
    for (let i = 0; i < cells.length; i++) {
      if (comp[i] >= 0) continue;
      const piece = [i];
      comp[i] = pieces.length;
      for (let qi = 0; qi < piece.length; qi++) {
        const [X, Y, Z] = cells[piece[qi]];
        for (const [dx, dy, dz] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0],
                                    [0, 0, 1], [0, 0, -1]]) {
          const j = idx.get(K(X + dx, Y + dy, Z + dz));
          if (j === undefined || comp[j] >= 0) continue;
          comp[j] = pieces.length;
          piece.push(j);
        }
      }
      pieces.push(piece);
    }
    return { pieces, comp, idx };
  };
  // EVERY PIECE NEEDS A COLLIDER. The engine derives a limb's collider from
  // its skin by MAJORITY FILL of 2x2x2 blocks (phys/lattice.h DownsampleSkin;
  // skinScale 8 over physScale 4 on every generated body), aligned to the
  // piece's own min corner, and a limb whose collider comes out EMPTY fails
  // Mob::BuildRig -- the whole creature refuses to spawn. A one-cell twig, a
  // thin ring (a sylvan's branch or snout) or a single braid strand has no
  // block half full. So a piece with none is THICKENED at one spot, filling
  // free cells of the first block that can reach four, and dropped only if
  // no block can.
  const taken = new Set([...mass.keys()]);
  for (const c of groups.snout || []) taken.add(K(c[0], c[1], c[2]));
  for (const c of groups.bough || []) taken.add(K(c[0], c[1], c[2]));
  // THE BLOCKS ARE THE ENGINE'S: the loader maps scene (x, y, z) to engine
  // (x, z, -y) and rebases to the min corner, so a block runs up from the min
  // corner in x and z but DOWN from the max corner in scene y.
  const giveCollider = cells => {
    const mn = [0, 1, 2].map(i => Math.min(...cells.map(c => c[i])));
    const mxY = Math.max(...cells.map(c => c[1]));
    const blk = c => [mn[0] + ((c[0] - mn[0]) >> 1) * 2,
                      mxY - 1 - ((mxY - c[1]) >> 1) * 2,
                      mn[2] + ((c[2] - mn[2]) >> 1) * 2];
    const count = new Map();
    for (const c of cells) {
      const b = blk(c).join(',');
      count.set(b, (count.get(b) || 0) + 1);
    }
    for (const n of count.values()) if (n >= 4) return cells;
    const have = new Set(cells.map(c => K(c[0], c[1], c[2])));
    for (const c of cells) {
      const [bx, by, bz] = blk(c);
      const free = [];
      for (let dz = 0; dz < 2; dz++)
        for (let dy = 0; dy < 2; dy++)
          for (let dx = 0; dx < 2; dx++) {
            const k = K(bx + dx, by + dy, bz + dz);
            // Never past the max-y corner the blocks are counted from.
            if (by + dy > mxY) continue;
            if (!have.has(k) && !body.has(k) && !taken.has(k))
              free.push([bx + dx, by + dy, bz + dz, c[3]]);
          }
      const n = count.get([bx, by, bz].join(','));
      if (n + free.length < 4) continue;
      // Grown FACE TO FACE from what is already there: a block cell that only
      // touches the piece along an edge would be a detached voxel to the
      // engine (and to the one-piece assert).
      const add = [], near = new Set(have);
      let grew = true;
      while (n + add.length < 4 && grew) {
        grew = false;
        for (const f of free) {
          const k = K(f[0], f[1], f[2]);
          if (near.has(k)) continue;
          if (![[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1],
                [0, 0, -1]].some(([dx, dy, dz]) =>
                near.has(K(f[0] + dx, f[1] + dy, f[2] + dz)))) continue;
          near.add(k); add.push(f); grew = true;
          if (n + add.length >= 4) break;
        }
      }
      if (n + add.length < 4) continue;
      for (const a of add) taken.add(K(a[0], a[1], a[2]));
      return cells.concat(add);
    }
    return null;
  };
  // A bough piece must be ROOTED: face-touching the torso it rides. A tuft or
  // a fork that the arm cut off from its branch is a floating scrap, and goes.
  const torsoCells = new Set();
  if (P.torso)
    for (const [x, y, z] of P.torso.cells)
      torsoCells.add(K(P.torso.mn[0] + x, P.torso.mn[1] + y, P.torso.mn[2] + z));
  const rooted = cs => cs.some(([X, Y, Z]) =>
    torsoCells.has(K(X + 1, Y, Z)) || torsoCells.has(K(X - 1, Y, Z)) ||
    torsoCells.has(K(X, Y + 1, Z)) || torsoCells.has(K(X, Y - 1, Z)) ||
    torsoCells.has(K(X, Y, Z + 1)) || torsoCells.has(K(X, Y, Z - 1)));
  for (const [base, list] of Object.entries(groups)) {
    let cells = list;
    for (let pass = 0; pass < 4; pass++) {
      const { pieces, comp, idx } = pieces6(cells);
      if (pieces.length < 2) break;
      const added = [];
      const have = new Set(cells.map(c => K(c[0], c[1], c[2])));
      for (let i = 0; i < cells.length; i++) {
        const [X, Y, Z, slot] = cells[i];
        for (let dz = -1; dz <= 1; dz++)
          for (let dy = -1; dy <= 1; dy++)
            for (let dx = -1; dx <= 1; dx++) {
              if (Math.abs(dx) + Math.abs(dy) + Math.abs(dz) < 2) continue;
              const j = idx.get(K(X + dx, Y + dy, Z + dz));
              if (j === undefined || comp[j] === comp[i]) continue;
              // x first, then y: the first free step joins them.
              for (const [sx, sy, sz] of [[dx, 0, 0], [0, dy, 0], [0, 0, dz],
                                          [dx, dy, 0], [dx, 0, dz], [0, dy, dz]]) {
                if (!sx && !sy && !sz) continue;
                const x = X + sx, y = Y + sy, z = Z + sz, k = K(x, y, z);
                if (body.has(k)) continue;
                if (!have.has(k)) { have.add(k); added.push([x, y, z, slot]); }
                break;
              }
            }
      }
      if (!added.length) break;
      cells = cells.concat(added);
    }
    const { pieces } = pieces6(cells);
    const total = cells.length;
    // Specks too small to be a limb (a crest ray that clipped a corner) are
    // dropped; everything else is a limb.
    // (The snout is small and deliberate: it keeps anything past a speck.)
    pieces.filter(p => p.length >= (base === 'snout' || base === 'bough' ? 8
                                                     : Math.max(32, total * 0.02)))
      .sort((a, b) => b.length - a.length)
      .map(p => p.map(i => cells[i]))
      .filter(pc => base !== 'bough' || rooted(pc))
      .map(pc => giveCollider(pc))
      .filter(Boolean)
      .forEach((pc, n) => out.push({
        name: n === 0 ? base : `${base}.${n + 1}`,
        parent: HAIR_PARTS[base],
        cells: pc,
      }));
  }
  return out;
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
    if (drop.size) cells = cells.filter(c => !drop.has(c[0] + ',' + c[2]));
    return bevelCrest(cells);
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
  return bevelCrest(out);
}

/** THE FRONT AND BACK EDGE OF THE CREST, for torso and arm cap alike. Both
 *  rules round the shoulder seen from the FRONT (x steps in as z climbs), but
 *  each part's DEPTH runs full to its top row, so seen from the side the crest
 *  is a square corner at chest and back. Taking the frontmost and backmost cell
 *  off every column of the top row is one line of voxels along that edge, and
 *  the shoulder reads round from all four sides. A column shorter than three
 *  cells is left alone so the front silhouette never loses a column. */
function bevelCrest(cells) {
  let top = -1;
  for (const c of cells) top = Math.max(top, c[2]);
  const span = new Map();                    // x -> [yLo, yHi] on the top row
  for (const [x, y, z] of cells) {
    if (z !== top) continue;
    const s = span.get(x);
    if (!s) span.set(x, [y, y]);
    else { s[0] = Math.min(s[0], y); s[1] = Math.max(s[1], y); }
  }
  return cells.filter(([x, y, z]) => {
    if (z !== top) return true;
    const [lo, hi] = span.get(x);
    return hi - lo < 2 || (y !== lo && y !== hi);
  });
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

/** The anatomy recipe a generated sidecar carries.
 *
 * THE RECIPE ITSELF IS `ANA.DEFAULT_ANATOMY`, and is not restated here. It was
 * restated here until 2026-09-20, and the two copies had disagreed about what
 * is inside the skull for long enough that the test pinned the wrong one. What
 * this adds is the AUTHORING NOTES — the `//` keys a human reading
 * assets/mobs/*.json wants and a module constant should not carry.
 */
export function anatomyRecipe(race = 'human') {
  if (race === 'sylvan') {
    const r = JSON.parse(JSON.stringify(SY.ANATOMY));
    return { ...SY.ANATOMY_NOTES, ...r };
  }
  const r = JSON.parse(JSON.stringify(ANA.DEFAULT_ANATOMY));
  return {
    '//': 'What is under the skin, by depth from the surface, baked into the ' +
          '.vox by `node scripts/anatomize_mob.mjs <mob>` or the tuner\'s ' +
          'Apply recipe button (assets/editor/anatomy.js documents the ' +
          'format). Materials by NAME. `keep` leaves the painted surface ' +
          'alone; `garments` are surface voxels that are clothes, so the ' +
          'voxel under them is skin. Head override: a bone skull two deep ' +
          'around a `brain` core. `carve` (per limb) shapes the bone into a ' +
          'skeleton: eye sockets and a nasal hole in the skull, ribs, ' +
          'sternum and spine in the chest.',
    ...r,
    limbs: {
      '//head': 'A bone skull two deep around a BRAIN. The core layer is ' +
                'open-ended, so `brain` is by definition everything the shell ' +
                'encloses -- there is no geometric brain test anywhere in the ' +
                'engine, and this recipe is the only thing that decides what ' +
                'is one. Losing a voxel of it costs gore.brainHpPerVoxel flat ' +
                'on top of the ordinary volume damage (materials.json ' +
                '`brain`), which is why the skull in front of it matters: the ' +
                'rot chews bone at half rate (bone.rotRate), so breaching ' +
                'takes time and what is behind goes fast.',
      '//torso': 'A ribcage, not a bone brick: the depth schedule would fill ' +
                 'the chest with solid bone, so `carve` hollows it to flesh ' +
                 '(the organs), walls it in muscle at depth 3, and puts bone ' +
                 'back as ribs, a sternum and a spine.',
      ...r.limbs,
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
    // ASLEEP (659ab19, NPC residents): an ACTIVITY state, not a damage one --
    // the schedule puts a resident to bed and the body lies down on the spot.
    // Restated here because human.json is the standard this list follows, and
    // a generated body missing it could not be written as a diff at all (a
    // named array cannot say "remove this element").
    { name: 'sleep', activity: 'sleep', clip: 'sleep', speedScale: 0,
      disableGait: true, bodyYOffset: 0, groundAlign: 1.0 },
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
  const sylvan = g.body.race === 'sylvan';
  // What the sylvan face pass found (eyes, mouth, brow), head-local shipped:
  // the crown hangs its veil and its snout off it.
  let sylFace = null;
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
    // gives the retired gen_human.py's geometry exactly, which is what
    // scripts/test_mobgen.mjs §K measures the round against — a deliberate
    // improvement on top of the translation rather than part of it.
    if (opts.roundShoulders !== false) {
      const before = cells.length;
      if (ARCHETYPE.shoulders.fin.includes(nm))
        cells = roundShoulders(cells, size, 'fin');
      else if (ARCHETYPE.shoulders.cap.includes(nm))
        cells = roundShoulders(cells, size, 'cap');
      rounded += before - cells.length;
    }
    // THE SYLVAN SURFACE (sylvan.js barkPass): bark, grain, roots in relief,
    // knots, moss, and on the head the wooden face. Inside the part's own box.
    if (sylvan) {
      const info = { ART, mn };
      if (ARCHETYPE.roles[nm] === 'head') {
        const eyeA = table.eyeZ - L[nm].mn[2] + g.face.eyeRow;
        const neckA = Math.max(1, Math.round(2 * artSize[2] / DEF.head.size[2]));
        info.eyeRow = eyeA * SKIN_UPSCALE;
        info.mouthRow = (eyeA - g.face.mouthDrop) * SKIN_UPSCALE + 0.5;
        info.mouthDx = g.face.mouthShift * SKIN_UPSCALE;
        info.neckTop = (neckA + 1) * SKIN_UPSCALE;
      }
      cells = SY.barkPass(g, ARCHETYPE.roles[nm], nm, cells, size, info);
      if (info.face) sylFace = info.face;
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

  // ---- the hair mass, as its own parts (see hairMass) ----------------------
  // Scene cells -> a tight box each. Appended AFTER the body, so every body
  // limb keeps its index and the figure's own asserts never see them.
  const hairId = opts.materials
    ? opts.materials.findIndex(m => m.id === HAIR_MAT_ID) + 1
    : 52;
  if (opts.materials && hairId <= 0)
    throw new Error(`materials.json has no "${HAIR_MAT_ID}" material`);
  if (hairId > 127)
    throw new Error(`"${HAIR_MAT_ID}" is material ${hairId}; mob voxel ` +
                    `material ids must stay <= 127`);
  const hairParts = [];
  for (const hp of hairMass(g, parts, { face: sylFace })) {
    const mn = [0, 1, 2].map(i => Math.min(...hp.cells.map(c => c[i])));
    const mx = [0, 1, 2].map(i => Math.max(...hp.cells.map(c => c[i])));
    const size = [0, 1, 2].map(i => mx[i] - mn[i] + 1);
    if (Math.max(...size) > 120)
      throw new Error(`${hp.name} is ${Math.max(...size)} cells long; it would ` +
                      `coarsen the whole body's collider (HAIR_MAX_EXTENT)`);
    const cells = hp.cells.map(([x, y, z, c]) => [x - mn[0], y - mn[1], z - mn[2], c]);
    painted += cells.length;
    const part = { name: hp.name, size, mn, cells, parent: hp.parent,
                   hair: true, mat: hairId };
    parts.push(part);
    hairParts.push(part);
  }

  // ---- .vox ---------------------------------------------------------------
  // A SYLVAN IS SEVERAL MATERIALS, picked by art slot (sylvan.js
  // slotMaterialNames): bark, root bark, leaves, branch wood, glowing eyes.
  // Resolved by name, each held to the same <= 127 rule as FLESH_ID.
  let slotMat = null;
  if (sylvan && opts.materials) {
    slotMat = {};
    for (const [slot, nmMat] of Object.entries(SY.slotMaterialNames(g, ART))) {
      const id = opts.materials.findIndex(m => m.id === nmMat) + 1;
      if (id <= 0)
        throw new Error(`materials.json has no "${nmMat}" material (sylvan)`);
      if (id > 127)
        throw new Error(`"${nmMat}" is material ${id}; mob voxel material ids ` +
                        `must stay <= 127`);
      slotMat[slot] = id;
    }
  }
  let vox = null;
  if (opts.materials) {
    const models = [];
    for (const p of parts) {
      const size = { x: p.size[0], y: p.size[1], z: p.size[2] };
      // Translation stores the box CENTRE (vox.js sceneChunks documents the
      // layout); pivot is the integer half-size, exactly as MagicaVoxel writes.
      const t = { x: p.mn[0] + (p.size[0] >> 1), y: p.mn[1] + (p.size[1] >> 1),
                  z: p.mn[2] + (p.size[2] >> 1) };
      const mat = p.mat || fleshId;
      models.push({ name: p.name, size, t,
                    voxels: p.cells.map(([x, y, z, c]) =>
                      ({ x, y, z, c: (slotMat && slotMat[c]) || mat })) });
      models.push({ name: p.name + '.col', size, t,
                    voxels: p.cells.map(([x, y, z, c]) => ({ x, y, z, c })) });
    }
    vox = writeVox(models, paletteBytes(opts.materials, g.colors, g.body.race),
                   { scene: true });
  }

  const sidecar = buildSidecar(g, table, opts, name, hairParts);
  return {
    name, genome: g, table, parts, vox, sidecar,
    stats: { painted, rounded, worldH: table.worldH, microH: table.microH,
             eyeZ: table.eyeZ, parts: parts.length,
             hair: hairParts.reduce((n, p) => n + p.cells.length, 0) },
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
 * SEPARATE FROM generateMob ON PURPOSE. The generator's own output is the
 * un-baked figure (the retired gen_human.py it was ported from had no anatomy
 * pass). Baking is a second, named step.
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

export function paletteBytes(materials, colors, race = 'human') {
  const pal = new Uint8Array(1024);
  const put = (idx, hex) => {
    const h = String(hex).replace('#', '');
    const n = parseInt(h.slice(0, 6), 16);
    const o = (idx - 1) * 4;
    pal[o] = (n >> 16) & 255; pal[o + 1] = (n >> 8) & 255;
    pal[o + 2] = n & 255;
    // `#rrggbbaa` carries a coverage (the wisps); voxload.cpp reads it.
    pal[o + 3] = h.length === 8 ? parseInt(h.slice(6, 8), 16) & 255 : 255;
  };
  (materials || []).forEach((m, i) => {
    const hex = m && (m.colors ? m.colors[0] : m.color0);
    if (hex) put(i + 1, hex);
  });
  // The sylvan's own slots are written for a sylvan only: the engine merges
  // every loaded body's art into ONE shared palette, and five colours no
  // human paints would cost it five slots per human.
  for (const [k, slot] of Object.entries(COLOR_SLOTS))
    if (colors[k] && (race === 'sylvan' || !(k in SY.COLOR_SLOTS)))
      put(slot, colors[k]);
  for (const [slot, hex] of Object.entries(wispColors(colors)))
    put(Number(slot), hex);
  return pal;
}

function buildSidecar(g, table, opts, name, hairParts = []) {
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

  // ---- THE HAIR LIMBS (hairMass) --------------------------------------------
  // BLOODLESS and SEVERABLE, and both are load-bearing. A limb that is not
  // severable is one whose loss is a DEATH (mob.cpp HpZeroSevers), so hair
  // burnt off would kill the character; severable, losing it is a haircut.
  // `bloodless` keeps it out of the bleed, wound, burn and hp bookkeeping.
  // `hair` rides the head rigidly; `mane` hangs off the torso on a stiff ball
  // joint with a slow spring, so it lags a step when the body moves -- the
  // one bit of motion rigid hair can have. Tag `hair`: never a gait, IK or AI
  // target, and measured out of the creature's size like a held prop.
  //
  // THE ANCHOR is the piece's cell nearest its parent's joint side: the top of
  // a mane, the scalp contact of a crest. The "hanging by a thread" check
  // (mob.cpp) measures the parent's voxels round it, so it must be on the
  // parent, not out at a tip.
  const hairLimbs = [], hairAnatomy = {};
  for (const p of hairParts) {
    let best = null, bestD = Infinity;
    for (const [x, y, z] of p.cells) {
      const X = (p.mn[0] + x) / U, Y = (p.mn[1] + y) / U, Z = (p.mn[2] + z) / U;
      const par = L[p.parent];
      const px = par.mn[0] + par.size[0] / 2, py = par.mn[1] + par.size[1] / 2;
      const pz = p.parent === 'head' ? par.mn[2] + par.size[2] * 0.5
                                     : par.mn[2] + par.size[2];
      const d = (X - px) ** 2 + (Y - py) ** 2 + (Z - pz) ** 2;
      if (d < bestD) { bestD = d; best = [p.mn[0] + x, p.mn[1] + y, p.mn[2] + z]; }
    }
    const hp = Math.max(4, Math.min(40, Math.round(p.cells.length / 60)));
    const limb = {
      // A BOUGH is wood growing out of the torso: fixed, like the head's hair,
      // not the hanging mane's sprung ball.
      name: p.name, parent: p.parent,
      joint: p.parent === 'head' || p.name.startsWith('bough') ? 'fixed' : 'ball',
      hp, severable: true, vital: false, bloodless: true, tag: 'hair',
      anchor: anchor([best[0] + 0.5, best[1] + 0.5, best[2] + 1]),
    };
    if (p.parent !== 'head' && !p.name.startsWith('bough')) {
      Object.assign(limb, { cone: 0.3, coneSide: 0.2, twist: 0.15,
                            spring: { halflife: 0.3, gain: 0.3, maxAngle: 0.25 } });
    }
    hairLimbs.push(limb);
    // Keep every voxel exactly as generated: hair has no inside.
    hairAnatomy[p.name] = { layers: [{ material: HAIR_MAT_ID, keep: true }] };
  }
  limbs.push(...hairLimbs);
  const anatomy = anatomyRecipe(g.body.race);
  if (hairLimbs.length) {
    anatomy['//hair'] = 'Hair has no inside: every voxel of a hair piece is ' +
                        'kept as generated, and keep-only pieces are left out ' +
                        'of the depth union so they do not bury the scalp.';
    Object.assign(anatomy.limbs, hairAnatomy);
  }

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
    // A sylvan bleeds sap (sylvan.js SAP): less of it, and it soaks the cut.
    bleed: g.body.race === 'sylvan' ? { material: SY.SAP, perDamage: 1.5 }
                                    : { material: 'blood', perDamage: 2.5 },
    anatomy,
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

/** SYLVAN starting points. `face` and `crown` name presets (sylvan.js); the
 *  rest merges over the race-switched default exactly as a human preset
 *  merges over the default. Kept out of PRESET_ORDER, which is the human
 *  list and which test_mobgen indexes by position. */
export const SYLVAN_PRESETS = {
  sylvan: { face: 'hollow', crown: 'leafy' },
  dryad: {
    face: 'veiled', crown: 'blossom', branches: 'none',
    body: { sex: 'female', heightM: 1.66 },
    colors: { ...SY.BARKS.birch, ...SY.LEAVES.spring, ...SY.FLOWERS.pink,
              eye: SY.GLOWS.cyan },
  },
  deku: {
    face: 'mask', crown: 'leafy',
    body: { heightM: 1.55, stack: { shin: 12, thigh: 12, torso: 17, head: 18 } },
    colors: { ...SY.BARKS.oak, ...SY.LEAVES.forest, eye: SY.GLOWS.amber },
    sylvan: { roots: 0.3, grain: 0.4 },
  },
  thornwood: {
    face: 'gnarled', crown: 'sparse', branches: 'antlers',
    body: { heightM: 1.84, shoulderWidth: 12, bodyDepth: 6,
            stack: { shin: 15, thigh: 16, torso: 18, head: 15 } },
    shape: { waist: 0.72, chest: 0.85, armGirth: 0.78, legGirth: 0.78 },
    colors: { ...SY.BARKS.dark, ...SY.LEAVES.autumn, eye: SY.GLOWS.ember },
    sylvan: { roots: 0.85, rootTwist: 0.8, knots: 0.6 },
  },
  willowkin: {
    face: 'hollow', crown: 'willow', branches: 'none',
    body: { sex: 'female', heightM: 1.78 },
    colors: { ...SY.BARKS.ash, ...SY.LEAVES.silver, ...SY.FLOWERS.white,
              eye: SY.GLOWS.white },
  },
  mossback: {
    face: 'howler', crown: 'mossy', branches: 'shoulders',
    body: { heightM: 1.7, shoulderWidth: 14, bodyDepth: 10, limbWidth: 6 },
    shape: { chest: 1.1, shoulder: 1.1, armGirth: 1.05 },
    colors: { ...SY.BARKS.mossy, ...SY.LEAVES.forest, eye: SY.GLOWS.green },
    sylvan: { moss: 0.9, roots: 0.7 },
  },
};
export const SYLVAN_PRESET_ORDER = Object.keys(SYLVAN_PRESETS);

/** A preset as a full genome. Presets are PARTIAL on purpose: a preset that
 *  restated every gene would silently freeze at whatever the defaults were the
 *  day it was written. */
/** The creature every generated character inherits from. A stem, not a path:
 *  it is `extends` in the file and the file lives beside the base. */
export const BASE_MOB = 'human';

/** The FOLDER a character is filed in (src/game/sidecar.h "PROTOTYPES AND
 *  VARIANTS": filing, not semantics). Every generated character still
 *  `extends` BASE_MOB -- a sylvan is the human's rig with another surface --
 *  but a sylvan is filed beside its kin in assets/mobs/sylvan/. */
export const raceFolder = genome =>
  (genome && genome.body && genome.body.race === 'sylvan') ? 'sylvan' : BASE_MOB;

/**
 * A GENERATED CHARACTER IS A DIFF, NOT A BODY.
 *
 * `generateMob` derives a whole creature, and for two years the whole creature
 * was what got written: fifteen limbs, four chains, seven loco states, three
 * natural weapons, an anatomy recipe and fifteen clips, 63 KB of it, frozen on
 * the day the character was born. Four commits then changed how a human fights
 * and falls apart, none of them reached the pool, and characters shipped with a
 * bounce-on-two-stumps loco state and a third of the standard's limb hp
 * (2c4f29b). The copies were the bug.
 *
 * So the file that is written is the DIFFERENCE between this body and the
 * resolved base, plus its genome and the two lines that say where it came from.
 * Everything the generator derived that equals the base is dropped and reaches
 * this character at load instead, which means editing the human's crawl, or its
 * jaws, or its anatomy recipe, reaches every character with no re-bake.
 *
 * It is computed by DIFFING rather than by listing the keys somebody believed
 * were per-body, because that list goes stale the first time a derived field is
 * added and the symptom — a character silently wearing the human's number — is
 * invisible. sidecar.js's `diffAgainst` re-applies its own output and throws if
 * it does not reproduce this body exactly, so a file that would load as a
 * different creature is never written.
 *
 * @param {object} full   a `generateMob(...).sidecar`
 * @param {object} base   the RESOLVED base sidecar (assets/mobs/human.json)
 * @param {string} name   the asset stem this will be saved under
 */
/**
 * WHAT THE GENERATOR DOES NOT MODEL IS INHERITED -- one level down as well as
 * at the top. A generated body authors TWO clips (walk and run, whose period
 * its own leg sets); every other clip on the human -- a bite, a lunge, the
 * punches -- is the base's business, and a body with no opinion about it must
 * inherit it rather than delete it (diffAgainst would write `bite: null`, and
 * the character could no longer bite). Returns a copy of `full` with those
 * filled in from `base`; thinSidecar diffs THIS, and test_mobgen section M
 * compares a resolved file against it.
 */
export function inheritUnmodelled(full, base) {
  const body = { ...full };
  for (const k of Object.keys(base))
    if (!k.startsWith('//') && !(k in body))
      body[k] = JSON.parse(JSON.stringify(base[k]));
  if (base.clips && body.clips) {
    body.clips = { ...body.clips };
    for (const [k, v] of Object.entries(base.clips))
      if (!k.startsWith('//') && !(k in body.clips))
        body.clips[k] = JSON.parse(JSON.stringify(v));
  }
  return body;
}

export function thinSidecar(full, base, name) {
  const body = inheritUnmodelled(full, base);
  delete body.genome;                       // the input, never inherited
  // WHAT THE GENERATOR DOES NOT MODEL IS INHERITED, NOT DELETED.
  //
  // diffAgainst turns a key the base has and the target does not into an
  // explicit null, which is the RFC rule and is right for a variant somebody
  // wrote by hand. It is wrong here: a generated body is a description of a
  // SHAPE — proportions, anchors, the two clip periods its leg implies — and
  // not a whole creature, so a key it has no opinion about would be STRIPPED
  // FROM EVERY CHARACTER IN THE POOL the day it was added to the human.
  // `turn` (what a body gets up as) was the first one and cost this comment;
  // whatever the next one is, absent means inherited (inheritUnmodelled,
  // which also does it for the clips the generator does not author).
  const patch = SC.diffAgainst(base, body) || {};
  return {
    '//': `A CHARACTER, not a creature: ${name} inherits ${BASE_MOB}'s rig — ` +
          'limb names and tags, chains, loco states, natural weapons, sockets, ' +
          'the anatomy recipe and the shared clip library — and states only ' +
          'what its own body makes different. Written by the Characters page ' +
          `or by \`node scripts/gen_mobs.mjs ${name} --rebake\`, from the ` +
          '`genome` block at the bottom. Do not hand-edit: re-roll it.',
    extends: BASE_MOB,
    // IGNORED BY THE LOADER, and said anyway: a file's own .vox wins over any
    // `model` (mob.cpp, CollectMobSources), so this is documentation that the
    // body is this character's own art and not the base's, which is the one
    // thing `extends` would otherwise imply and get wrong.
    model: name,
    ...patch,
    genome: full.genome,
  };
}

export function presetGenome(key) {
  const sp = SYLVAN_PRESETS[key];
  const p = PRESETS[key] || sp;
  if (!p) return defaultGenome();
  const g = defaultGenome();
  const merge = (dst, src) => {
    for (const [k, v] of Object.entries(src || {})) {
      if (v && typeof v === 'object' && !Array.isArray(v)) merge(dst[k] ??= {}, v);
      else dst[k] = v;
    }
  };
  if (sp) {
    const { face, crown, branches, body, ...rest } = sp;
    const { sex, ...bodyRest } = body || {};
    if (sex) applySex(g, sex);
    applyRace(g, 'sylvan');
    SY.applyFace(g, face);
    applyCrown(g, crown);
    SY.applyBranches(g, branches || 'sprigs');
    merge(g, { body: bodyRest, ...rest });
  } else merge(g, p);
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
  // The BODY's corner is the prefab origin (mob.cpp rebases to it, measuring
  // hair out exactly as it measures a held prop out); hair may lie past it.
  const bodyParts = parts.filter(p => !p.hair);
  const minX = Math.min(...bodyParts.map(p => p.mn[0]));
  const maxY = Math.max(...bodyParts.map(p => p.mn[1] + p.size[1]));
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
