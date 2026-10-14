/* serpent.js — A SERPENT'S TAIL IN PLACE OF THE LEGS: a LOWER-BODY PLAN.
 *
 * WHAT IT IS, TO THE GENERATOR. Not a race. `genome.body.lower` is 'legs' (the
 * human's six leg limbs, the gait, the stride) or 'serpent' (this file): the
 * upper body -- hips, torso, head, arms, hands, sockets, fists, jaws, the arm
 * IK chains, every upper-body clip -- is the human's exactly, and below the
 * hips there is one long tail. The naga race (naga.js) is the first body that
 * wears it; nothing here knows what a naga is, so any race could be given a
 * tail (and a later transformation can swap one in: the sidecar side of the
 * swap is the patch buildSidecar writes from here, which is data).
 *
 * THE RIG. Twelve parts hang off the hips:
 *
 *   tail1         the WAIST, straight down from the pelvis to half its height
 *   tail2         the RISE, a 45-degree run down and back to the ground
 *   tail3..tail12 the TRAIL, ten segments lying on the ground behind the body
 *
 * tail1 and tail2 are the upright figure's lower half and are measured like
 * legs (they are what it stands on). The trail is TRAILING (sidecar
 * `trailing: true`, mob.h MobLimbDef::trailing): measured OUT of the body box
 * the way hair is, so the creature's footing, pivot and spawn box are an
 * upright figure's and not a 4-metre snake's. The trail is the `slither`
 * chain, with `upright: true` (mob.h SlitherDef::upright): the engine waves it
 * as the body travels (Mob::ApplySlither, the odometer-phase wave the snake
 * uses) and lays each segment on the ground behind the body (Mob::DrapeTail)
 * without leaning the body. In water the same chain kicks on a clock with the
 * `slither.water` wave -- a PITCH for a mermaid's kick or a yaw for a sea
 * snake's undulation -- under the swim strokes below, which hold tail2 and
 * tail3 straight so the whole body lies in one line.
 *
 * THE SHAPE IS ONE SWEPT TUBE, cut into parts by arc length. A path runs from
 * inside the pelvis straight down (the waist), 45 degrees down and back (the
 * rise) and then back along the ground (the trail, its centre at half its own
 * thickness so the belly stays on the ground as it thins). Every cell of the
 * tube knows its arc length, which is what the scale pattern is painted by,
 * so the bands and diamonds run unbroken from the waist to the tip and across
 * every joint. Fins, frills and the tip (a rattle, a spade, a mermaid's fluke,
 * a fan, a club) are added to the same cell set before the cut.
 *
 * PURE AND NODE-RUNNABLE; imports only robotkit.js. Every gene is gated on
 * `lower: 'serpent'` (mobgen specApplies), so a legged roll takes no draw for
 * any of them and every human litter's sequence is unchanged.
 */

'use strict';

import { rnd, clamp, vnoise } from './robotkit.js';

export const LOWERS = ['legs', 'serpent'];
export const isSerpent = g => !!(g && g.body && g.body.lower === 'serpent');

/** The leg limbs a serpent body does not have (the human's names). */
export const LEG_PARTS = ['legU.L', 'legL.L', 'foot.L', 'legU.R', 'legL.R',
                          'foot.R'];
export const TRAIL_COUNT = 10;
export const RISE = ['tail1', 'tail2'];
export const TRAIL = Array.from({ length: TRAIL_COUNT }, (_, i) => `tail${i + 3}`);
export const PARTS = [...RISE, ...TRAIL];

// =============================================================================
// art slots and colours: the serpent's own, below the naga's (214)
// =============================================================================

export const SLOTS = {
  SCALE: 213,        // the dorsal scales: most of the tail
  SCALE_SHADE: 212,  // the flanks, turned from the light
  PATTERN: 211,      // bands, diamonds, rings, blotches, stripes
  BELLY: 210,        // the belly scutes
  FIN: 209,          // fins, frills, the fluke, the hood's webbing
};
export const COLOR_SLOTS = {
  scale: SLOTS.SCALE, scaleShade: SLOTS.SCALE_SHADE, pattern: SLOTS.PATTERN,
  belly: SLOTS.BELLY, fin: SLOTS.FIN,
};
export const DEFAULT_COLORS = {
  scale: '#4a6a3a', scaleShade: '#33492a', pattern: '#2a3a1e',
  belly: '#d8cf9a', fin: '#8aa86a',
};
export const COLOR_SPECS = [
  ['scale', 'scales', 'The tail\'s back and most of its length.'],
  ['scaleShade', 'scales, shaded', 'The tail\'s flanks, turned from the ' +
   'light. A darker version of the scales.'],
  ['pattern', 'pattern', 'The bands, diamonds, rings, blotches or stripes.'],
  ['belly', 'belly', 'The broad scutes underneath, and the inside of a hood.'],
  ['fin', 'fins', 'Crest, frills, the fluke or fan at the tip, fin ears.'],
];

// =============================================================================
// the genome
// =============================================================================

export const PATTERNS = ['plain', 'banded', 'diamond', 'ringed', 'blotched',
                         'striped', 'speckled'];
export const TIPS = ['point', 'rattle', 'spade', 'fluke', 'fan', 'club'];
export const SWIMS = ['kick', 'undulate'];

export function defaultGenes() {
  return {
    length: 1.7,       // the trail, in BODY HEIGHTS (the upright figure's)
    girth: 0.5,        // how thick the tail is at the waist
    taper: 0.55,       // 0 stays thick nearly to the end; 1 thins from the rise
    flat: 0.3,         // the trail's section: 0 round, 1 a flat-bellied ribbon
    pattern: 'banded',
    patternScale: 0.5, // how far apart the bands / diamonds / blotches sit
    belly: 0.5,        // how far up the sides the belly scutes reach
    crest: 0,          // a dorsal fin along the trail, its height
    crestRun: 0.6,     // ...how much of the trail it runs, from the tip forward
    frills: 0,         // side frills along the back half of the trail
    tip: 'point',
    tipSize: 0.5,
    speed: 0.9,        // slithering speed, as a share of a man's sprint
    wave: 0.5,         // how wide the slither's S is
    waveLength: 0.5,   // how long each wave of it is
    swim: 'kick',      // 'kick' (a mermaid: up and down) | 'undulate' (side to side)
    kick: 0.5,         // how fast and hard the tail beats in water
    sway: 0.4,         // the idle sway of the waist, always
    tipFlick: 0.5,     // the idle flick of the tip, always
  };
}

export const GENE_GROUP = {
  key: 'serpent', title: 'serpent tail', lower: 'serpent',
  note: 'Below the hips: a waist, a rise to the ground and a long tail lying ' +
        'behind, which slithers in an S as the body travels and kicks like a ' +
        'mermaid\'s (or undulates like a sea snake\'s) in water. Lengths are ' +
        'in body heights. The pattern runs unbroken from the waist to the tip.',
};

const S = (key, label, hint, extra = {}) => ({
  path: 'serpent.' + key, label, group: 'serpent', min: 0, max: 1,
  step: 0.05, sigma: 0.15, lower: 'serpent', hint, ...extra });

export const GENE_SPECS = [
  S('length', 'tail length', 'How long the tail lying behind is, in body ' +
    'heights: a short tail at 1, a great serpent at 3.',
    { min: 0.8, max: 3, sigma: 0.3 }),
  S('girth', 'girth', 'How thick the tail is at the waist: a slim ' +
    'whip-snake at 0, a python at 1.'),
  S('taper', 'taper', 'How soon the tail thins: at 0 it stays thick nearly ' +
    'to the end, at 1 it narrows from the rise.'),
  S('flat', 'flatness', 'The tail\'s section: round at 0, a flat-bellied ' +
    'ribbon at 1.'),
  { path: 'serpent.pattern', label: 'pattern', group: 'serpent', kind: 'enum',
    choices: PATTERNS, sigma: 0.15, lower: 'serpent',
    hint: 'Plain; banded; diamondback; ringed right round (a coral snake); ' +
          'blotched (a python); striped along the length; speckled.' },
  S('patternScale', 'pattern spacing', 'How far apart the bands, diamonds ' +
    'and blotches are.'),
  S('belly', 'belly scutes', 'How far up the sides the pale belly reaches. ' +
    '0 is scales all round.'),
  S('crest', 'dorsal crest', 'A fin along the top of the tail, its height. ' +
    '0 is none.', { roll: 0.25 }),
  S('crestRun', 'crest length', 'How much of the tail the crest runs, from ' +
    'the tip forward.'),
  S('frills', 'side frills', 'Scalloped fins along both sides of the back ' +
    'half of the tail. 0 is none.', { roll: 0.2 }),
  { path: 'serpent.tip', label: 'tail tip', group: 'serpent', kind: 'enum',
    choices: TIPS, sigma: 0.15, lower: 'serpent',
    hint: 'A plain point; a rattle; a flat spade; a mermaid\'s fluke (flat, ' +
          'two lobes); an upright fan; a bony club.' },
  S('tipSize', 'tip size', 'How big the rattle, spade, fluke, fan or club ' +
    'is.'),
  S('speed', 'slither speed', 'How fast it travels on land, against a ' +
    'man\'s sprint.', { min: 0.5, max: 1.2, sigma: 0.1 }),
  S('wave', 'slither width', 'How wide the S the tail swims across the ' +
    'ground is. 0 is nearly straight.'),
  S('waveLength', 'wave length', 'How long each wave of the S is: tight ' +
    'and quick at 0, long and lazy at 1.'),
  { path: 'serpent.swim', label: 'swimming', group: 'serpent', kind: 'enum',
    choices: SWIMS, sigma: 0.1, lower: 'serpent',
    hint: 'kick: the tail beats up and down like a mermaid\'s. undulate: it ' +
          'swims side to side like a sea snake\'s.' },
  S('kick', 'swim beat', 'How fast and how far the tail beats in water.'),
  S('sway', 'idle sway', 'How much the waist sways while it waits. 0 holds ' +
    'it still.'),
  S('tipFlick', 'tip flick', 'How far the end of the tail flicks while it ' +
    'waits.'),
];

export function randomize(g, r, isLocked) {
  const s = g.serpent;
  for (const spec of GENE_SPECS) {
    if (isLocked(spec.path)) continue;
    const k = spec.path.split('.')[1];
    if (spec.kind === 'enum') { s[k] = r.pick(spec.choices); continue; }
    // A rare feature (`roll`) is rolled ON that often and otherwise left off.
    if (spec.roll !== undefined && r() > spec.roll) { s[k] = 0; continue; }
    const v = s[k] + r.norm() * (spec.sigma ?? 0.1) * 0.6;
    s[k] = clamp(v, spec.min, spec.max);
  }
  return g;
}

// =============================================================================
// THE TABLE: the tube, swept and cut. Authored micro (mobgen's 40 per metre),
// SCENE coordinates (x across, y BACK, z up), centred on x = 0.
// =============================================================================

const even = v => Math.max(2, Math.round(v / 2) * 2);

/** The trail's section at u (0 at the rise, 1 at the tip), as a fraction of
 *  its starting width. */
function thinAt(s, u) {
  const p = 1.0 + 1.8 * (1 - s.taper);
  return 1 - (1 - 0.14) * Math.pow(clamp(u, 0, 1), p);
}

/**
 * Extend a limb table built for legs into one for a serpent: the six leg
 * boxes go, the twelve tail boxes come, each with its own cells (scene, with
 * colours) for SHAPES to hand back. Mutates and returns `table`.
 */
export function extendTable(g, table) {
  const s = g.serpent;
  const L = table.limbs;
  const b = g.body;
  for (const n of LEG_PARTS) delete L[n];
  const hb = table.rows.hips.base;
  // The waist fills the pelvis: as wide as the hips at girth 0.5.
  // THE WAIST CARRIES THE PELVIS: about the hips' own width (a little under
  // at girth 0, a little over at 1) -- wider and it reads as a pillar the
  // body is standing on. The tail is heaviest where it meets the ground
  // (w0), the way a snake's body is behind its neck.
  const wW = even(b.hipWidth * (0.92 + 0.22 * s.girth) + 0.5);
  const wD = even(b.bodyDepth * (0.95 + 0.25 * s.girth) + 0.5);
  const w0 = Math.max(4, wW * (1.08 + 0.14 * s.girth));    // the trail's start
  const hRatio = 0.92 - 0.3 * s.flat;
  const h0 = Math.max(3, w0 * hRatio);
  const r1 = h0 / 2;
  const zTop = hb + 2;                    // inside the pelvis by its overlap
  const zA = Math.max(Math.ceil(r1 + 3), Math.round(hb * 0.5));
  const R = Math.max(2, zA - r1);         // the rise: 45 degrees
  const microH = table.microH;
  const trailLen = Math.max(TRAIL_COUNT * 4, Math.round(s.length * microH));
  const segLen = trailLen / TRAIL_COUNT;

  // ---- the path, as samples: centre (y, z), tangent (ty, tz), radii, s ----
  const sA = zTop - zA, sB = sA + R * Math.SQRT2, sEnd = sB + trailLen;
  const ds = 0.25;
  const samples = [];
  for (let sv = 0; sv <= sEnd + 1e-6; sv += ds) {
    let y, z, ty, tz, rx, rp;
    if (sv <= sA) {
      const t = sv / sA;
      y = 0; z = zTop - sv; ty = 0; tz = -1;
      rx = wW / 2 * (1 - 0.06 * t); rp = wD / 2 * (1 - 0.04 * t);
    } else if (sv <= sB) {
      const t = (sv - sA) / (sB - sA);
      const d = (sv - sA) / Math.SQRT2;
      y = d; z = zA - d; ty = Math.SQRT1_2; tz = -Math.SQRT1_2;
      rx = (wW / 2 * 0.94) * (1 - t) + (w0 / 2) * t;
      rp = (wD / 2 * 0.96) * (1 - t) + r1 * t;
    } else {
      const u = (sv - sB) / trailLen;
      const k = thinAt(s, u);
      rx = Math.max(0.7, w0 / 2 * k);
      rp = Math.max(0.6, r1 * k);
      y = R + (sv - sB); z = rp; ty = 1; tz = 0;
    }
    samples.push({ s: sv, y, z, ty, tz, rx, rp });
  }

  // ---- the tube: each cell claimed by the sample it is deepest inside ----
  const K = (x, y, z) => `${x},${y},${z}`;
  const cells = new Map();
  for (const p of samples) {
    const X = Math.ceil(p.rx) + 1, P = Math.ceil(p.rp) + 1;
    for (let z = Math.floor(p.z - P); z <= Math.ceil(p.z + P); z++) {
      // Never below the ground, and never up through the pelvis: the waist
      // stops where the hips' box takes over.
      if (z < 0 || z >= zTop) continue;
      for (let y = Math.floor(p.y - P); y <= Math.ceil(p.y + P); y++)
        for (let x = -X; x < X; x++) {
          const dx = (x + 0.5) / p.rx;
          const dy = y + 0.5 - p.y, dz = z + 0.5 - p.z;
          const d = dx * dx + (dy * dy + dz * dz) / (p.rp * p.rp);
          if (d > 1) continue;
          const k = K(x, y, z);
          const was = cells.get(k);
          if (!was || d < was.d) cells.set(k, { x, y, z, d, p });
        }
    }
  }

  // ---- the paint: belly, flank, pattern, scutes -----------------------------
  const P = 3 + 9 * s.patternScale * (0.7 + 0.6 * s.girth);   // pattern period
  const seed = 0x5e9e;
  const out = [];                       // [x, y, z, colour, s]
  for (const c of cells.values()) {
    const p = c.p;
    // The tube's own frame at this cell: across (x), and `nrm` toward the
    // belly (front on the waist, down on the trail).
    const oy = c.y + 0.5 - p.y, oz = c.z + 0.5 - p.z;
    const nrm = (oy * p.tz + oz * -p.ty) / p.rp;     // + toward the belly
    const lat = (c.x + 0.5) / p.rx;                  // -1..1 across
    const up = -nrm;                                 // + on the back
    const sv = p.s + (oy * p.ty + oz * p.tz);        // the cell's own arc length
    const isBelly = s.belly > 0.02 && nrm > 0.62 - 1.0 * s.belly &&
                    Math.abs(lat) < 0.55 + 0.6 * s.belly;
    let col = Math.abs(lat) > 0.62 && up < 0.35 ? SLOTS.SCALE_SHADE : SLOTS.SCALE;
    if (isBelly) {
      col = SLOTS.BELLY;
      // The scutes: a thin shaded seam every few cells along the length, at
      // the belly's edges only, so the plates read without striping it.
      if (((sv % 4) + 4) % 4 < 0.8 && Math.abs(lat) > 0.45)
        col = SLOTS.SCALE_SHADE;
    }
    const ph = ((sv / P) % 1 + 1) % 1;
    switch (s.pattern) {
      case 'banded':
        if (!isBelly && ph < 0.32) col = SLOTS.PATTERN;
        break;
      case 'ringed':
        // Right round, the belly too: wide, a narrow pale ring, wide.
        if (ph < 0.38) col = SLOTS.PATTERN;
        else if (ph < 0.5) col = SLOTS.BELLY;
        break;
      case 'diamond': {
        // A chain of diamonds down the spine, each outlined.
        const dd = Math.abs(lat) * 1.6 + Math.abs(ph - 0.5) * 2;
        if (!isBelly && up > -0.3) {
          if (dd < 0.75 && dd > 0.5) col = SLOTS.PATTERN;
          else if (dd <= 0.5 && dd > 0.32) col = SLOTS.SCALE_SHADE;
        }
        break;
      }
      case 'blotched':
        if (!isBelly && vnoise(seed, c.x * 1.2, sv, c.z * 1.2, P * 0.55) > 0.62)
          col = SLOTS.PATTERN;
        break;
      case 'striped':
        if (!isBelly && (Math.abs(lat) < 0.16 ||
                         (Math.abs(lat) > 0.5 && Math.abs(lat) < 0.68)))
          col = SLOTS.PATTERN;
        break;
      case 'speckled':
        if (!isBelly && rnd(seed, c.x, c.y, c.z) > 0.86) col = SLOTS.PATTERN;
        break;
      default:
        break;
    }
    out.push([c.x, c.y, c.z, col, sv]);
  }

  // ---- fins: the crest, the frills, the tip --------------------------------
  const taken = new Set(out.map(c => K(c[0], c[1], c[2])));
  const add = (x, y, z, col, sv) => {
    if (z < 0) return;
    const k = K(x, y, z);
    if (taken.has(k)) return;
    taken.add(k);
    out.push([x, y, z, col, sv]);
  };
  const trailAt = sv => samples[Math.min(samples.length - 1,
                                         Math.max(0, Math.round(sv / ds)))];
  // The crest: a fin two cells thick on the centre line, scalloped, rising
  // from the tail's back over the last `crestRun` of the trail.
  if (s.crest > 0.02) {
    const run = Math.max(0.1, s.crestRun) * trailLen;
    const s0 = sEnd - run;
    for (let y = Math.floor(R + (s0 - sB)); y < Math.floor(R + trailLen); y++) {
      const sv = sB + (y + 0.5 - R);
      const u = (sv - s0) / run;
      const p = trailAt(sv);
      const env = Math.sin(Math.PI * clamp(u, 0, 1));
      const scallop = (Math.floor(sv / 3) % 2) ? 1 : 0.7;
      const H = s.crest * h0 * 0.9 * env * scallop;
      const top = Math.floor(p.z + p.rp);
      for (let z = top; z < top + Math.round(H); z++)
        for (const x of [-1, 0]) add(x, y, z, SLOTS.FIN, sv);
    }
  }
  // The frills: thin fins out of both flanks along the back half.
  if (s.frills > 0.02) {
    for (let y = Math.floor(R + trailLen * 0.35); y < Math.floor(R + trailLen * 0.95); y++) {
      const sv = sB + (y + 0.5 - R);
      const u = (sv - sB) / trailLen;
      const p = trailAt(sv);
      const env = Math.sin(Math.PI * clamp((u - 0.35) / 0.6, 0, 1));
      const scallop = (Math.floor(sv / 2) % 2) ? 1 : 0.55;
      const W = s.frills * w0 * 0.55 * env * scallop;
      const z = Math.max(0, Math.floor(p.z));
      const x0 = Math.ceil(p.rx) - 1;
      for (let i = 0; i < Math.round(W); i++) {
        add(x0 + i, y, z, SLOTS.FIN, sv);
        add(-x0 - i - 1, y, z, SLOTS.FIN, sv);
      }
    }
  }
  tipCells(s, { sB, sEnd, R, trailLen, segLen, w0, h0, samples: trailAt, add,
                out, taken, K });

  // ---- the cut: each cell to the part whose arc range holds it, plus the
  // seam's rows into both neighbours so a bent joint shows no gap.
  const bounds = [0, sA, sB];
  for (let k = 1; k <= TRAIL_COUNT; k++) bounds.push(sB + k * segLen);
  bounds[bounds.length - 1] = Infinity;          // the tip's extras
  const byPart = PARTS.map(() => []);
  for (const c of out) {
    const sv = c[4];
    let i = 0;
    while (i < PARTS.length - 1 && sv >= bounds[i + 1]) i++;
    byPart[i].push(c);
    // The seam is in BOTH parts, a row each way: the joint (on the path at
    // the boundary) is then inside the child's box as well as the parent's.
    if (i > 0 && sv - bounds[i] < 1.0) byPart[i - 1].push(c);
    if (i < PARTS.length - 1 && bounds[i + 1] - sv < 1.0) byPart[i + 1].push(c);
  }
  const limbs = {};
  const partCells = {};
  for (let i = 0; i < PARTS.length; i++) {
    const cs = byPart[i];
    if (!cs.length) throw new Error(`serpent: ${PARTS[i]} has no cells`);
    const X = Math.max(...cs.map(c => Math.max(c[0] + 1, -c[0])));
    const y0 = Math.min(...cs.map(c => c[1])), y1 = Math.max(...cs.map(c => c[1]));
    const z0 = Math.min(...cs.map(c => c[2])), z1 = Math.max(...cs.map(c => c[2]));
    const mn = [-X, y0, z0];
    const size = [2 * X, y1 - y0 + 1, z1 - z0 + 1];
    limbs[PARTS[i]] = { size, mn, trailing: i >= RISE.length };
    partCells[PARTS[i]] = cs.map(([x, y, z, col]) => [x - mn[0], y - mn[1],
                                                      z - mn[2], col]);
    L[PARTS[i]] = limbs[PARTS[i]];
  }
  // The joints, on the path (scene, authored): each part hangs from the point
  // where its arc range starts.
  const at = sv => {
    const p = samples[Math.min(samples.length - 1, Math.round(sv / ds))];
    return [0, p.y, p.z];
  };
  const joints = { tail1: [0, 0, hb + 1] };
  for (let i = 1; i < PARTS.length; i++) joints[PARTS[i]] = at(bounds[i]);
  table.serpent = { partCells, joints, trailLen, segLen, sA, sB, R, wW, wD,
                    w0, h0, zA, zTop };
  return table;
}

/** The tip: what replaces the last stretch of the point. */
function tipCells(s, ctx) {
  const { sEnd, R, trailLen, w0, h0, samples, add, out, K } = ctx;
  const yTip = R + trailLen;              // just past the end of the trail
  const size = 0.4 + 0.9 * s.tipSize;
  const end = samples(sEnd);
  const cz = end.z;
  if (s.tip === 'rattle') {
    // Beads of keratin over the last stretch: bulging rings, alternating
    // pale and dark, a little wider than the tail they cap.
    // Never longer than the last segment: the rattle is that part's, and a
    // rattle reaching past its joint would leave the joint outside its box.
    const L = Math.min(Math.round(w0 * (0.8 + 0.8 * size)),
                       Math.floor(ctx.segLen * 0.9));
    const r = Math.max(1.2, w0 * 0.26 * (0.8 + 0.5 * size));
    // Clear the point under the rattle so the rings are the end.
    for (let i = out.length - 1; i >= 0; i--)
      if (out[i][1] >= yTip - L) {
        ctx.taken.delete(K(out[i][0], out[i][1], out[i][2]));
        out.splice(i, 1);
      }
    const bead = Math.max(2, Math.round(r * 0.9));
    for (let y = yTip - L; y < yTip; y++) {
      const t = (y - (yTip - L)) / L;
      const ring = Math.floor((y - (yTip - L)) / bead);
      const q = ((y - (yTip - L)) % bead) / bead;
      const rr = r * (1 - 0.35 * t) * (0.78 + 0.22 * Math.sin(Math.PI * q));
      const zc = Math.max(rr, cz);
      for (let z = 0; z <= Math.ceil(zc + rr); z++)
        for (let x = -Math.ceil(rr) - 1; x <= Math.ceil(rr); x++) {
          const dx = x + 0.5, dz = z + 0.5 - zc;
          if (dx * dx + dz * dz > rr * rr) continue;
          add(x, y, z, ring % 2 ? SLOTS.BELLY : SLOTS.SCALE_SHADE,
              sEnd - (yTip - y));
        }
    }
    return;
  }
  if (s.tip === 'club') {
    const r = Math.max(1.5, w0 * 0.42 * (0.7 + 0.6 * size));
    const yc = yTip - r * 0.6, zc = Math.max(r * 0.9, cz);
    for (let z = 0; z <= Math.ceil(zc + r); z++)
      for (let y = Math.floor(yc - r); y <= Math.ceil(yc + r); y++)
        for (let x = -Math.ceil(r) - 1; x <= Math.ceil(r); x++) {
          const dx = x + 0.5, dy = y + 0.5 - yc, dz = z + 0.5 - zc;
          const rr = dx * dx + dy * dy + dz * dz;
          if (rr > r * r) continue;
          // Knobbed: a few studs proud of the ball in the pattern colour.
          add(x, y, z, rr > r * r * 0.6 && rnd(7, x, y, z) > 0.55
                ? SLOTS.PATTERN : SLOTS.SCALE_SHADE, sEnd);
        }
    return;
  }
  // The flat ones: spade and fluke lie FLAT (spread across x, a cell or two
  // thick), the fan stands UP (spread in z). `lobe(a)` is the half-span at
  // `a` cells behind where the fin starts.
  const Lf = Math.max(3, Math.round(w0 * (0.9 + 1.6 * size)));
  const span = w0 * (0.8 + 1.8 * size);
  const y0 = yTip - Math.round(Lf * 0.35);
  for (let a = 0; a < Lf; a++) {
    const y = y0 + a;
    const t = a / Lf;
    let half, notch = 0;
    if (s.tip === 'spade') {
      half = (t < 0.45 ? t / 0.45 : (1 - t) / 0.55) * span * 0.6;
    } else if (s.tip === 'fluke') {
      // Two lobes swept back, with a notch between them at the trailing edge.
      half = Math.pow(clamp(t / 0.8, 0, 1), 0.7) * span;
      notch = t > 0.55 ? half * clamp((t - 0.55) / 0.45, 0, 1) * 0.75 : 0;
    } else if (s.tip === 'fan') {
      half = Math.sin(Math.PI * clamp(t * 0.9 + 0.1, 0, 1)) * span * 0.85;
    } else {
      continue;                           // 'point': the taper is the tip
    }
    const sv = sEnd - (yTip - y);
    if (s.tip === 'fan') {
      // Upright: from the ground (never below it) to `half` over the tail.
      const top = Math.round(cz + half), bot = Math.max(0, Math.round(cz - half * 0.35));
      for (let z = bot; z <= top; z++)
        for (const x of [-1, 0]) add(x, y, z, SLOTS.FIN, sv);
    } else {
      const zs = s.tip === 'fluke' ? [Math.max(0, Math.floor(cz))]
                                   : [Math.max(0, Math.floor(cz)),
                                      Math.max(0, Math.floor(cz)) + 1];
      for (let x = 0; x < Math.round(half); x++) {
        if (x + 0.5 < notch && t > 0.55) continue;
        for (const z of zs) {
          add(x, y, z, SLOTS.FIN, sv);
          add(-x - 1, y, z, SLOTS.FIN, sv);
        }
      }
    }
  }
  void h0;
}

/** SHAPES' answer for a tail part: the cells extendTable cut for it. */
export function shape(g, name, table) {
  const cs = table.serpent && table.serpent.partCells[name];
  if (!cs) throw new Error(`serpent: no cells for ${name} (extendTable ran?)`);
  return cs.map(c => [...c]);
}

// =============================================================================
// THE SIDECAR: the tail's limbs, its slither, and the states and clips a body
// with no legs needs instead of the human's.
// =============================================================================

const qr = v => Math.round(v * 1e4) / 1e4;
const qaa = (deg, ax) => {
  const l = Math.hypot(ax[0], ax[1], ax[2]) || 1;
  const h = deg * Math.PI / 360, sn = Math.sin(h);
  return [qr(ax[0] / l * sn), qr(ax[1] / l * sn), qr(ax[2] / l * sn),
          qr(Math.cos(h))];
};
const qx = d => qaa(d, [1, 0, 0]);
const qy = d => qaa(d, [0, 1, 0]);
const qz = d => qaa(d, [0, 0, 1]);
const qmul = (a, b) => [
  qr(a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1]),
  qr(a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0]),
  qr(a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3]),
  qr(a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2])];
/** An upper arm, from hanging: raised `fwd` degrees forward, then out `out`
 *  degrees to its own side (model +X is the LEFT). */
const arm = (side, fwd, out = 0) =>
  qmul(qz(side === 'L' ? out : -out), qx(-fwd));
/** A key list from [t, q] pairs, eased. */
const rot = (pairs, ease = 'quadInOut') =>
  ({ rot: pairs.map(([t, q], i) =>
    (i < pairs.length - 1 ? { t, q, ease } : { t, q })) });
const hold = (q, dur) => rot([[0, q], [dur, q]]);
const mirrorQ = q => [q[0], qr(-q[1]), qr(-q[2]), q[3]];

/** The upper body every part list starts from. */
const UPPER = ['hips', 'torso', 'head', 'armU.L', 'armL.L', 'hand.L',
               'armU.R', 'armL.R', 'hand.R'];

/** The limb rows for the tail. `anchor(scene)` and `hpScale(name)` are
 *  buildSidecar's: the shipped-unit anchor and a volume-scaled hp. */
export function limbs(g, table, anchor, U) {
  const J = table.serpent.joints;
  const vol = n => { const z = table.limbs[n].size;
                     return z[0] * z[1] * z[2]; };
  const ref = vol('tail3');
  const out = [];
  for (let i = 0; i < PARTS.length; i++) {
    const n = PARTS[i];
    const j = J[n];
    const trailing = i >= RISE.length;
    const hp = n === 'tail1' ? 150 : n === 'tail2' ? 110
      : Math.max(18, Math.round(70 * Math.sqrt(vol(n) / ref)));
    const limb = {
      name: n, parent: i === 0 ? 'hips' : PARTS[i - 1], joint: 'ball',
      hp, severable: true,
      // The waist is the spine's (it shares the look and the aim twist, so a
      // naga turns at the waist to face what it fights); the rest is tail.
      tag: n === 'tail1' ? 'spine' : 'tail',
      anchor: anchor([j[0] * U, j[1] * U, j[2] * U]),
      severImpactSpeed: n === 'tail1' ? 22 : n === 'tail2' ? 20 : 15,
    };
    if (trailing) limb.trailing = true;
    out.push(limb);
  }
  return out;
}

/** The slither block, land and water. */
export function slither(g) {
  const s = g.serpent;
  return {
    parts: [...TRAIL],
    upright: true,
    wavelength: pr(0.32 + 0.45 * s.waveLength),
    amplitude: pr(0.02 + 0.07 * s.wave),
    ampHead: 0.05,
    ampRamp: 0.55,
    turnBend: 1,
    maxCurvature: 0.04,
    bendHalfLife: 0.25,
    water: {
      axis: s.swim === 'kick' ? 'pitch' : 'yaw',
      beatHz: pr(0.5 + 1.1 * s.kick),
      slip: 0.7,
      ampIdle: 0.35,
      ampSpeed: 0.3,
      wavelength: s.swim === 'kick' ? pr(0.9 + 0.4 * s.waveLength)
                                    : pr(0.45 + 0.4 * s.waveLength),
      amplitude: pr(0.035 + 0.05 * s.kick),
      ampHead: 0.0,
      ampRamp: s.swim === 'kick' ? 1.0 : 0.6,
    },
  };
}
const pr = v => Math.round(v * 1000) / 1000;

/** The human's state ladder, re-predicated for a body whose locomotion is a
 *  tail. Same NAMES and ORDER, so the human's list is the base it is a
 *  patch of: the leg-loss states that have no meaning here (crawl.shins,
 *  stumps) are left out, which the diff writes as tombstones. */
export function states(humanStates) {
  const byName = Object.fromEntries(humanStates.map(s => [s.name, s]));
  const trail = TRAIL;
  return [
    // The tail gone from the rise down (tail3 and everything after it) and an
    // arm gone as well: the squirm.
    { ...byName.squirm, missing: ['tail3'], missingAny: ['armU.L', 'armU.R'] },
    // The tail gone: an army crawl on the arms, the man's.
    { ...byName.crawl, missing: ['tail3'] },
    // Half the tail or more gone: it still slithers, slower and unbalanced --
    // listing to one side with the arms out (`taillimp`); the wave on what is
    // left is still the gait.
    { name: 'limp', missingAny: trail.slice(1, 6), clip: 'taillimp',
      speedScale: 0.6 },
    byName.onearm,
    byName.headless,
    // ASLEEP: coiled where it lies, upright, not stretched on the ground.
    { name: 'sleep', activity: 'sleep', clip: 'coil', speedScale: 0,
      disableGait: true, bodyYOffset: 0 },
    byName.prone,
  ].filter(Boolean);
}

/** The overlay states (kept playing under every other state). */
export function overlayStates(g) {
  const s = g.serpent;
  return (s.sway > 0.02 || s.tipFlick > 0.02)
    ? [{ name: 'serpentsway', overlay: true, clip: 'serpentsway' }] : [];
}

/**
 * THE CLIPS A TAIL NEEDS. walk / run: a slithering body's carriage (no stride
 * to lock to: the tail's wave is the gait); `serpentsway`: the idle sway and
 * tip flick, always; `coil`: asleep; and the SWIM FAMILY -- tread, swim,
 * swim_back, swim_side (+ .mirror), swim_up, swim_down -- authored for a body
 * whose legs are a tail. Every stroke holds tail2 and tail3 at -45 degrees,
 * which lays the rise in line with the waist and the trail in line with the
 * rise: the whole body is one line, the hips' pitch lays it out along the
 * stroke (the engine's swimPitch), and the water wave kicks the rest. The
 * hips' pitch and lift are the human strokes' own (assets/anims/swim*.json):
 * the engine's waterline shares and the dive pitch (pose.cpp kSwimCrawlPitch)
 * are stated about them. Positions are sidecar units (10 per metre).
 */
export function clips(g) {
  const s = g.serpent;
  const out = {};
  const D = 1600;                          // the swim family's one period
  const STRAIGHT = qx(-45);
  const T = [0, 400, 800, 1200, 1600];
  const both = (fn) => ({
    'armU.L': rot(T.map((t, i) => [t, fn(i, 'L').u])),
    'armL.L': rot(T.map((t, i) => [t, qx(-fn(i, 'L').f)])),
    'armU.R': rot(T.map((t, i) => [t, fn(i, 'R').u])),
    'armL.R': rot(T.map((t, i) => [t, qx(-fn(i, 'R').f)])),
  });
  const swimMask = [...UPPER, 'tail1', 'tail2', 'tail3'];
  const stroke = (note, hipsQ, hipsPos, torsoQ, headQ, arms, extra = {}) => ({
    '//': note,
    durationMs: D, loop: true, mode: 'override', blendInMs: 250,
    blendOutMs: 300, mask: [...swimMask],
    tracks: {
      hips: { ...rot(T.map((t, i) => [t, hipsQ(i)])),
              pos: T.map((t, i) => ({ t, v: [0, hipsPos(i), 0] })) },
      torso: rot(T.map((t, i) => [t, torsoQ(i)])),
      head: rot(T.map((t, i) => [t, headQ(i)])),
      ...arms,
      // The waist holds still: a rock here swings three metres of tail.
      tail1: hold(qx(0), D),
      tail2: hold(STRAIGHT, D),
      tail3: hold(STRAIGHT, D),
      ...extra,
    },
  });
  const C = i => i % 4;                    // keys 0 and 4 are the same pose
  // TREAD: upright at the surface, the hands sculling in and out twice a
  // cycle, the tail hanging straight below and kicking.
  out.tread = stroke(
    'TREADING WATER on a tail: upright, hands sculling in front at chest ' +
    'height twice a cycle, the tail hanging straight down beneath and ' +
    'kicking (the slither.water wave). Generated (serpent.js clips).',
    () => qx(6), i => [0, 0.26, 0.26, 0.1, 0][i], () => qx(4), () => qx(-6),
    both((i, sd) => i % 2 ? { u: arm(sd, 50, 28), f: 58 }
                          : { u: arm(sd, 58, 8), f: 46 }));
  // SWIM: a mermaid's stroke. Both arms together -- reach, sweep out and
  // back to the sides, glide with them along the body, draw them up the
  // chest and shoot them forward -- while the tail drives. The hips pitch
  // 70.9 degrees and lift 0.3 m like the human crawl's.
  out.swim = stroke(
    'SWIMMING FORWARD on a tail: a mermaid\'s stroke. Both arms reach ahead, ' +
    'sweep out and back to the sides, glide along the body, draw up the ' +
    'chest and shoot forward again, while the tail drives from the hips ' +
    'down. The hips\' 70.9-degree pitch and 0.3 m lift are the human ' +
    'crawl\'s (pose.cpp kSwimCrawlPitch is stated about them).',
    () => qx(70.9), () => 3.0,
    i => qx([-4, 2, 6, 0, -4][i]), i => qx([-22, -18, -20, -24, -22][i]),
    both((i, sd) => [{ u: arm(sd, 168, 8), f: 5 },
                     { u: arm(sd, 110, 62), f: 14 },
                     { u: arm(sd, 12, 22), f: 8 },
                     { u: arm(sd, 70, 6), f: 125 },
                     { u: arm(sd, 168, 8), f: 5 }][i]));
  // BACK: upright and leaning back, both hands pushing the water away in
  // front, the tail beneath.
  out.swim_back = stroke(
    'SWIMMING BACKWARD on a tail: upright, leaning back 15 degrees, both ' +
    'hands pushing the water away in front and sweeping round; the tail ' +
    'beneath does the work.',
    () => qx(-15), i => [0.5, 0.8, 0.4, 0.6, 0.5][i], () => qx(-5),
    () => qx(10),
    both((i, sd) => [{ u: arm(sd, 45, 20), f: 110 }, { u: arm(sd, 70, 6), f: 32 },
                     { u: arm(sd, 75, 40), f: 7 }, { u: arm(sd, 30, 50), f: 50 },
                     { u: arm(sd, 45, 20), f: 110 }][i]));
  // SIDE (travelling to the body's right; the mirror is derived below).
  out.swim_side = stroke(
    'SWIMMING SIDEWAYS on a tail, travelling to the body\'s RIGHT: leaning ' +
    'into the travel, the leading (right) arm reaching out overhead and ' +
    'pulling down to the hip, the other sculling at the chest. The left is ' +
    'swim_side.mirror.',
    () => qaa(21, [0.48, 0.08, 0.87]), i => [0.5, 0.74, 0.86, 0.7, 0.5][i],
    () => qz(6), () => qz(-12),
    {
      'armU.L': rot(T.map((t, i) => [t, [arm('L', 52, 12), arm('L', 40, 50),
                                          arm('L', 10, 38), arm('L', 36, 18),
                                          arm('L', 52, 12)][i]])),
      'armL.L': rot(T.map((t, i) => [t, qx(-[100, 38, 0, 76, 100][i])])),
      'armU.R': rot(T.map((t, i) => [t, [arm('R', 20, 150), arm('R', 25, 112),
                                          arm('R', 15, 46), arm('R', 31, 5),
                                          arm('R', 20, 150)][i]])),
      'armL.R': rot(T.map((t, i) => [t, qx(-[20, 60, 90, 30, 20][i])])),
    });
  // UP: upright, a vertical breaststroke overhead.
  out.swim_up = stroke(
    'SWIMMING UP on a tail: upright, chin up, the arms reaching overhead and ' +
    'pulling down and out to the sides, the tail driving beneath.',
    () => qx(0), i => [0, -0.35, -0.22, -0.1, 0][i], () => qx(-4),
    () => qx(-25),
    both((i, sd) => [{ u: arm(sd, 165, 8), f: 5 }, { u: arm(sd, 100, 40), f: 20 },
                     { u: arm(sd, 27, 20), f: 30 }, { u: arm(sd, 69, 14), f: 120 },
                     { u: arm(sd, 165, 8), f: 5 }][i]));
  // DOWN: a head-first dive, the hips past horizontal like the human's.
  out.swim_down = stroke(
    'SWIMMING DOWN on a tail: a head-first dive, the hips pitched 115 ' +
    'degrees like the human\'s, the arms pulling toward the tail.',
    () => qx(115), () => 2.0, i => qx(i % 2 ? 4 : -4), () => qx(-25),
    both((i, sd) => [{ u: arm(sd, 165, 8), f: 5 }, { u: arm(sd, 100, 40), f: 20 },
                     { u: arm(sd, 27, 20), f: 30 }, { u: arm(sd, 69, 14), f: 120 },
                     { u: arm(sd, 165, 8), f: 5 }][i]));
  out['swim_side.mirror'] = mirrorClip(out.swim_side,
    'swim_side, travelling LEFT: the mirror of swim_side (every .L/.R part ' +
    'swapped and every rotation reflected through the body\'s midplane, as ' +
    'the clip library derives one).');
  void C;

  // ---- on land ---------------------------------------------------------------
  // WALK / RUN: a carriage, not a stride. The arms swing a little and the
  // waist and torso counter-turn; the tail's wave is the gait.
  const carriage = (period, swing, fore, twist, lean) => {
    const h = Math.floor(period / 2);
    const sw = (a, b) => rot([[0, a], [h, b], [period, a]]);
    return {
      durationMs: period, loop: true, mode: 'additive', blendInMs: 200,
      blendOutMs: 200,
      mask: ['armU.L', 'armL.L', 'armU.R', 'armL.R', 'torso', 'head', 'tail1'],
      tracks: {
        'armU.L': sw(qx(swing), qx(-swing)),
        'armU.R': sw(qx(-swing), qx(swing)),
        'armL.L': sw(qx(-fore - 4), qx(-fore + 4)),
        'armL.R': sw(qx(-fore + 4), qx(-fore - 4)),
        torso: sw(qmul(qx(lean), qy(twist)), qmul(qx(lean), qy(-twist))),
        tail1: sw(qy(-twist * 1.4), qy(twist * 1.4)),
        head: sw(qy(-twist * 0.6), qy(twist * 0.6)),
      },
    };
  };
  out.walk = carriage(1500, 7, 14, 3.5, 2);
  out.run = carriage(950, 13, 38, 5, 8);
  // THE SWAY, always (an overlay state): the waist leans a little side to
  // side and the torso and head counter it, and the tip flicks twice a
  // cycle. Additive, so it rides the wave, the walk and the swim alike.
  const sw = s.sway, fl = s.tipFlick;
  if (sw > 0.02 || fl > 0.02) {
    const P = 4000;
    const tracks = {};
    if (sw > 0.02) {
      tracks.tail1 = rot([[0, qz(2.5 * sw)], [P / 2, qz(-2.5 * sw)], [P, qz(2.5 * sw)]]);
      tracks.torso = rot([[0, qz(-2 * sw)], [P / 2, qz(2 * sw)], [P, qz(-2 * sw)]]);
      tracks.head = rot([[0, qz(-1 * sw)], [P / 2, qz(1 * sw)], [P, qz(-1 * sw)]]);
    }
    if (fl > 0.02) {
      const f = 28 * fl;
      tracks.tail12 = rot([[0, qy(0)], [300, qy(f)], [650, qy(-f * 0.6)],
                           [1000, qy(0)], [2400, qy(0)], [2700, qy(-f)],
                           [3050, qy(f * 0.6)], [3400, qy(0)], [P, qy(0)]]);
      tracks.tail11 = rot([[0, qy(0)], [400, qy(f * 0.4)], [1000, qy(0)],
                           [2500, qy(0)], [2800, qy(-f * 0.4)], [3400, qy(0)],
                           [P, qy(0)]]);
    }
    out.serpentsway = {
      durationMs: P, loop: true, mode: 'additive', blendInMs: 400,
      blendOutMs: 400, mask: Object.keys(tracks), tracks,
    };
  }
  // TAILLIMP: half the tail gone. Additive over the carriage: the body lists
  // to one side and rocks back, the arms held out for balance.
  {
    const P = 1800, h = P / 2;
    out.taillimp = {
      durationMs: P, loop: true, mode: 'additive', blendInMs: 300,
      blendOutMs: 300, mask: ['torso', 'tail1', 'armU.L', 'armU.R', 'head'],
      tracks: {
        tail1: rot([[0, qz(7)], [h, qz(4)], [P, qz(7)]]),
        torso: rot([[0, qmul(qz(-4), qx(5))], [h, qmul(qz(-1), qx(8))],
                    [P, qmul(qz(-4), qx(5))]]),
        head: hold(qz(-3), P),
        'armU.L': rot([[0, qz(16)], [h, qz(24)], [P, qz(16)]]),
        'armU.R': rot([[0, qz(-24)], [h, qz(-16)], [P, qz(-24)]]),
      },
    };
  }
  // COIL: asleep. The trail curls round in a tightening spiral beside the
  // body, the torso slumps over it and the arms fold.
  {
    const P = 4200, tracks = {};
    TRAIL.forEach((n, k) => { tracks[n] = hold(qy(19 + 3.2 * k), P); });
    tracks.torso = rot([[0, qx(26)], [P / 2, qx(30)], [P, qx(26)]]);
    tracks.head = hold(qx(22), P);
    tracks.tail1 = hold(qx(-8), P);
    for (const sd of ['L', 'R']) {
      tracks[`armU.${sd}`] = hold(arm(sd, 38, 12), P);
      tracks[`armL.${sd}`] = hold(qx(-100), P);
    }
    out.coil = {
      '//': 'ASLEEP on a tail: the trail curls into a spiral, the torso slumps ' +
            'over it, the arms fold. Held by the `sleep` activity state.',
      durationMs: P, loop: true, mode: 'override', blendInMs: 900,
      blendOutMs: 700, mask: Object.keys(tracks), tracks,
    };
  }
  return out;
}

/** A clip reflected through the body's midplane: .L/.R swapped, every
 *  rotation (x, -y, -z, w), every position (-x, y, z). mob.cpp's
 *  MirrorClipDoc, for a clip the generator owns. */
function mirrorClip(c, note) {
  const sw = n => n.endsWith('.L') ? n.slice(0, -2) + '.R'
                : n.endsWith('.R') ? n.slice(0, -2) + '.L' : n;
  const tracks = {};
  for (const [n, tr] of Object.entries(c.tracks)) {
    const t2 = {};
    if (tr.rot) t2.rot = tr.rot.map(k => ({ ...k, q: mirrorQ(k.q) }));
    if (tr.pos) t2.pos = tr.pos.map(k => ({ ...k, v: [-k.v[0] || 0, k.v[1], k.v[2]] }));
    tracks[sw(n)] = t2;
  }
  return { ...c, '//': note, mask: c.mask.map(sw), tracks };
}

/** Land speed, world voxels a second at the sidecar's 10 per metre. */
export function speed(g, sprintMps) {
  return Math.round(sprintMps * 10 * g.serpent.speed * 1e4) / 1e4;
}
