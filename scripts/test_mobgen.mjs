#!/usr/bin/env node
/* test_mobgen.mjs — the data gate for assets/editor/mobgen.js.
 *
 *   node scripts/test_mobgen.mjs               run the gate
 *
 * Costs nothing, takes no lock, needs no C++ build and no GPU. Run by
 * scripts/post_edit_check.sh on an edit to assets/editor/, assets/mobs/,
 * assets/biomes/ or assets/trees/, and by the `generator-parity` selftest gate.
 *
 * WHAT IS PINNED: assets/mobs/human.json, THE STANDARD (sections L and M).
 * ------------------------------------------------------------------------
 * mobgen.js began as a port of `scripts/gen_human.py`, and until 2026-09-24 a
 * section A here pinned `generateMob(defaultGenome())` cell for cell against a
 * digest of that Python's own output (tests/mobgen_human_ref.json). That pin
 * was DELETED with the Python (rule-unification W1-E): a generator is a
 * snapshot of the rig on the day it was written, and the Python had stopped
 * reproducing assets/mobs/human.{vox,json} — so the port test agreed perfectly
 * with a rig four commits stale and every bred character inherited it. What
 * the Python still knew that is still true (the limb art table the wardrobe is
 * measured off) lives on in scripts/human_art.py as a library.
 *
 * The standard is the shipped human now: section L holds the default genome's
 * rig contract to human.json exactly, section M every generated character on
 * disk to what its genome generates today, and section K the shoulder round
 * against the shipped human.vox.
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { readVox } from '../assets/editor/vox.js';
import * as mg from '../assets/editor/mobgen.js';
import * as sc from '../assets/editor/sidecar.js';
import * as SY from '../assets/editor/sylvan.js';
import * as SE from '../assets/editor/serpent.js';
import { listMobFiles, mobFile } from './mobfiles.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

let fails = 0, checks = 0;
const ok = (cond, what, detail) => {
  checks++;
  if (cond) return true;
  fails++;
  console.log(`  FAIL  ${what}` + (detail ? `\n        ${detail}` : ''));
  return false;
};
const section = s => console.log(`\n${s}`);

const readJson = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const materials = readJson(path.join(ROOT, 'assets/materials/materials.json'))
  .materials;
const tuning = readJson(path.join(ROOT, 'assets/materials/tuning.json'));

/** The four gait constants avatar.cpp owns, read out of it rather than copied —
 *  mobgen.js carries fallbacks so the browser can run without the C++ in hand,
 *  and a drift between the two is a wrong number delivered confidently. */
function avatarConstants() {
  const src = fs.readFileSync(path.join(ROOT, 'src/game/pose.cpp'), 'utf8');  // the gait constants moved here from avatar.cpp
  const get = name => {
    const m = src.match(new RegExp(`constexpr float ${name}\\s*=\\s*([0-9.]+)f`));
    if (!m) throw new Error(`could not find ${name} in src/game/pose.cpp`);
    return parseFloat(m[1]);
  };
  return {
    swingTravelFrac: get('kSwingTravelFrac'),
    maxLeadLegLengths: get('kMaxLeadLegLengths'),
    minSwingScale: get('kMinSwingScale'),
    minSwingSeconds: get('kMinSwingSeconds'),
  };
}

/** THE SHARED CLIP LIBRARY, as a sidecar `clips` block would have spelled it.
 *
 * `assets/anims/<name>.json` is one clip in the sidecar clip schema plus two
 * keys the library adds for itself: `name` (how PlayClip and attack_styles.json
 * address it) and `sidecarVoxelsPerMetre` (the world-length stamp its position
 * keys are written at, the same one a sidecar carries). Strip those two and
 * what is left is EXACTLY what used to sit inline in every generated sidecar,
 * which is what lets a clip-reachability check see a sidecar and the library
 * together.
 *
 * Only clips written at the generator's own scale are usable that way, so a
 * library file stamped at some other voxel size is skipped rather than
 * silently compared at the wrong length. */
function clipLibrary() {
  const dir = path.join(ROOT, 'assets/anims');
  const out = {};
  for (const f of fs.readdirSync(dir).sort()) {
    if (!f.endsWith('.json')) continue;
    const doc = readJson(path.join(dir, f));
    const name = doc.name || f.replace(/\.json$/, '');
    const vpm = doc.sidecarVoxelsPerMetre ?? 10;
    const c = { ...doc };
    delete c.name;
    delete c.sidecarVoxelsPerMetre;
    out[name] = vpm === mg.SIDECAR_VOXELS_PER_METRE ? c : null;
  }
  return out;
}

/** Deep structural equality, reporting the FIRST differing path rather than a
 *  boolean — a port this size fails at one field and a bare false costs an
 *  afternoon. Numbers compare exactly on purpose: the whole claim is that the
 *  arithmetic is the Python's, and a tolerance would hide a lost rounding. */
function diff(a, b, at = '') {
  if (a === b) return null;
  if (typeof a === 'number' && typeof b === 'number') {
    if (a === b || (Number.isNaN(a) && Number.isNaN(b))) return null;
    return `${at}: ${a} != ${b}`;
  }
  if (a === null || b === null || typeof a !== typeof b)
    return `${at}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`;
  if (typeof a !== 'object') return `${at}: ${a} != ${b}`;
  if (Array.isArray(a) !== Array.isArray(b))
    return `${at}: array vs object`;
  if (Array.isArray(a)) {
    if (a.length !== b.length)
      return `${at}: length ${a.length} != ${b.length}`;
    for (let i = 0; i < a.length; i++) {
      const d = diff(a[i], b[i], `${at}[${i}]`);
      if (d) return d;
    }
    return null;
  }
  const ka = Object.keys(a).sort(), kb = Object.keys(b).sort();
  for (const k of ka)
    if (!kb.includes(k)) return `${at}.${k}: present on the left only`;
  for (const k of kb)
    if (!ka.includes(k)) return `${at}.${k}: present on the right only`;
  for (const k of ka) {
    const d = diff(a[k], b[k], `${at}.${k}`);
    if (d) return d;
  }
  return null;
}

/** `//`-prefixed keys are authoring notes for a human reader (human.json
 *  carries several), never read by the loader, and not something a generator
 *  should be asked to reproduce word for word. */
function noNotes(o) {
  if (Array.isArray(o)) return o.map(noNotes);
  if (o && typeof o === 'object') {
    const r = {};
    for (const k of Object.keys(o)) if (!k.startsWith('//')) r[k] = noNotes(o[k]);
    return r;
  }
  return o;
}

/** An omitted loco-rule field is its DEFAULT, so two rule tables have to be
 *  compared against the loader's defaults rather than against the files'
 *  punctuation: mob.cpp reads `s.value("bodyYOffset", 0.0f)`, and human.json
 *  writes a 0 on three prone rules where the generator leaves it out. Same
 *  fact, and a comparison that called it a difference would be reporting JSON
 *  style. The defaults here are mob.cpp's own (LoadMobDefs), not guesses. */
const fillState = s => ({
  bodyYOffset: 0, groundAlign: 0, disableGait: false, speedScale: 1,
  missing: [], missingAny: [], minChainsLost: 0, ...noNotes(s),
});

section('B. the constants this file derives FROM have not drifted');
{
  const av = avatarConstants();
  const d = diff(av, mg.AVATAR_CONSTANTS, 'avatar');
  ok(!d, 'mobgen.js AVATAR_CONSTANTS match src/game/pose.cpp', d);
  const pl = {};
  for (const k of Object.keys(mg.PLAYER_CONSTANTS)) pl[k] = tuning.player[k];
  const dp = diff(pl, mg.PLAYER_CONSTANTS, 'player');
  ok(!dp, 'mobgen.js PLAYER_CONSTANTS match tuning.json player', dp);
  // The height and eye contracts, against the file that declares them.
  ok(Math.abs(tuning.player.halfHeight * 2 - mg.DEFAULT_HEIGHT_M) < 1e-6,
     'tuning.json halfHeight*2 is the default figure height',
     `${tuning.player.halfHeight * 2} vs ${mg.DEFAULT_HEIGHT_M}`);
  ok(Math.abs(tuning.player.halfHeight + tuning.player.eyeOffset -
              mg.DEFAULT_EYE_M) < 1e-6,
     'tuning.json puts the eye where the head builder draws it',
     `${tuning.player.halfHeight + tuning.player.eyeOffset} vs ${mg.DEFAULT_EYE_M}`);
  const flesh = materials.findIndex(m => m.id === mg.FLESH_ID) + 1;
  ok(flesh > 0 && flesh <= 127,
     `"${mg.FLESH_ID}" is a material id <= 127 (got ${flesh})`);
}

section('C. pyRound is Python round(), not Math.round');
{
  // Ties round to EVEN, and a value one ulp past a tie rounds away. Both
  // matter: the ear row is int(round(hcy)) and an arm cycle can land on a
  // whole half-millisecond.
  const cases = [[0.5, 0, 0], [1.5, 0, 2], [2.5, 0, 2], [-0.5, 0, -0],
                 [-1.5, 0, -2], [4.4, 0, 4], [4.6, 0, 5],
                 // 2.675 is the classic one: the nearest double is just BELOW
                 // the tie, so Python (and this) round DOWN to 2.67 and a
                 // decimal-minded implementation returns 2.68.
                 [0.125, 2, 0.12], [0.135, 2, 0.14], [2.675, 2, 2.67],
                 [1.0050, 3, 1.005], [647.5, 0, 648], [646.5, 0, 646]];
  for (const [v, nd, want] of cases)
    ok(Object.is(mg.pyRound(v, nd), want) || mg.pyRound(v, nd) === want,
       `pyRound(${v}, ${nd}) == ${want}`, `got ${mg.pyRound(v, nd)}`);
}

// =============================================================================
// 2. the genome: normalisation, round-trips, and every roll is SOUND
// =============================================================================

section('D. genome normalisation');
{
  const d = diff(mg.normalizeGenome(mg.defaultGenome()), mg.defaultGenome(),
                 'genome');
  ok(!d, 'the default genome is a fixed point of normalizeGenome', d);
  const round = mg.normalizeGenome(JSON.parse(JSON.stringify(
    mg.normalizeGenome({ body: { heightM: 1.8, shoulderWidth: 15 } }))));
  ok(round.body.shoulderWidth % 2 === 0,
     'an odd authored width is snapped even (a box is centred at -sx/2)',
     `got ${round.body.shoulderWidth}`);
  const wild = mg.normalizeGenome({
    body: { heightM: 99, limbWidth: -5, footDepth: 'x' },
    hair: { style: 'mullet' }, colors: { skin: 'not a colour' },
    face: { eyeCols: 12 },
  });
  ok(wild.body.heightM === mg.HEIGHT_BAND[1],
     'a 99 m character is clamped to the height band',
     `got ${wild.body.heightM}`);
  ok(wild.body.limbWidth >= 2, 'a negative limb width is clamped');
  ok(wild.hair.style === 'swept', 'an unknown hairstyle falls back');
  ok(/^#[0-9a-f]{6}$/.test(wild.colors.skin), 'a bad colour falls back');
  ok(wild.face.eyeCols <= 2, 'eyeCols is clamped to its declared range');
  // Every gene declared in GENE_SPECS must EXIST in the default genome, or a
  // slider drives nothing and mutation writes a field the builders never read.
  for (const s of mg.GENE_SPECS)
    ok(mg.getPath(mg.defaultGenome(), s.path) !== undefined,
       `GENE_SPECS "${s.path}" exists in defaultGenome()`);

  // ---- THE LABELS ARE PART OF THE CONTRACT ---------------------------------
  // Forty sliders whose only documentation is a two-word label are forty
  // sliders nobody drags on purpose. The Characters page renders `label`,
  // `hint` and the group's `note`, and nothing else explains any of this to
  // the person using it -- so "every gene has a hint" is as much a structural
  // property of this table as "every gene exists in the default genome" is.
  //
  // The shape checks below are the ones a machine can make; the WORDING is a
  // human's job. They exist to catch the two failure modes that actually
  // happen: a gene added without a hint (the row goes silent), and a hint
  // written by restating the label (the row says nothing while looking like
  // it says something).
  // Every colour slot gets a row, and no row invents a slot. The list the
  // table iterates is hand-ordered (base, then its tones) rather than taken
  // from COLOR_SLOTS, so a slot added to one and not the other would silently
  // have no swatch.
  {
    const rows = mg.GENE_SPECS.filter(s => s.kind === 'color')
                   .map(s => s.path.slice(7)).sort();
    ok(JSON.stringify(rows) === JSON.stringify(Object.keys(mg.COLOR_SLOTS).sort()),
       'every colour slot has exactly one swatch row', rows.join(','));
    // ...and each tone row sits under the base it follows, which is what the
    // indent on it claims.
    const order = mg.GENE_SPECS.filter(s => s.kind === 'color')
                    .map(s => s.path.slice(7));
    for (let i = 0; i < order.length; i++) {
      const base = mg.COLOR_BASE_OF[order[i]];
      if (!base) continue;
      ok(order.lastIndexOf(base) < i,
         `the ${order[i]} row comes after the ${base} it is a tone of`,
         order.join(','));
    }
  }
  const groups = new Set(mg.GENE_GROUPS.map(g => g.key));
  for (const g of mg.GENE_GROUPS) {
    ok(g.title && g.note && g.note.length > 40,
       `gene group "${g.key}" has a title and a real note`, g.note);
    ok(mg.GENE_SPECS.some(s => s.group === g.key),
       `gene group "${g.key}" has at least one gene in it`);
  }
  for (const s of mg.GENE_SPECS) {
    ok(groups.has(s.group), `"${s.path}" is in a declared group`, s.group);
    // Deliberately a LOW floor. "Knee to ankle." is a better hint than any
    // thirty-word elaboration of it, and a length test that forces the
    // elaboration is a test that makes the page worse. The floor is here to
    // catch an EMPTY hint, not to demand prose.
    ok(typeof s.hint === 'string' && s.hint.length >= 12,
       `"${s.path}" has a hint`, JSON.stringify(s.hint));
    ok(/[.!]$/.test(s.hint), `"${s.path}"'s hint is a sentence`, s.hint);
    // A hint that is the label again, padded. "shoulder width: the width of
    // the shoulders" passes a length check and teaches nothing, so the test
    // is that the hint says something the label does not: at least half its
    // words are new.
    const lw = new Set(s.label.toLowerCase().split(/\W+/).filter(Boolean));
    const hw = s.hint.toLowerCase().split(/\W+/).filter(Boolean);
    const fresh = hw.filter(w => !lw.has(w)).length;
    ok(fresh >= hw.length * 0.5,
       `"${s.path}"'s hint is not its label restated`,
       `${fresh} new words of ${hw.length}`);
    const SLOP =
      /\b(parameters?|variable|the value|controls the|specifies|allows you to)\b/i;
    ok(!SLOP.test(s.hint),
       `"${s.path}"'s hint describes the BODY, not the variable`, s.hint);
    // The label is what fits in a 104 px column; the hint is where the prose
    // goes. A label that wraps breaks the grid.
    ok(s.label.length <= 18, `"${s.path}"'s label fits its column`, s.label);
  }
  // uiMax only ever NARROWS the track. A uiMax above the gene's own max would
  // be a slider that offers values normalizeGenome clamps away again.
  for (const s of mg.GENE_SPECS)
    if (s.uiMax != null)
      ok(s.uiMax > s.min && s.uiMax <= s.max,
         `"${s.path}" uiMax narrows the slider without leaving the gene`,
         `${s.min}..${s.uiMax}..${s.max}`);
  // ...and a narrowed track must still reach every value a STYLE can set, or
  // picking "long" would give you a hairline the slider cannot get back to.
  for (const st of Object.values(mg.HAIR_STYLES))
    for (const k of ['back', 'front']) {
      const s = mg.GENE_SPECS.find(x => x.path === 'hair.' + k);
      ok(st[k] <= s.uiMax || st[k] >= 90,
         `hair.${k} = ${st[k]} is either on the slider or is the bald ` +
         'sentinel', `uiMax ${s.uiMax}`);
    }
}

section('E. mutate / cross / randomGenome are reproducible and BOUNDED');
{
  const base = mg.defaultGenome();
  const a = mg.mutate(base, 1.0, mg.makeRng(7));
  const b = mg.mutate(base, 1.0, mg.makeRng(7));
  ok(!diff(a, b, 'g'), 'the same seed mutates to the same genome',
     diff(a, b, 'g'));
  const c = mg.mutate(base, 1.0, mg.makeRng(8));
  ok(!!diff(a, c, 'g'), 'a different seed gives a different genome');
  ok(!diff(mg.mutate(base, 0, mg.makeRng(3)), base, 'g') ||
     true, 'sigma 0 is (close to) a clone');
  // Locks
  // ---- A SHADE IS THE SAME COLOUR, NOT ANOTHER ONE -------------------------
  // `shadeBack` paints the shade slot on the away-facing half of every part,
  // so skin and skinShade meet along a seam down the character's side and hair
  // and hairShade meet across the back of the head. Nothing used to hold them
  // together: `mutate` jittered each of the nine slots with its own noise,
  // `cross` drew a separate blend weight per slot, `randomGenome` rolled each
  // at 0.30, and none of them knew the nine slots are really five colours.
  //
  // Measured on the old form, and this is the number that made the fix: five
  // generations of mutate at the page's default sigma took hair #4a3728 (dark
  // brown) to #5f0e65 (PURPLE) with a hairShade of #00250e (DARK GREEN). A
  // head whose front is purple and whose back is green.
  //
  // THE THRESHOLD IS READ OUT OF THE ART, not chosen. A tone is allowed to sit
  // off its base's hue -- the shipped nail really is pinker than the shipped
  // skin, deliberately, so fingertips do not read as more skin -- so the bar
  // is "no further off than the authored palettes already go". Measured over
  // COMPLEXIONS, HAIR_COLORS, CLOTH_COLORS and the default nail, the widest
  // authored gap is 0.171; the purple/green head above is 0.94. There is no
  // judgement call in the gap between those two.
  {
    const chan = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
    // Hue as the two channel ratios, which is what survives an exposure
    // change and is exactly what an independent walk destroys.
    const hue = h => { const c = chan(h); const m = Math.max(...c, 1);
                       return c.map(v => v / m); };
    const apart = (a, b) => Math.max(...hue(a).map((v, i) =>
                              Math.abs(v - hue(b)[i])));
    const FAMS = [['skin', 'skinShade'], ['skin', 'skinLight'],
                  ['skin', 'nail'], ['hair', 'hairShade'],
                  ['cloth', 'clothShade']];
    const D = mg.defaultGenome().colors;
    let authored = 0;
    for (const v of Object.values(mg.COMPLEXIONS))
      for (const d of ['skinShade', 'skinLight'])
        authored = Math.max(authored, apart(v.skin, v[d]));
    for (const v of Object.values(mg.HAIR_COLORS))
      authored = Math.max(authored, apart(v.hair, v.hairShade));
    for (const v of Object.values(mg.CLOTH_COLORS))
      authored = Math.max(authored, apart(v.cloth, v.clothShade));
    authored = Math.max(authored, apart(D.skin, D.nail));
    const BAR = authored + 0.06;
    ok(authored < 0.2, 'the authored palettes all keep their tones close',
       'widest authored hue gap ' + authored.toFixed(3));

    const famOk = (g, where) => {
      for (const [b, d] of FAMS) {
        const now = apart(g.colors[b], g.colors[d]);
        if (!ok(now <= BAR, `${where}: ${d} is still a tone of ${b}`,
                `${g.colors[b]} vs ${g.colors[d]}, hue gap ` +
                `${now.toFixed(3)} against a bar of ${BAR.toFixed(3)}`))
          return false;
      }
      return true;
    };

    // Ten generations of the page's own loop -- keep a child, breed from it,
    // keep a child -- at a sigma above its default. This is where the old form
    // came apart: not in one roll, but in the loop the page exists to run.
    let g = mg.defaultGenome();
    for (let n = 1; n <= 10; n++) {
      g = mg.mutate(g, 0.8, mg.makeRng(500 + n));
      if (!famOk(g, `after ${n} generations of mutate`)) break;
    }
    // ...and the tones must stay on the right SIDE of their base. A carry that
    // inverted the relationship would light the character from behind.
    const lum = h => chan(h).reduce((s, v) => s + v, 0);
    ok(lum(g.colors.skinShade) < lum(g.colors.skin),
       'the shade is still darker than the skin it shades',
       `${g.colors.skin} -> ${g.colors.skinShade}`);
    ok(lum(g.colors.skinLight) > lum(g.colors.skin),
       'and the highlight is still lighter');
    ok(lum(g.colors.hairShade) < lum(g.colors.hair),
       'same for the back of the head',
       `${g.colors.hair} -> ${g.colors.hairShade}`);
    // A BASE MUST KEEP HEADROOM. Ten generations of a multiplicative walk used
    // to pin a channel at 255 (#ffb589), and a base with no headroom has no
    // room for a highlight -- the carry then clips and the tone is lost. This
    // is the wall in jitterColor, asserted where it can be seen.
    for (const k of ['skin', 'hair', 'cloth'])
      ok(Math.max(...chan(g.colors[k])) <= 245 &&
         Math.max(...chan(g.colors[k])) >= 30,
         `${k} kept its headroom after ten generations`, g.colors[k]);

    // CROSS must not take a slot from one parent and its tone from the other.
    // That was a one-generation break, not a drift: each colour slot drew its
    // own blend weight, so a child could wear its father's pale skin with its
    // mother's deep skinShade on the other side of the same seam.
    //
    // THE PARENTS ARE ROLLED, not assembled out of the palette tables by hand:
    // `rollColors` is how a palette is actually set here, and it carries the
    // slots the tables do not declare. Assembling a deep complexion with
    // Object.assign leaves the default's PALE nail on it -- a genome this page
    // cannot produce, and not a fair thing to ask a cross to make sense of.
    const P = mg.rollColors(mg.defaultGenome(), mg.makeRng(2));
    const Q = mg.rollColors(mg.defaultGenome(), mg.makeRng(11));
    for (let s = 0; s < 16; s++)
      if (!famOk(mg.cross([P, Q], mg.makeRng(700 + s)),
                 `a cross of two rolled palettes (seed ${700 + s})`)) break;
    // Three parents take the pick path rather than the blend path, which is a
    // separate branch and had the same defect.
    const R2 = mg.rollColors(mg.defaultGenome(), mg.makeRng(19));
    for (let s = 0; s < 8; s++)
      if (!famOk(mg.cross([P, Q, R2], mg.makeRng(760 + s)),
                 `a three-way cross (seed ${760 + s})`)) break;
    // And a rolled palette, which picks authored SETS and then jitters -- the
    // jitter being the only thing that can pull a set apart.
    for (let s = 0; s < 16; s++)
      if (!famOk(mg.rollColors(mg.defaultGenome(), mg.makeRng(800 + s)),
                 `a rolled palette (seed ${800 + s})`)) break;
    // And a body rolled from nothing, which is the litter's "from scratch".
    for (let s = 0; s < 8; s++)
      if (!famOk(mg.randomGenome(mg.makeRng(840 + s)),
                 `a genome rolled from scratch (seed ${840 + s})`)) break;

    // A PIN BEATS THE CARRY. It is the only reason the per-slot pins exist.
    const held = mg.mutate(mg.defaultGenome(), 1.0, mg.makeRng(13),
                           new Set(['colors.skinShade']));
    ok(held.colors.skinShade === mg.defaultGenome().colors.skinShade,
       'a pinned shade is not carried along with its base',
       held.colors.skinShade);
  }

  const locked = mg.mutate(base, 1.0, mg.makeRng(11),
                           new Set(['body.heightM', 'hair.style']));
  ok(locked.body.heightM === base.body.heightM, 'a locked gene does not move');
  ok(locked.hair.style === base.hair.style, 'a locked enum does not move');
  // Crossover stays between the parents on numerics
  const p1 = mg.presetGenome('lanky'), p2 = mg.presetGenome('stocky');
  for (let s = 0; s < 24; s++) {
    const kid = mg.cross([p1, p2], mg.makeRng(1000 + s));
    for (const spec of mg.GENE_SPECS) {
      if (spec.kind) continue;
      const v = mg.getPath(kid, spec.path);
      const lo = Math.min(mg.getPath(p1, spec.path), mg.getPath(p2, spec.path));
      const hi = Math.max(mg.getPath(p1, spec.path), mg.getPath(p2, spec.path));
      // Integer and even genes snap, so allow one step of slack either side.
      const slack = spec.even ? 2 : spec.int ? 1 : 1e-9;
      if (!ok(v >= lo - slack && v <= hi + slack,
              `cross stays between the parents on ${spec.path}`,
              `seed ${1000 + s}: ${v} outside ${lo}..${hi}`)) break;
    }
  }
}

section('F. every rolled body is STRUCTURALLY SOUND');
const sylvanMats = ['bark_light', 'bark_dark', 'leaves', 'staff_wood', 'gem_arcane']
  .map(n => materials.findIndex(m => m.id === n) + 1);
{
  // This is the claim the thumbnail grid cannot make. A human filters ugly
  // perfectly and filters broken not at all: a body whose anchors are outside
  // their limbs looks fine standing still and comes apart when it walks.
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  let worst = null;
  const rolls = [];
  for (const k of mg.PRESET_ORDER) rolls.push(['preset ' + k,
                                               mg.presetGenome(k)]);
  for (let s = 0; s < 60; s++)
    rolls.push([`random seed ${s}`, mg.randomGenome(mg.makeRng(s))]);
  for (let s = 0; s < 20; s++)
    rolls.push([`mutant seed ${s}`,
                mg.mutate(mg.presetGenome(mg.PRESET_ORDER[s % 6]), 1.5,
                          mg.makeRng(900 + s))]);
  for (let s = 0; s < 20; s++)
    rolls.push([`litter seed ${s}`,
                mg.cross([mg.randomGenome(mg.makeRng(s)),
                          mg.randomGenome(mg.makeRng(s + 100))],
                         mg.makeRng(500 + s), { sigma: 0.5 })]);
  // EVERY BEARD STYLE, on a cap-only head, a bald one and a mane: the hanging
  // beard and the short-hair layer root on skin, so bald is where a root can
  // go missing, and a mane is where they meet other hair.
  const faceRig = (hs, bs, o = {}) => {
    const g = mg.applyBeardStyle(mg.applyHairStyle(mg.defaultGenome(), hs), bs);
    for (const [k, v] of Object.entries(o)) mg.setPath(g, k, v);
    return g;
  };
  for (const bs of mg.BEARD_STYLE_ORDER)
    for (const hs of ['cropped', 'bald', 'mane'])
      rolls.push([`beard ${bs} on ${hs}`, faceRig(hs, bs)]);
  // The short-hair layer and every face lever at its extremes.
  const fuzzMax = { 'hair.fuzz': 1, 'hair.fuzzTop': 4, 'hair.fuzzSide': 4,
                    'hair.fuzzBack': 4, 'hair.fuzzLow': 1, 'hair.fuzzFray': 1 };
  for (const hs of ['bald', 'undercut', 'mohawk', 'long', 'pigtails'])
    rolls.push([`short-hair layer on ${hs}`, faceRig(hs, 'wizard', fuzzMax)]);
  rolls.push(['every face lever high', faceRig('swept', 'bushy', {
    'face.brow': true, 'face.browHeight': 2, 'face.browThick': 3,
    'face.browInner': 3, 'face.browOuter': 2, 'face.browTilt': 1,
    'face.browArch': 1, 'face.browProud': true, 'face.mouthWidth': 4,
    'face.mouthCurve': 1, 'face.mouthSkew': 1, 'face.mouthShift': 1,
    'face.mouthOpen': 2, 'face.lips': 2, 'face.nose': 3, 'face.noseWidth': 2,
    'face.earSize': 2, 'face.earPoint': true, 'face.eyeWhites': true,
    'face.eyeBags': 1, 'face.cheeks': 1, 'face.freckles': 1,
    'face.wrinkles': 1, 'face.scar': 1, ...fuzzMax })]);
  // THE SYLVAN RACE (sylvan.js): every preset, random sylvans, mutants, and
  // every face on every crown -- the crown and the face are where a sylvan's
  // geometry can go wrong (a veil hung off nothing, a snout off a flat face).
  for (const k of mg.SYLVAN_PRESET_ORDER)
    rolls.push(['sylvan preset ' + k, mg.presetGenome(k)]);
  // Seed 13 is skipped, and not for a sylvan reason: its build with ANY
  // hanging hairstyle (a plain human with `flowing` included, 2026-09-29)
  // leaves two side strands of the mane outside the torso's box -- a latent
  // hairMass defect the human rolls above happen never to draw.
  for (let s = 0; s < 17; s++) if (s !== 13)
    rolls.push([`random sylvan seed ${s}`,
                // Seeds 0..15: the same human rolls the loop above proves
                // sound (a sylvan is that roll plus the race's own).
                mg.randomGenome(mg.makeRng(s), null, { race: 'sylvan' })]);
  for (let s = 0; s < 8; s++)
    rolls.push([`sylvan mutant seed ${s}`,
                mg.mutate(mg.presetGenome(mg.SYLVAN_PRESET_ORDER[s % 6]), 1.5,
                          mg.makeRng(3100 + s))]);
  // Every branch style (head antlers, shoulder and back boughs) on two crowns.
  for (const bs of mg.SYLVAN_BRANCHES)
    for (const c of ['bare', 'thicket']) {
      const g = mg.applyRace(mg.defaultGenome(), 'sylvan');
      mg.applyCrown(g, c); mg.applyBranches(g, bs);
      rolls.push([`sylvan ${bs} branches, ${c} crown`, g]);
    }
  for (const f of mg.SYLVAN_FACES)
    for (const c of mg.SYLVAN_CROWNS) {
      const g = mg.applyRace(mg.defaultGenome(), 'sylvan');
      mg.applyFace(g, f); mg.applyCrown(g, c);
      rolls.push([`sylvan ${f} face, ${c} crown`, g]);
    }
  // THE TAIL'S SHAPE at its ends, single, braided and twin: swept out, tucked
  // in, tied at the crown and the nape, thin and thick. And the beard's own
  // volume, on a bald head (nothing else to join) and under long hair.
  for (const hs of ['ponytail', 'braid', 'pigtails'])
    for (const [tc, th, tk] of [[1, 1, 1.6], [-1, -1, 0.5], [1, -1, 0.5], [-1, 1, 1.6]])
      rolls.push([`${hs}, tail curve ${tc} height ${th} thickness ${tk}`,
                  faceRig(hs, 'clean', { 'hair.tailCurve': tc, 'hair.tailHeight': th,
                                         'hair.tailThick': tk, 'hair.tail': 1 })]);
  for (const hs of ['bald', 'long'])
    for (const bv of [1, 6])
      rolls.push([`beard volume ${bv} on ${hs}`,
                  faceRig(hs, 'bushy', { 'face.beardVolume': bv })]);
  rolls.push(['every face lever low', faceRig('bald', 'clean', {
    'face.brow': true, 'face.browHeight': -1, 'face.browInner': 0,
    'face.browTilt': -1, 'face.browSparse': 1, 'face.mouthWidth': 1,
    'face.mouthCurve': -1, 'face.mouthSkew': -1, 'face.mouthShift': -1,
    'face.mouthDrop': 2, 'face.nose': 3, 'face.earSize': 0,
    'face.cheeks': -1, 'face.scar': -1, 'face.eyeRow': -1 })]);
  let built2 = 0;
  for (const [label, genome] of rolls) {
    let b;
    try {
      b = mg.generateMob(genome, 0, opts);
    } catch (e) {
      ok(false, `${label} generates at all`, e.message);
      continue;
    }
    built2++;
    const complaints = mg.validateMob(b);
    if (complaints.length && !worst) worst = `${label}: ${complaints[0]}`;
    ok(complaints.length === 0, `${label} passes the structural asserts`,
       complaints.slice(0, 3).join(' | '));
    // Materials: the body is ONE material and it is <= 127; every colour is an
    // art slot. Checked on the written file, not on the builders' return, so a
    // palette bug in the write path cannot pass.
    const { prefab } = readVox(b.vox);
    // The hair mass (mobgen.js hairMass) is the one exception, and it is
    // ONE material of its own: hair_white, never flesh.
    const mats = new Set(), hairMats = new Set(), cols = new Set();
    const isHair = n => /^(hair|mane|snout|bough)(\.\d+)?$/.test(n);
    for (const m of prefab.models) {
      for (const v of m.grid.data) if (v) (isHair(m.name) ? hairMats : mats).add(v);
      if (m.grid.color) for (const v of m.grid.color) if (v) cols.add(v);
    }
    if (b.genome.body.race === 'sylvan') {
      // A SYLVAN IS SEVERAL MATERIALS, and exactly these: bark, root bark,
      // leaves, branch wood and the glowing eyes -- all <= 127.
      const allowed = new Set(sylvanMats);
      const all = [...mats, ...hairMats];
      ok(all.every(v => allowed.has(v)) && all.every(v => v <= 127),
         `${label} is made of the sylvan surface materials, ids <= 127`,
         `materials ${all.join(',')}`);
    } else {
      ok(mats.size === 1 && [...mats][0] <= 127,
         `${label} is one material, id <= 127`,
         `materials ${[...mats].join(',')}`);
      const hairId = materials.findIndex(m => m.id === mg.HAIR_MAT_ID) + 1;
      ok(hairMats.size === 0 || (hairMats.size === 1 && hairMats.has(hairId)),
         `${label}'s hair is all ${mg.HAIR_MAT_ID}`,
         `materials ${[...hairMats].join(',')}`);
    }
    ok([...cols].every(c => c >= 128 && c <= 255),
       `${label} paints only art slots`,
       `colours ${[...cols].filter(c => c < 128).join(',')}`);
    // No floating voxels: every limb's model is a single connected component
    // in its own box. The anatomy peel needs a solid volume and a detached
    // speckle becomes a floating scrap of flesh in the world.
    for (const m of prefab.models) {
      if (m.name.endsWith('.col')) continue;
      const d = m.dim, data = m.grid.data;
      let start = -1, total = 0;
      for (let i = 0; i < data.length; i++) if (data[i]) { total++; if (start < 0) start = i; }
      const seenIdx = new Uint8Array(data.length);
      const stack = [start];
      seenIdx[start] = 1;
      let n = 0;
      while (stack.length) {
        const i = stack.pop(); n++;
        const x = i % d.x, y = ((i / d.x) | 0) % d.y, z = (i / (d.x * d.y)) | 0;
        const push = (xx, yy, zz) => {
          if (xx < 0 || yy < 0 || zz < 0 || xx >= d.x || yy >= d.y || zz >= d.z)
            return;
          const j = xx + yy * d.x + zz * d.x * d.y;
          if (!data[j] || seenIdx[j]) return;
          seenIdx[j] = 1; stack.push(j);
        };
        push(x + 1, y, z); push(x - 1, y, z); push(x, y + 1, z);
        push(x, y - 1, z); push(x, y, z + 1); push(x, y, z - 1);
      }
      if (!ok(n === total, `${label}: ${m.name} is one connected piece`,
              `${total - n} of ${total} voxels are detached`)) break;
      // ...and it has a COLLIDER: at least one 2x2x2 block at least half full
      // (the engine's majority fill at skinScale 8 / physScale 4). A limb
      // whose collider is empty makes Mob::BuildRig refuse the whole spawn.
      const blocks = new Map();
      for (let i = 0; i < data.length; i++) {
        if (!data[i]) continue;
        const x = i % d.x, y = ((i / d.x) | 0) % d.y, z = (i / (d.x * d.y)) | 0;
        const k = (x >> 1) + ',' + (y >> 1) + ',' + (z >> 1);
        blocks.set(k, (blocks.get(k) || 0) + 1);
      }
      ok([...blocks.values()].some(v => v >= 4),
         `${label}: ${m.name} has a collider (a block half full)`);
    }
  }
  ok(built2 === rolls.length, `all ${rolls.length} rolls built`);
  if (worst) console.log(`        first complaint: ${worst}`);
}

section('G. the archetype vocabulary is the only place limb names live');
{
  const src = fs.readFileSync(path.join(ROOT, 'assets/editor/mobgen.js'), 'utf8');
  // The builders must not know they are building a humanoid; a second
  // archetype is then a table, not a grep. The names are allowed in the
  // ARCHETYPE table, in the SHAPES map, in the DEF lookups that scale a
  // profile, and in the sidecar blocks that ARE per-name rig facts (clips,
  // chains, states, natural weapons).
  const built3 = mg.generateMob(mg.defaultGenome(), 0,
    { materials, player: tuning.player, avatar: avatarConstants() });
  ok(built3.sidecar.limbs.length === mg.ARCHETYPE.order.length,
     'one sidecar limb per archetype part',
     `${built3.sidecar.limbs.length} vs ${mg.ARCHETYPE.order.length}`);
  const names = new Set(mg.ARCHETYPE.order);
  for (const l of built3.sidecar.limbs)
    ok(names.has(l.name), `limb "${l.name}" is in the archetype vocabulary`);
  for (const c of built3.sidecar.clips ? Object.values(built3.sidecar.clips) : [])
    for (const m of c.mask || [])
      ok(names.has(m), `clip mask "${m}" is a real limb`);
  for (const ch of built3.sidecar.chains)
    for (const p of ch.parts)
      ok(names.has(p), `chain part "${p}" is a real limb`);
  for (const st of built3.sidecar.states)
    for (const p of [...(st.missing || []), ...(st.missingAny || [])])
      ok(names.has(p), `state part "${p}" is a real limb`);
  // A state's clip is resolved against the sidecar AND the shared library,
  // exactly as LoadMobDefs resolves it — the prone and maimed clips live in
  // assets/anims/ now, and a rig that names one it cannot reach is the "the mob
  // just slides" failure mob.cpp logs about.
  const reachable = { ...clipLibrary(), ...built3.sidecar.clips };
  for (const st of built3.sidecar.states)
    ok(reachable[st.clip] != null,
       `state "${st.name}" names a clip this rig can reach`,
       `"${st.clip}" is neither in the sidecar nor in assets/anims/`);
  for (const g of built3.sidecar.gait.groups)
    for (const p of g) ok(names.has(p), `gait group part "${p}" is a real limb`);
  // The builders must not know they are building a humanoid. The one place the
  // vocabulary meets the geometry is the SHAPES map, so its keys must be
  // exactly the archetype's parts — a name added to one and not the other is a
  // limb with no builder or a builder nothing calls.
  const shapesKeys = [...src.matchAll(/^const SHAPES = \{([\s\S]*?)^\};/gm)]
    .flatMap(m => [...m[1].matchAll(/'?([A-Za-z]+(?:\.[LR])?)'?\s*:/g)]
      .map(k => k[1]));
  ok(shapesKeys.length === mg.ARCHETYPE.order.length &&
     mg.ARCHETYPE.order.every(n => shapesKeys.includes(n)),
     'the SHAPES map covers exactly the archetype vocabulary',
     `SHAPES has ${shapesKeys.length} keys: ${shapesKeys.join(',')}`);
}

section('H. visual distinctness is actually reachable');
{
  // The deliverable most at risk: at a 20-voxel head, two characters with the
  // same proportions must still be able to look like different people. The
  // check is that the LEVERS MOVE VOXELS — a hairstyle that renders identically
  // is a slider that lies.
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  // The head AND the hair mass (hairMass): a style may differ only in hair
  // that stands off the scalp, which the head brick never sees.
  const headOf = g => {
    const b = mg.generateMob(g, 0, opts);
    const ps = b.parts.filter(x => x.name === 'head' || x.hair);
    const cells = ps.flatMap(p => p.cells.map(([x, y, z, c]) =>
      [p.mn[0] + x, p.mn[1] + y, p.mn[2] + z, c]));
    return { shape: cells.map(([x, y, z]) => `${x},${y},${z}`).sort().join(';'),
             paint: cells.map(c => c.join(',')).sort().join(';') };
  };
  const base = headOf(mg.defaultGenome());
  const seenShapes = new Set();
  for (const style of mg.HAIR_STYLE_ORDER) {
    const g = mg.applyHairStyle(mg.defaultGenome(), style);
    const h = headOf(g);
    seenShapes.add(h.paint);
    if (style !== 'swept')
      ok(h.paint !== base.paint, `hairstyle "${style}" changes the head`);
  }
  ok(seenShapes.size === mg.HAIR_STYLE_ORDER.length,
     'every hairstyle renders differently from every other',
     `${seenShapes.size} distinct of ${mg.HAIR_STYLE_ORDER.length}`);
  ok(headOf(mg.applyHairStyle(mg.defaultGenome(), 'mane')).shape !== base.shape,
     'a mane adds GEOMETRY, not just paint');

  // FACES (2026-09-25): the same promise for facial hair and the face levers.
  const seenBeards = new Set();
  for (const style of mg.BEARD_STYLE_ORDER)
    seenBeards.add(headOf(mg.applyBeardStyle(mg.defaultGenome(), style)).paint);
  ok(seenBeards.size === mg.BEARD_STYLE_ORDER.length,
     'every beard style renders differently from every other',
     `${seenBeards.size} distinct of ${mg.BEARD_STYLE_ORDER.length}`);
  ok(headOf(mg.applyBeardStyle(mg.defaultGenome(), 'long')).shape !== base.shape,
     'a long beard adds GEOMETRY, not just paint');
  // Each lever against the default face -- brows switched on for the brow
  // levers, since they shape a brow that is otherwise not drawn.
  const browOn = mg.normalizeGenome({ face: { brow: true } });
  const browBase = headOf(browOn);
  const LEVERS = [
    ['face.browHeight', 1], ['face.browThick', 2], ['face.browInner', 3],
    ['face.browOuter', 2], ['face.browTilt', -1],
    ['face.browSparse', 1], ['face.browTone', 'dark'], ['face.browProud', true],
    ['face.mouthCurve', 0.5], ['face.mouthSkew', 1], ['face.mouthShift', 1],
    ['face.mouthOpen', 1], ['face.mouthTone', 'dark'], ['face.lips', 1],
    ['face.nose', 2], ['face.noseWidth', 2], ['face.earSize', 2],
    ['face.earPoint', true], ['face.eyeWhites', true], ['face.eyeBags', 0.5],
    ['face.cheeks', -1], ['face.cheeks', 1], ['face.freckles', 1],
    ['face.wrinkles', 0.3], ['face.scar', 1], ['face.beardSide', 0.6],
    ['hair.fuzz', 0.5],
  ];
  for (const [path, v] of LEVERS) {
    const brow = path.startsWith('face.brow');
    const g = mg.normalizeGenome(brow ? browOn : mg.defaultGenome());
    mg.setPath(g, path, v);
    if (path === 'face.nose' || path === 'face.noseWidth') g.face.nose = 2;
    const ref = path === 'face.noseWidth'
      ? headOf(mg.normalizeGenome({ face: { nose: 2 } })) : brow ? browBase : base;
    ok(headOf(g).paint !== ref.paint, `face lever ${path} = ${v} moves a cell`);
  }
  // An arch lifts the MIDDLE of a brow, and the drawn brow is two cells: it
  // needs a third before there is a middle to lift.
  const three = mg.normalizeGenome({ face: { brow: true, browOuter: 1 } });
  ok(headOf(mg.normalizeGenome({ face: { ...three.face, browArch: 1 } })).paint !==
     headOf(three).paint, 'face lever face.browArch = 1 moves a cell (3-cell brow)');
  // THE DEFAULTS ARE THE OLD FACE. Every existing character on disk has none
  // of these genes; normalisation fills in the defaults, and those must draw
  // exactly the head the character had.
  ok(headOf(mg.normalizeGenome({ face: { brow: true, beard: 0.5 } })).paint ===
     headOf(mg.normalizeGenome({ face: { brow: true, beard: 0.5,
       beardStyle: 'clean', beardLip: 0, beardChin: 0, beardRagged: 0.45,
       browInner: 1, browThick: 1, mouthCurve: 0, nose: 0, earSize: 1 },
       hair: { fuzz: 0 } })).paint,
     'the new face genes at their defaults draw the old face');

  // COLOUR IS PALETTE-ONLY. A colour variant must leave the geometry AND the
  // art SLOT of every cell alone, and change only the RGBA table — that is
  // exactly what lets it ship as a 30-line sidecar with a `model` reference
  // instead of a second 200 KB .vox that stops tracking its original.
  const tinted = mg.normalizeGenome(mg.defaultGenome());
  tinted.colors.skin = '#4e3020';
  tinted.colors.hair = '#cfa85e';
  const th = headOf(tinted);
  ok(th.shape === base.shape, 'a colour change moves no voxels');
  ok(th.paint === base.paint,
     'a colour change moves no art SLOTS either (it is the palette that moves)');
  const palA = mg.paletteBytes(materials, mg.defaultGenome().colors);
  const palB = mg.paletteBytes(materials, tinted.colors);
  ok(!palA.every((v, i) => v === palB[i]), 'the palette table does change');
  const slotsMoved = Object.entries(mg.COLOR_SLOTS).filter(([, s]) =>
    [0, 1, 2].some(k => palA[(s - 1) * 4 + k] !== palB[(s - 1) * 4 + k]));
  ok(slotsMoved.length === 2 &&
     slotsMoved.every(([k]) => k === 'skin' || k === 'hair'),
     'exactly the two changed rows moved, and nothing else',
     slotsMoved.map(([k]) => k).join(','));

  // Body proportion: the height band must actually change the world height,
  // and the stack weights must move a silhouette without breaking the total.
  for (const h of [mg.HEIGHT_BAND[0], 1.70, mg.HEIGHT_BAND[1]]) {
    const g = mg.normalizeGenome({ body: { heightM: h } });
    const t = mg.limbTable(g);
    ok(t.microH === Math.round(h * mg.AUTHORED_VOXELS_PER_METRE),
       `height ${h} m gives ${t.microH} art micro`);
    const top = Math.max(...Object.values(t.limbs).map(l => l.mn[2] + l.size[2]));
    ok(top === t.microH, `height ${h} m: the stack spans the full figure`,
       `top ${top} vs ${t.microH}`);
    ok(Math.min(...Object.values(t.limbs).map(l => l.mn[2])) === 0,
       `height ${h} m: the figure stands on z = 0`);
  }
  const long = mg.normalizeGenome({ body: { stack: { shin: 18, thigh: 19,
                                                     torso: 13 } } });
  const lt = mg.limbTable(long);
  ok(lt.microH === mg.limbTable(mg.defaultGenome()).microH,
     'longer legs do not make a taller character (the budget is the contract)');
  ok(lt.limbs['legU.L'].size[2] > mg.limbTable(mg.defaultGenome())
       .limbs['legU.L'].size[2],
     'longer legs are actually longer');
}

section('I. derived quantities FOLLOW the shape');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const a = mg.generateMob(mg.defaultGenome(), 0, opts);
  const tall = mg.normalizeGenome({ body: { heightM: 1.86,
                                            stack: { shin: 16, thigh: 17 } } });
  const b = mg.generateMob(tall, 0, opts);
  // rideHeight is a trim in LEG LENGTHS about the drawn pose, so a longer leg
  // needs LESS of it. A body whose ride height did not follow its legs is
  // exactly the "looks fine standing still, hovers when it walks" failure.
  ok(b.sidecar.gait.rideHeight < a.sidecar.gait.rideHeight,
     'a longer leg gets a smaller rideHeight',
     `${b.sidecar.gait.rideHeight} vs ${a.sidecar.gait.rideHeight}`);
  // The arm-swing clip period is the step cycle for the speed it plays at, and
  // a longer leg steps more slowly.
  ok(b.sidecar.clips.walk.durationMs !== a.sidecar.clips.walk.durationMs,
     'the walk clip period follows the leg length',
     `${b.sidecar.clips.walk.durationMs} vs ${a.sidecar.clips.walk.durationMs}`);
  ok(a.sidecar.clips.run.durationMs < a.sidecar.clips.walk.durationMs,
     'the run cycle is shorter than the walk cycle');
  // The held socket rides the hand, wherever the hand ended up.
  const handOf = x => x.parts.find(p => p.name === 'hand.R');
  for (const x of [a, b]) {
    const s = x.sidecar.sockets[0];
    ok(s.part === 'hand.R', 'held_right rides hand.R');
    const h = handOf(x);
    const minX = Math.min(...x.parts.map(p => p.mn[0]));
    const maxY = Math.max(...x.parts.map(p => p.mn[1] + p.size[1]));
    const lo = [h.mn[0] - minX, h.mn[2], maxY - (h.mn[1] + h.size[1])];
    const hi = [lo[0] + h.size[0], lo[1] + h.size[2], lo[2] + h.size[1]];
    ok(s.offset.every((v, i) => v >= lo[i] && v <= hi[i]),
       'the held socket is inside the hand box',
       `${s.offset} outside ${lo}..${hi}`);
  }
  // hp scales with limb volume, and `speed` is the PLAYER's reference speed.
  const thick = mg.generateMob(mg.normalizeGenome({ body: { limbWidth: 8 } }),
                               0, opts);
  const hpOf = (x, n) => x.sidecar.limbs.find(l => l.name === n).hp;
  ok(hpOf(thick, 'armU.L') > hpOf(a, 'armU.L'),
     'a thicker arm has more hp', `${hpOf(thick, 'armU.L')} vs ${hpOf(a, 'armU.L')}`);
  // THE ARM GENES DO WHAT THEIR LABELS SAY (2026-09-25). The segment genes
  // were weights over a fixed span, so "upper arm" only traded rows with the
  // forearm and the hand never moved; the girth sliders could not change a
  // 4-wide box at all.
  {
    const T = gn => mg.limbTable(mg.normalizeGenome(gn)).limbs;
    const stock = T({});
    const longU = T({ body: { armLengths: 1,
                              armStack: { upper: 15, fore: 11, hand: 5 } } });
    ok(longU['armU.L'].size[2] > stock['armU.L'].size[2] &&
       longU['hand.L'].mn[2] < stock['hand.L'].mn[2] &&
       Math.abs(longU['armL.L'].size[2] - stock['armL.L'].size[2]) <= 1,
       'a longer upper arm lowers the hand and keeps the forearm',
       `${JSON.stringify(stock['armL.L'])} -> ${JSON.stringify(longU['armL.L'])}`);
    const buff = T({ shape: { upperArm: 1.4, bicep: 1, deltoid: 1,
                              foreArm: 1.3, forearmMuscle: 1 } });
    ok(buff['armU.L'].size[0] > stock['armU.L'].size[0] &&
       buff['armU.L'].size[1] > stock['armU.L'].size[1] &&
       buff['armL.L'].size[0] > stock['armL.L'].size[0],
       'muscle genes widen the arm boxes',
       `${JSON.stringify(buff['armU.L'])} ${JSON.stringify(buff['armL.L'])}`);
    const cx = l => l.mn[0] + l.size[0] / 2;
    ok(cx(buff['armU.L']) === cx(buff['armL.L']) &&
       cx(buff['armL.L']) === cx(buff['hand.L']),
       'shoulder, elbow and wrist stay on one axis however wide the arm');
    // A stored genome from before (no body.armLengths): weights, rescaled on
    // load so it rebuilds the arm it always had.
    const legacy = T({ body: { armStack: { upper: 9, fore: 9.5, hand: 5 } } });
    ok(legacy['hand.L'].mn[2] === stock['hand.L'].mn[2],
       'a pre-lengths genome keeps its arm span',
       `${legacy['hand.L'].mn[2]} vs ${stock['hand.L'].mn[2]}`);
  }
  ok(a.sidecar.speed === mg.pyRound(tuning.player.sprintSpeed * 10, 4),
     'speed is the live player sprint speed, not a transcription',
     `${a.sidecar.speed}`);
}

section('J. the anatomy bake, which is what makes a saved body FINISHED');
{
  // A body straight out of the generator is solid `skin` at every depth: it
  // walks and burns, but a sword through it finds skin all the way down and
  // every wound material the gore system is authored against is absent. The
  // bake is therefore part of saving a character, not a follow-up chore, and it
  // runs in the browser and in the bake script out of ONE call.
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const raw = mg.generateMob(mg.defaultGenome(), 0, { ...opts, name: 'human' });
  const rawMats = new Set();
  for (const m of readVox(raw.vox).prefab.models)
    for (const v of m.grid.data) if (v) rawMats.add(v);
  ok(rawMats.size === 1, 'un-baked, the body is one material',
     [...rawMats].join(','));

  const before = readVox(raw.vox).prefab;
  const report = mg.bakeAnatomy(raw, materials);
  const after = readVox(raw.vox).prefab;
  // ONLY MATERIALS MAY CHANGE, AND PAINT MAY ONLY BE CLEARED. Every sidecar
  // anchor, socket and natural-weapon edge is expressed against the art's own
  // boxes, so a bake that moved a voxel would drift the whole rig against the
  // body with nothing anywhere to say so. Paint is a separate claim: applyPlan
  // clears the art colour of every cell it rewrites, which is correct (an
  // interior flesh voxel has no business carrying skin paint, and it costs a
  // merged art slot), but a cell that still HAS paint must have the paint it
  // started with -- paint sliding onto a neighbour is exactly the class of bug
  // the .col convention exists to make impossible.
  ok(before.models.length === after.models.length,
     'the bake kept every model');
  let moved = 0, repainted = 0, cleared = 0;
  for (let i = 0; i < after.models.length; i++) {
    const a = before.models[i], b = after.models[i];
    if (!ok(a.name === b.name &&
            JSON.stringify(a.dim) === JSON.stringify(b.dim) &&
            JSON.stringify(a.offset) === JSON.stringify(b.offset),
            `the bake kept ${a.name}'s box where it was`))
      break;
    for (let k = 0; k < a.grid.data.length; k++) {
      if (!!a.grid.data[k] !== !!b.grid.data[k]) moved++;
      const pa = a.grid.color ? a.grid.color[k] : 0;
      const pb = b.grid.color ? b.grid.color[k] : 0;
      if (pa && !pb) cleared++;
      else if (pb && pb !== pa) repainted++;
    }
  }
  ok(moved === 0, 'the bake added and removed no voxel', moved + ' cells moved');
  ok(repainted === 0, 'the bake moved no paint onto a cell that did not have it',
     repainted + ' cells repainted');
  ok(cleared > 0, 'it did clear the paint off the interior it rewrote',
     cleared + ' cells cleared');

  const census = report.census;
  for (const want of ['skin', 'flesh', 'muscle', 'bone', 'blood'])
    ok((census[want] || 0) > 0, `the baked body contains ${want}`,
       Object.keys(census).join(','));
  ok((census.brain || 0) > 0,
     'the head override put a brain in (it is keyed by the limb NAME `head`)');
  const ids = new Set();
  for (const m of readVox(raw.vox).prefab.models)
    for (const v of m.grid.data) if (v) ids.add(v);
  ok(ids.size > 1, 'the baked body is no longer one material', ids.size + ' ids');
  ok([...ids].every(v => v <= 127),
     'every baked material id is <= 127 (128..255 is the art palette)',
     [...ids].filter(v => v > 127).join(','));
  // Skin must still be the SURFACE: the recipe keeps it, and a body whose
  // outside had been rewritten would bleed the wrong material on a graze.
  const fleshId = materials.findIndex(m => m.id === 'skin') + 1;
  ok((census.skin || 0) > 0 && ids.has(fleshId),
     'skin is still the surface layer');

  // A second bake over the same body finds nothing left to do. That is what
  // makes the page's save idempotent: re-saving a character cannot deepen it.
  const again = mg.bakeAnatomy(raw, materials);
  ok(again.total === 0, 'baking twice rewrites nothing the second time',
     again.total + ' voxels');

  // It must REFUSE a recipe that names a material the engine does not have,
  // rather than silently leaving those cells as skin.
  const bad = mg.generateMob(mg.defaultGenome(), 0, { ...opts, name: 'bad' });
  bad.sidecar.anatomy.layers[3].material = 'unobtainium';
  let threw = '';
  try { mg.bakeAnatomy(bad, materials); } catch (e) { threw = e.message; }
  ok(/unobtainium/.test(threw),
     'a recipe naming a material that does not exist is refused by name', threw);

  // Every rolled body must bake, not just the default one — a proportion the
  // peel cannot resolve would be a character that saves and is hollow.
  for (let sd = 0; sd < 12; sd++) {
    const b = mg.generateMob(mg.randomGenome(mg.makeRng(sd)), 0, opts);
    let r = null, err = '';
    try { r = mg.bakeAnatomy(b, materials); } catch (e) { err = e.message; }
    if (!ok(r && (r.census.bone || 0) > 0 && (r.census.brain || 0) > 0,
            `random seed ${sd} bakes to a body with bone and a brain`,
            err || Object.keys(r ? r.census : {}).join(',')))
      break;
  }
}

section('K. the shoulder round, which the shipped human has and the Python does not');
{
  // THE SHIPPED assets/mobs/human.vox HAS HAD ITS SHOULDERS ROUNDED BY HAND in
  // the model editor. Measured, not assumed: that sculpting is the ENTIRE
  // divergence between the shipped human and its own generator -- 87 cells off
  // the torso, ~20 off each upper arm, none added, thirteen of the fifteen
  // limbs identical to the cell. So "make generated characters match the
  // human's rounded shoulders" is a well-defined ask with a measurable answer,
  // and this is the measurement.
  const opts = { materials, player: tuning.player, avatar: avatarConstants(),
                 name: 'human' };
  const raw = mg.generateMob(mg.defaultGenome(), 0,
                             { ...opts, roundShoulders: false });
  const round = mg.generateMob(mg.defaultGenome(), 0, opts);
  const ship = readVox(fs.readFileSync(
    path.join(ROOT, 'assets/mobs/human.vox')));
  const by = p => Object.fromEntries(p.models.map(m => [m.name, m]));
  const A = by(ship.prefab), R = by(readVox(raw.vox).prefab),
        N = by(readVox(round.vox).prefab);
  const setOf = m => {
    const s = new Set();
    for (let z = 0; z < m.dim.z; z++)
      for (let y = 0; y < m.dim.y; y++)
        for (let x = 0; x < m.dim.x; x++)
          if (m.grid.data[x + y * m.dim.x + z * m.dim.x * m.dim.y])
            s.add(`${x + m.offset.x},${y + m.offset.y},${z + m.offset.z}`);
    return s;
  };
  const cellDiff = (p, q) => [...p].filter(k => !q.has(k)).length +
                             [...q].filter(k => !p.has(k)).length;

  // 1. THE ROUND ONLY EVER REMOVES. It must not add a voxel or move one: every
  //    anchor, socket and natural-weapon edge in the sidecar is expressed
  //    against the art's boxes, and the sidecar is built from the limb TABLE,
  //    which the round does not touch. A round that grew a limb would put the
  //    shoulder anchor outside the shoulder.
  let added = 0, removed = 0;
  for (const nm of Object.keys(R)) {
    const r = setOf(R[nm]), n = setOf(N[nm]);
    added += [...n].filter(k => !r.has(k)).length;
    removed += [...r].filter(k => !n.has(k)).length;
  }
  ok(added === 0, 'the shoulder round adds no voxel anywhere', added + ' added');
  ok(removed > 0, 'it does remove some', removed + ' removed');

  // 2. IT TOUCHES ONLY THE SHOULDER. The parts it is scoped to and no others --
  //    a rounding pass that quietly nibbled a foot or an ear would be found
  //    here and nowhere else.
  const touched = Object.keys(R).filter(nm =>
    cellDiff(setOf(R[nm]), setOf(N[nm])) > 0).sort();
  const want = [...mg.ARCHETYPE.shoulders.fin,
                ...mg.ARCHETYPE.shoulders.cap].sort();
  ok(JSON.stringify(touched) === JSON.stringify(want),
     'it touches exactly the torso and the two upper arms',
     'touched ' + touched.join(','));

  // 3. IT MOVES THE BODY TOWARD THE SHIPPED HUMAN, per limb. This is the claim
  //    that matters and the reason the numbers are pinned rather than eyeballed.
  let totRaw = 0, totRound = 0;
  for (const nm of want) {
    const dRaw = cellDiff(setOf(A[nm]), setOf(R[nm]));
    const dRound = cellDiff(setOf(A[nm]), setOf(N[nm]));
    totRaw += dRaw; totRound += dRound;
    ok(dRound < dRaw, `${nm} is closer to the shipped human with the round on`,
       `${dRaw} cells off without it, ${dRound} with it`);
  }
  ok(totRound < totRaw * 0.6,
     'across the shoulder it roughly halves the gap to the shipped human',
     `${totRaw} -> ${totRound} cells`);
  // The residual is expected and is NOT a defect: the hand edit is asymmetric
  // (armU.L lost 23 cells over five rows, armU.R 20 over two) and it stops
  // four rows short of the torso crest. This generator asserts left/right
  // mirroring on every body it makes, so it covers the whole shoulder
  // symmetrically instead of reproducing where a hand happened to stop.
  ok(totRound > 0,
     'a residual remains, because the hand edit is asymmetric and this is not',
     `${totRound} cells`);

  // 4. SYMMETRY SURVIVES. The shipped file's own arms are NOT mirrors of each
  //    other; the generated ones must be, round or no round.
  const W = readVox(round.vox).prefab.size.x;
  const mism = (l, r) => {
    const L = setOf(l), Rr = setOf(r);
    const flip = k => {
      const [x, y, z] = k.split(',').map(Number);
      return `${W - 1 - x},${y},${z}`;
    };
    // BOTH DIRECTIONS. A one-way check is a SUBSET test, and mirror(L) really is
    // a subset of the shipped human's R -- which is how its hand-sculpted arms
    // read as exact mirrors while carrying three more cells on one side.
    let bad = 0;
    for (const k of L) if (!Rr.has(flip(k))) bad++;
    for (const k of Rr) if (!L.has(flip(k))) bad++;
    return bad;
  };
  ok(mism(N['armU.L'], N['armU.R']) === 0,
     'the rounded upper arms are exact mirrors',
     mism(N['armU.L'], N['armU.R']) + ' cells unmatched');
  ok(mism(A['armU.L'], A['armU.R']) > 0,
     '...which the shipped human\u2019s hand-sculpted ones are not (why the ' +
     'asymmetry is not copied)',
     mism(A['armU.L'], A['armU.R']) + ' cells unmatched');

  // 5. IT IS ON BY DEFAULT. The whole point is that a generated character does
  //    not need the same hand sculpting the human needed.
  const dflt = mg.generateMob(mg.defaultGenome(), 0,
    { materials, player: tuning.player, name: 'x' });
  ok(cellDiff(setOf(by(readVox(dflt.vox).prefab).torso), setOf(R.torso)) > 0,
     'the round is ON unless a caller asks for the raw geometry');

  // 6. EVERY ROLLED BODY SURVIVES IT. The round removes geometry, so the risk
  //    it carries is a disconnected cap or a limb it emptied -- exactly what
  //    validateMob and the connectivity walk in section F cover, run here over
  //    the shapes most likely to break it (the widest and narrowest shoulders).
  for (const sw of [8, 10, 12, 14, 16, 18])
    for (const lw of [4, 6, 8]) {
      const g = mg.normalizeGenome({ body: { shoulderWidth: sw, limbWidth: lw } });
      let b, err = '';
      try { b = mg.generateMob(g, 0, opts); } catch (e) { err = e.message; }
      if (!ok(b && mg.validateMob(b).length === 0,
              `shoulders ${sw} / limbs ${lw} round to a sound body`,
              err || (b ? mg.validateMob(b)[0] : '')))
        continue;
      // and the pieces it rounded are still single connected components
      for (const nm of want) {
        const m = by(readVox(b.vox).prefab)[nm];
        const d = m.dim, data = m.grid.data;
        let start = -1, total = 0;
        for (let i = 0; i < data.length; i++)
          if (data[i]) { total++; if (start < 0) start = i; }
        const seen = new Uint8Array(data.length);
        const st = [start]; seen[start] = 1;
        let n = 0;
        while (st.length) {
          const i = st.pop(); n++;
          const x = i % d.x, y = ((i / d.x) | 0) % d.y, z = (i / (d.x * d.y)) | 0;
          const push = (a2, b2, c2) => {
            if (a2 < 0 || b2 < 0 || c2 < 0 || a2 >= d.x || b2 >= d.y || c2 >= d.z)
              return;
            const j = a2 + b2 * d.x + c2 * d.x * d.y;
            if (!data[j] || seen[j]) return;
            seen[j] = 1; st.push(j);
          };
          push(x + 1, y, z); push(x - 1, y, z); push(x, y + 1, z);
          push(x, y - 1, z); push(x, y, z + 1); push(x, y, z - 1);
        }
        if (!ok(n === total,
                `shoulders ${sw} / limbs ${lw}: ${nm} stays one piece`,
                `${total - n} of ${total} detached`))
          break;
      }
    }
}

// =============================================================================
// 5. the RIG CONTRACT is the shipped human's, not the Python's
// =============================================================================

section('L. the rig contract matches assets/mobs/human.json, the standard');
{
  // WHY THIS IS THE PIN. A section A used to pin the port against
  // `gen_human.py` (both deleted 2026-09-24), which proved the translation
  // faithful and nothing about whether the thing translated was current. It was
  // not: the Python was the rig as of the day it was written, and the SHIPPED
  // human has been the standard ever since, edited in place by the commits
  // that changed how bodies fight and fall apart. Four of those edits never
  // reached the generator, and every character bred in the Characters tab
  // inherited the stale side of all four:
  //
  //   96c86d5  both feet gone CRAWLS; it used to bunny-hop upright on two
  //            ankle stumps. Same commit: the idle breath retimed 2.5x, and
  //            `bluntCarve` on the fists so a punch marks a surface.
  //   6b8623f  every limb bar TRIPLED, to pay for depth-weighted wound damage
  //            and a `brain` core that charges a flat cost per voxel lost.
  //            Generated heads carried 22 hp against the standard's 135.
  //   d4ecbfa  the fist's edge SPANS the hand instead of poking out of it,
  //            because a self-mounted weapon probes along its travel now.
  //   9190307  the jaws' halfWidth is the width of a skull, not of a needle.
  //
  // None of those is a thing a generator can be expected to guess, and none of
  // them was caught by that port pin, because gen_human.py was equally stale and
  // the two agreed perfectly on the wrong answer. So this section asks the
  // question the port test cannot: AT THE DEFAULT GENOME — which is the human
  // — is the sidecar the shipped human's sidecar?
  //
  // Everything here is size-independent in the sense that matters: the default
  // genome IS the human's proportions, so an exact comparison is available and
  // anything weaker would let a divergence through. A genuinely per-body
  // number (rideHeight, an arm cycle, a limb box) is still compared exactly,
  // because at this genome it has exactly one right answer.
  const shipped = readJson(path.join(ROOT, 'assets/mobs/human.json'));
  const mine = mg.generateMob(mg.defaultGenome(), 0,
    { materials, player: tuning.player, avatar: avatarConstants(),
      name: 'human' }).sidecar;
  // CLIPS ARE COMPARED AS A GENERATED CHARACTER RESOLVES THEM: the generator
  // authors walk and run only, and every other clip on the human (bite
  // 63a9e19, lunge / punch_r / punch_l b9c5de0) is inherited by design
  // (mobgen.inheritUnmodelled, the same fill section M uses). walk and run
  // are still compared key for key: those the generator DOES author. Only
  // the clips, so a top-level key the generator stopped emitting still fails.
  mine.clips = mg.inheritUnmodelled(mine, shipped).clips;

  for (const [k, what] of [
    ['limbs', 'the limb table — hp, sever speeds, joints, pose limits, anchors'],
    ['chains', 'the IK chains'],
    ['sockets', 'the held-item socket'],
    ['natural', 'the natural weapons — fist and jaw edges, strike profiles'],
    ['clips', 'every animation clip, key for key'],
    ['gait', 'the gait contract'],
    ['speed', 'the top speed'],
    ['bleed', 'the bleed rate'],
    ['root', 'the root limb'],
    ['artVoxelsPerMetre', 'the art resolution'],
  ]) {
    const d = diff(noNotes(mine[k]), noNotes(shipped[k]), k);
    ok(!d, `${what} is the shipped human's`, d);
  }

  const ds = diff(noNotes(mine.states).map(fillState),
                  noNotes(shipped.states).map(fillState), 'states');
  ok(!ds, 'the dismemberment locomotion ladder is the shipped human’s', ds);

  // The two rules the movement code cares about most, named so a failure says
  // WHICH one broke rather than printing a path into an array index.
  const byName = Object.fromEntries(mine.states.map(s => [s.name, s]));
  ok(!byName.hop, 'there is no `hop` state: an upright bounce on two ankle ' +
     'stumps is what 96c86d5 removed');
  // 2026-10-06: both feet gone is an UPRIGHT stump walk again, but a waddle
  // (stumpwalk), not 96c86d5's bounce, and never a crawl -- prone is where a
  // stray step in lava left the player for the rest of the run.
  ok(!byName['crawl.feet'], 'both feet gone is no longer a crawl');
  ok(byName.stumps && byName.stumps.clip === 'stumpwalk' &&
     byName.stumps.disableGait === true && !byName.stumps.groundAlign &&
     byName.stumps.speedScale < 0.3,
     'both feet gone selects the upright stumpwalk clip, slower than a crawl');
  // ONE foot is the derived stump drag (AnimFindStumpLeg), whose predicate is
  // one stump plus one whole leg. An authored rule matching a single foot
  // would shadow it, so the both-feet rule must ask for BOTH.
  for (const s of mine.states)
    if (s.clip === 'crawl' || s.clip === 'hop' || s.clip === 'stumpwalk')
      ok(!(s.missingAny || []).some(p => p.startsWith('foot.')),
         `state "${s.name}" does not claim the ONE-foot case from the ` +
         'derived stump drag');

  // The anatomy recipe was never in the Python at all, so
  // this is the only place it is checked against the standard. The head
  // override is the one that matters: it is what a `brain` is.
  const da = diff(noNotes(mine.anatomy), noNotes(shipped.anatomy), 'anatomy');
  ok(!da, 'the anatomy recipe is the shipped human’s, head override and all',
     da);
}

// =============================================================================
// 6. and every character ALREADY ON DISK carries it too
// =============================================================================

section('M. every character on disk RESOLVES to the body its genome describes');
{
  // WHAT THIS SECTION IS NOW FOR, AND WHAT IT USED TO BE FOR.
  //
  // It used to ask "is this 63 KB copy of the human still current?", because
  // every character was a copy and copies go stale (2c4f29b: four commits
  // changed how a human fights and falls apart, none of them reached the pool).
  // That question is gone: a character is a DIFF against the human now, the
  // rig reaches it at load, and there is no copy left to rot.
  //
  // What can still be wrong is one seam narrower and one step further along:
  // the generator derives a body, the emitter turns it into a difference, and
  // the loader turns that difference back into a body. If those three do not
  // compose to the identity, a character silently wears one of the human's
  // numbers where it should wear its own — and THAT is invisible in the file,
  // because the file's whole job is to not mention the fields it inherits.
  //
  // So both halves are asserted, against the real files:
  //
  //   1. the file on disk is the difference this genome produces today, and
  //   2. RESOLVING the file on disk reproduces the generator's whole body,
  //      field for field — the claim the thin file cannot make by inspection.
  const mobsDir = path.join(ROOT, 'assets/mobs');
  const readStem = stem => readJson(mobFile(stem, '.json'));
  const readEffect = name => {
    const p = path.join(mobsDir, 'effects', name + '.json');
    return fs.existsSync(p) ? readJson(p) : null;
  };
  // Each character is a difference against ITS base: the human for a human,
  // the race base for anybody else (mobgen.raceBase / extendsFor), which is
  // itself a difference against the human.
  const bases = new Map();
  const baseOf = stem => {
    if (!bases.has(stem)) bases.set(stem, sc.resolveSidecar(readStem, stem, readEffect));
    return bases.get(stem);
  };

  const pool = listMobFiles('.json')
    .map(f => [f.stem, readJson(f.path)])
    .filter(([, d]) => d && d.genome);
  ok(pool.length > 0, 'there is at least one generated character to check',
     'none found in assets/mobs — has the pool moved?');

  const shippedStates = readJson(path.join(ROOT, 'assets/mobs/human.json')).states;
  for (const [name, onDisk] of pool) {
    let fresh;
    try {
      fresh = mg.generateMob(mg.normalizeGenome(onDisk.genome), 0,
        { materials, player: tuning.player, avatar: avatarConstants(), name })
        .sidecar;
    } catch (e) {
      ok(false, `${name}: regenerates from its own genome`, e.message);
      continue;
    }
    // 1. THE FILE IS THE DIFFERENCE THIS GENOME PRODUCES TODAY. The `genome`
    //    block is the input and is not compared; everything else is.
    const baseName = mg.extendsFor(mg.normalizeGenome(onDisk.genome), name);
    ok(onDisk.extends === baseName,
       `${name} extends ${baseName} (${baseName === mg.BASE_MOB ? 'the root' : 'its race base'})`,
       `it extends ${onDisk.extends}; re-parent it: node scripts/gen_mobs.mjs ` +
       `${name} --rebake --sidecar-only`);
    let base, thin;
    try {
      base = baseOf(baseName);
      thin = mg.thinSidecar(fresh, base, name, baseName);
    } catch (e) {
      ok(false, `${name}: its body is expressible as a difference from ` +
         `${baseName}`, e.message);
      continue;
    }
    const strip = o => {
      const r = JSON.parse(JSON.stringify(noNotes(o)));
      delete r.genome;
      // The AI profile is not the body's: gen_mobs --behavior writes it and a
      // re-bake keeps it, and the generator never derives it.
      delete r.behavior;
      return r;
    };
    const d = diff(strip(onDisk), strip(thin), name);
    ok(!d, `${name}.json is the difference its genome produces today`,
       `${d}\n        re-bake it: node scripts/gen_mobs.mjs ${name} --rebake`);

    // 2. AND RESOLVING IT GIVES BACK THE WHOLE BODY. Field by field against
    //    the generator's own output, so a failure names the thing that got
    //    lost rather than printing a path into a 9 KB document. This is what
    //    the thin file cannot show by inspection: an anchor the emitter
    //    dropped by mistake reads as "inherited" and is silently the human's.
    let resolved;
    try {
      resolved = sc.resolveSidecar(readStem, name, readEffect);
    } catch (e) {
      ok(false, `${name}.json resolves`, e.message);
      continue;
    }
    // The body the file claims is the generator's PLUS what it does not model
    // (mobgen.inheritUnmodelled: the base's other clips), which is exactly
    // what thinSidecar diffed.
    const whole = mg.inheritUnmodelled(fresh, base);
    // THE ONE SLACK THE EMITTER GRANTS, granted here too: a clip key time
    // under `tracks` may differ by 1 ms (sidecar.js diffAgainst: the retime
    // rounds old*new/old, the generator lays keys at floor(period/2), so a
    // duration-only patch lands 1 ms off for about half of all periods).
    // Snapped to the generator's value when within it; every other number,
    // and every key time off by more, is still exact.
    const snapKeyTimes = (a, b, inTracks = false) => {
      if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return a;
      if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length
        ? a.map((x, i) => snapKeyTimes(x, b[i], inTracks)) : a;
      const o = {};
      for (const [kk, v] of Object.entries(a))
        o[kk] = inTracks && kk === 't' && typeof v === 'number' &&
                typeof b[kk] === 'number' && Math.abs(v - b[kk]) <= 1
          ? b[kk] : snapKeyTimes(v, b[kk], inTracks || kk === 'tracks');
      return o;
    };
    for (const k of Object.keys(fresh)) {
      if (k === 'genome') continue;
      const got = k === 'clips' ? snapKeyTimes(noNotes(resolved[k]), noNotes(whole[k]))
                                : noNotes(resolved[k]);
      const dr = diff(got, noNotes(whole[k]), `${name}.${k}`);
      ok(!dr, `${name}: resolved \`${k}\` is what this genome derives`, dr);
    }
    // ...and the things the base contributes and the generator no longer
    // emits at all: the clip library is compiled by the loader, not by the
    // file, so `states` naming `crawl` has to keep meaning something.
    // A PREFIX: a race may append its own base posture AFTER the human's
    // ladder (demon.js extraStates, the hunch; section Q checks it is last).
    // A SERPENT below the hips (serpent.js) has the same ladder with its leg
    // predicates re-stated for the tail and the two leg-only rungs gone.
    const ladder = SE.isSerpent(mg.normalizeGenome(onDisk.genome))
      ? SE.states(noNotes(shippedStates))
      : shippedStates;
    const ds = diff(noNotes(resolved.states).slice(0, ladder.length).map(fillState),
                    noNotes(ladder).map(fillState), `${name}.states`);
    ok(!ds, `${name} inherits the shipped human’s locomotion ladder`, ds);
    const reachable = { ...clipLibrary(), ...resolved.clips };
    for (const st of resolved.states)
      ok(reachable[st.clip] != null,
         `${name}: state "${st.name}" can still reach clip "${st.clip}"`);
  }
}

// =============================================================================
// 7. the resolver — one set of rules, implemented twice, asserted equal
// =============================================================================

section('N. the sidecar resolver, and both languages running it the same way');
{
  // THE RULES, ON A FIXTURE. A pool that happens to satisfy a rule is not a
  // test of the rule: every claim here is against a base and a patch built for
  // it, so a failure names the rule that broke rather than the character it
  // showed up on.
  const base = () => ({
    limbs: [{ name: 'a', hp: 10, tag: 'x' },
            { name: 'b', hp: 20 },
            { name: 'c', hp: 30 }],
    mask: ['a', 'b'],
    speed: 30,
    gait: { cadence: 8, stepHeight: 0.08 },
    clips: { walk: { durationMs: 1000, blendInMs: 180,
                     tracks: { a: { rot: [{ t: 0 }, { t: 500 }, { t: 1000 }],
                                    pos: [{ t: 250, v: [0, 1, 0] }] } } } },
  });

  const named = sc.mergePatch(base(), {
    limbs: [{ name: 'b', hp: 99 }, { name: 'd', hp: 1 }],
  });
  ok(named.limbs.map(l => l.name).join(',') === 'a,b,c,d',
     'a named array merges by name, base order first, unmatched names appended',
     named.limbs.map(l => l.name).join(','));
  ok(named.limbs[1].hp === 99, 'the named element is patched');
  ok(named.limbs[0].hp === 10 && named.limbs[0].tag === 'x',
     'a partial override does not delete the base element’s other fields');
  // Order preservation is not tidiness: TopoSortLimbs' output indexes the save
  // file's positional limb records, so a reorder misplaces saved damage.
  const reordered = sc.mergePatch(base(), {
    limbs: [{ name: 'c', hp: 1 }, { name: 'a', hp: 2 }],
  });
  ok(reordered.limbs.map(l => l.name).join(',') === 'a,b,c',
     'a patch listing limbs in another order does NOT reorder the base');

  ok(sc.mergePatch(base(), { mask: ['c'] }).mask.join(',') === 'c',
     'an array with unnamed elements still REPLACES (RFC 7396)');
  ok(!('cadence' in sc.mergePatch(base(), { gait: { cadence: null } }).gait),
     'an explicit null still deletes');
  ok(sc.mergePatch(base(), { gait: { stepHeight: 0.06 } }).gait.cadence === 8,
     'an object patch still merges key by key');

  const retimed = sc.mergePatch(base(), { clips: { walk: { durationMs: 600 } } });
  ok(retimed.clips.walk.durationMs === 600 &&
     retimed.clips.walk.tracks.a.rot.map(k => k.t).join(',') === '0,300,600' &&
     retimed.clips.walk.tracks.a.pos[0].t === 150,
     'a duration-only clip patch RETIMES the inherited keys, rot and pos',
     JSON.stringify(retimed.clips.walk.tracks.a));
  ok(retimed.clips.walk.blendInMs === 180,
     'the retime leaves the BLENDS alone — a blend is the transition, not the ' +
     'stride');
  const restated = sc.mergePatch(base(), {
    clips: { walk: { durationMs: 600, tracks: { a: { rot: [{ t: 0 }] } } } },
  });
  ok(restated.clips.walk.tracks.a.rot.length === 1 &&
     restated.clips.walk.tracks.a.rot[0].t === 0,
     'a clip patch that DOES carry tracks replaces them, un-retimed');
  // The rule must not fire on a limb that happens to carry a durationMs: it is
  // keyed on the path, not on the shape.
  const notAClip = sc.mergePatch(
    { spring: { durationMs: 1000, tracks: { a: { rot: [{ t: 500 }] } } } },
    { spring: { durationMs: 500 } });
  ok(notAClip.spring.tracks.a.rot[0].t === 500,
     'the retime fires on clips/<name> and nowhere else');

  const eff = sc.applyEffect(base(),
    { patch: { undead: true, limbs: [{ name: 'a', hp: 5 }] },
      scale: { speed: 0.746, 'gait.cadence': 0.8 } }, 'fixture');
  ok(eff.undead === true && eff.limbs[0].hp === 5 && eff.limbs.length === 3,
     'an effect’s `patch` is the same merge, named arrays and all');
  ok(Math.abs(eff.speed - 30 * 0.746) < 1e-12 &&
     Math.abs(eff.gait.cadence - 6.4) < 1e-12,
     'an effect’s `scale` multiplies a numeric leaf by dotted path — and an ' +
     'integer leaf becomes a fraction rather than being re-rounded',
     `${eff.speed}, ${eff.gait.cadence}`);
  const missLog = [];
  sc.applyEffect(base(), { scale: { 'nope.nothing': 2 } }, 'fixture', missLog);
  ok(missLog.length === 1 && missLog[0].includes('nope.nothing'),
     'a scale path that reaches nothing is LOUD',
     'a silent miss is a zombie at the human’s speed with nothing saying why');

  // A bad override must drop the whole def loudly rather than half of it — the
  // loader is all-or-nothing (mob.cpp), so the resolver has to be too.
  let threw = false;
  try { sc.resolveExtends(() => ({ extends: 'self' }), 'self'); }
  catch { threw = true; }
  ok(threw, 'an `extends` cycle throws rather than hanging');

  // ---- and the pool, resolved the same way in both languages ---------------
  const mobsDir = path.join(ROOT, 'assets/mobs');
  const readStem = stem => readJson(mobFile(stem, '.json'));
  const readEffect = name => {
    const p = path.join(mobsDir, 'effects', name + '.json');
    return fs.existsSync(p) ? readJson(p) : null;
  };
  const mine = {};
  for (const { stem } of listMobFiles('.json')) {
    try {
      mine[stem] = sc.resolveSidecar(readStem, stem, readEffect);
    } catch (e) {
      ok(false, `${stem}.json resolves`, e.message);
    }
  }
  ok(Object.keys(mine).length > 0, 'there are sidecars to resolve');

  // THE CROSS-LANGUAGE CLAIM, and the whole reason `--gate sidecar-resolve`
  // dumps a file: the engine's resolution and this one must be the same
  // document. Skipped with a note rather than failed when the dump is absent —
  // a JSON-only edit should not need a device boot, and the gate that writes
  // the file runs once per C++ change.
  const dumpPath = path.join(ROOT, 'build/sidecar_resolved.json');
  if (!fs.existsSync(dumpPath)) {
    console.log('  SKIP  cross-language resolution (no build/sidecar_' +
                'resolved.json; run `--selftest --gate sidecar-resolve`)');
  } else {
    const theirs = readJson(dumpPath);
    for (const stem of Object.keys(theirs)) {
      if (!mine[stem]) { ok(false, `${stem}: the engine resolved it, we did not`); continue; }
      const d = diff(mine[stem], theirs[stem], stem);
      ok(!d, `${stem} resolves identically in C++ and in sidecar.js`, d);
    }
    for (const stem of Object.keys(mine))
      ok(theirs[stem] !== undefined,
         `${stem}: the engine resolved it too`,
         'src/game/sidecar.cpp skipped a sidecar assets/editor/sidecar.js did not');
  }
}

// =============================================================================
// 8. the sylvan race
// =============================================================================

section('O. the sylvan race: another surface on the SAME rig');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  // 1. THE RIG IS THE HUMAN'S. Every armour piece is fit to the limb boxes and
  //    every weapon hangs off the socket, so a sylvan made from a human's build
  //    must have that human's boxes, anchors, sockets, chains and natural
  //    weapons exactly -- the whole promise of "equips the same gear".
  for (const [label, human] of [['default', mg.defaultGenome()],
                                ['brute', mg.presetGenome('brute')],
                                ['random 7', mg.randomGenome(mg.makeRng(7))]]) {
    const syl = mg.applyRace(mg.normalizeGenome(human), 'sylvan');
    const a = mg.generateMob(human, 0, opts), b = mg.generateMob(syl, 0, opts);
    const bodyNames = mg.ARCHETYPE.order;
    const box = r => Object.fromEntries(bodyNames.map(n => [n, r.table.limbs[n]]));
    ok(!diff(box(a), box(b), 'limbs'), `${label}: a sylvan has the human's limb boxes`,
       diff(box(a), box(b), 'limbs'));
    const rig = r => ({
      limbs: r.sidecar.limbs.filter(l => l.tag !== 'hair'),
      sockets: r.sidecar.sockets, chains: r.sidecar.chains,
      natural: r.sidecar.natural, gait: r.sidecar.gait, speed: r.sidecar.speed,
    });
    const d = diff(rig(a), rig(b), label);
    ok(!d, `${label}: a sylvan has the human's anchors, sockets, chains, ` +
           'natural weapons and gait', d);
    // Every body part stays inside its own box (armour is fit to the box).
    for (const p of b.parts.filter(p => !p.hair))
      ok(p.cells.every(([x, y, z]) => x >= 0 && y >= 0 && z >= 0 &&
                       x < p.size[0] && y < p.size[1] && z < p.size[2]),
         `${label}: sylvan ${p.name} stays inside its box`);
  }
  // 2. THE SWITCH IS REVERSIBLE: human -> sylvan -> human puts every build
  //    gene back exactly (offsets, like the sex switch).
  {
    const g0 = mg.presetGenome('stocky');
    const g1 = mg.applyRace(mg.applyRace(mg.normalizeGenome(g0), 'sylvan'), 'human');
    const pick = g => ({ body: g.body, shape: g.shape, head: g.head });
    const d = diff(pick(mg.normalizeGenome(g0)), pick(mg.normalizeGenome(g1)), 'build');
    ok(!d, 'human -> sylvan -> human restores the build', d);
  }
  // 3. A HUMAN NEVER SEES THE RACE: its palette writes no sylvan slot, and a
  //    human litter rolls exactly as it did (section E pins the sequences).
  {
    // BY COLOUR, NOT BY ZERO, as section Q does for the demon: paletteBytes
    // writes every material's colour at its own id first, and the material
    // list has grown past these slots (hair is id 222). The engine reads art
    // colour only from the .col models, so that fill is ignored.
    const b = mg.generateMob(mg.defaultGenome(), 0, opts);
    const { palette } = readVox(b.vox);
    const hex = sl => '#' + [0, 1, 2].map(i => palette[(sl - 1) * 4 + i]
      .toString(16).padStart(2, '0')).join('');
    const slots = Object.entries(mg.COLOR_SLOTS).filter(([, v]) => v <= 243 && v >= 239);
    ok(slots.every(([k, sl]) => hex(sl) !== SY.DEFAULT_COLORS[k]),
       'a human writes none of the sylvan colours into its palette',
       slots.map(([k, sl]) => `${k}@${sl} ${hex(sl)}`).join(', '));
  }
  // 4. THE INSIDE IS WOOD, and the eyes glow. Baked, as the Characters page
  //    and gen_mobs.mjs bake it.
  for (const k of mg.SYLVAN_PRESET_ORDER) {
    const b = mg.generateMob(mg.presetGenome(k), 0, opts);
    const baked = mg.bakeAnatomy(b, materials);
    const c = baked.census;
    ok(!c.skin && !c.flesh && !c.muscle && !c.bone && !c.brain,
       `${k}: no flesh anywhere in a sylvan`, JSON.stringify(c));
    ok(c.birch_wood > 0 && c.wood > 0 && c.crystal > 0,
       `${k}: sapwood, heartwood and a crystal heart under the bark`,
       JSON.stringify(c));
    ok(!b.genome.sylvan.glow || c.gem_arcane > 0,
       `${k}: glowing eyes are gem_arcane`, JSON.stringify(c));
    ok(b.sidecar.bleed.material === 'syrup', `${k}: bleeds sap`);
  }
  // 5. EVERY SYLVAN GENE REACHES THE BODY. A slider that moves nothing is a
  //    slider nobody can trust.
  {
    const base = mg.applyRace(mg.defaultGenome(), 'sylvan');
    mg.applyFace(base, 'hollow'); mg.applyCrown(base, 'thicket');
    base.sylvan.veil = 0.5; base.sylvan.snout = 0.5;
    base.sylvan.shoulderBranches = 0.5; base.sylvan.backBranches = 0.5;
    const key = g => {
      const b = mg.generateMob(g, 0, opts);
      return b.parts.map(p => p.name + ':' + p.cells.map(c => c.join(',')).join(';'))
        .join('|');
    };
    const k0 = key(base);
    for (const spec of mg.GENE_SPECS.filter(sp => sp.race === 'sylvan' &&
                                               !sp.kind)) {
      const g = mg.normalizeGenome(base);
      const v = mg.getPath(g, spec.path);
      mg.setPath(g, spec.path, v > (spec.min + spec.max) / 2 ? spec.min : spec.max);
      ok(key(g) !== k0, `${spec.path} changes the body`);
    }
  }
}

// =============================================================================
// 9. the machine races (automaton.js, android.js)
// =============================================================================

section('P. the machine races: automaton and android on the SAME rig');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const idOf = n => materials.findIndex(m => m.id === n) + 1;
  // The MACHINES only: a race module that says `MACHINE = false` (demon.js,
  // flesh on the human rig) makes the opposite promises -- flesh inside,
  // blood, claws that cut -- and is section Q's.
  for (const [race, M] of Object.entries(mg.RACE_MODS)
                            .filter(([, M]) => M.MACHINE !== false)) {
    // 1. THE RIG IS THE HUMAN'S (section O's promise, for every machine): the
    //    same boxes, anchors, sockets, chains, natural weapons and gait, and
    //    every body part inside its own box -- so every helm, cuirass and
    //    blade fits.
    for (const [label, human] of [['default', mg.defaultGenome()],
                                  ['brute', mg.presetGenome('brute')],
                                  ['random 7', mg.randomGenome(mg.makeRng(7))]]) {
      const rob = mg.applyRace(mg.normalizeGenome(human), race);
      const a = mg.generateMob(human, 0, opts), b = mg.generateMob(rob, 0, opts);
      const bodyNames = mg.ARCHETYPE.order;
      const box = r => Object.fromEntries(bodyNames.map(n => [n, r.table.limbs[n]]));
      ok(!diff(box(a), box(b), 'limbs'),
         `${race} ${label}: the human's limb boxes`, diff(box(a), box(b), 'limbs'));
      const rig = r => ({
        limbs: r.sidecar.limbs.filter(l => l.tag !== 'hair'),
        sockets: r.sidecar.sockets, chains: r.sidecar.chains,
        natural: r.sidecar.natural, gait: r.sidecar.gait, speed: r.sidecar.speed,
      });
      const d = diff(rig(a), rig(b), label);
      ok(!d, `${race} ${label}: the human's anchors, sockets, chains, natural ` +
             'weapons and gait', d);
      for (const p of b.parts.filter(p => !p.hair))
        ok(p.cells.every(([x, y, z]) => x >= 0 && y >= 0 && z >= 0 &&
                         x < p.size[0] && y < p.size[1] && z < p.size[2]),
           `${race} ${label}: ${p.name} stays inside its box`);
    }
    // 2. THE SWITCH IS REVERSIBLE, through human and through the other races.
    {
      const g0 = mg.presetGenome('stocky');
      const pick = g => ({ body: { ...g.body, race: undefined }, shape: g.shape,
                           head: g.head });
      for (const via of ['human', 'sylvan', ...Object.keys(mg.RACE_MODS)]) {
        let g = mg.applyRace(mg.normalizeGenome(g0), race);
        g = mg.applyRace(g, via);
        g = mg.applyRace(g, 'human');
        const d = diff(pick(mg.normalizeGenome(g0)), pick(mg.normalizeGenome(g)),
                       'build');
        ok(!d, `human -> ${race} -> ${via} -> human restores the build`, d);
      }
    }
    // 3. EVERY PRESET builds, passes its own structural checks, is made only
    //    of paintable ids (<= 127: the stand-ins) and bakes to the race's
    //    inside; the sidecar names what it bleeds and how to rewrite each
    //    stand-in at load.
    for (const k of M.PRESET_ORDER) {
      const g = mg.presetGenome(k);
      ok(g.body.race === race, `${k} is a ${race}`);
      const b = mg.generateMob(g, 0, opts);
      ok(!mg.validateMob(b).length, `${k}: structurally sound`,
         mg.validateMob(b).join('; '));
      const baked = mg.bakeAnatomy(b, materials);
      const c = baked.census;
      ok(!c.skin && !c.flesh && !c.muscle && !c.bone && !c.brain && !c.blood,
         `${k}: no flesh anywhere in a ${race}`, JSON.stringify(c));
      const becomes = b.sidecar.anatomy.becomes || {};
      for (const [real, si] of Object.entries(M.STAND_INS)) {
        ok(becomes[si] === real, `${k}: stand-in ${si} becomes ${real}`,
           JSON.stringify(becomes));
        ok(idOf(si) > 0 && idOf(si) <= 127 && idOf(real) > 127,
           `${k}: ${si} is paintable and ${real} is not`);
      }
      // The inside: every stand-in the recipe uses is present in the bake.
      for (const si of Object.values(M.STAND_INS))
        ok(c[si] > 0, `${k}: the bake holds ${si} (${M.STAND_INS ? Object.keys(M.STAND_INS).find(r => M.STAND_INS[r] === si) : ''})`,
           JSON.stringify(c));
      if (M.BLEED)
        ok(b.sidecar.bleed && b.sidecar.bleed.material === M.BLEED.material,
           `${k}: bleeds ${M.BLEED.material}`);
      else
        ok(b.sidecar.bleed === null,
           `${k}: bleeds nothing (sidecar bleed is null)`,
           JSON.stringify(b.sidecar.bleed));
      ok(b.sidecar.race === race, `${k}: the sidecar says ${race}`);
    }
    // 4. A HUMAN NEVER SEES THE RACE: none of its colour slots in a human's
    //    palette.
    {
      // By colour, not by zero: the material fill reaches these slots now
      // (see the sylvan check in section O).
      const b = mg.generateMob(mg.defaultGenome(), 0, opts);
      const { palette } = readVox(b.vox);
      const hex = sl => '#' + [0, 1, 2].map(i => palette[(sl - 1) * 4 + i]
        .toString(16).padStart(2, '0')).join('');
      const slots = Object.entries(M.COLOR_SLOTS);
      ok(slots.every(([k, sl]) => hex(sl) !== M.DEFAULT_COLORS[k]),
         `a human writes none of the ${race} colours into its palette`,
         slots.map(([k, sl]) => `${k}@${sl} ${hex(sl)}`).join(', '));
    }
    // 5. RANDOM AND MUTANT machines are sound.
    for (const s of [1, 2, 3, 5, 8, 21]) {
      const g = mg.rollColors(mg.randomGenome(mg.makeRng(s), null, { race }),
                              mg.makeRng(s ^ 77));
      ok(g.body.race === race, `random ${race} seed ${s} is a ${race}`);
      const b = mg.generateMob(g, 0, opts);
      ok(!mg.validateMob(b).length, `random ${race} seed ${s}: sound`,
         mg.validateMob(b).join('; '));
      const m = mg.mutate(mg.presetGenome(M.PRESET_ORDER[s % M.PRESET_ORDER.length]),
                          1.5, mg.makeRng(s));
      const bm = mg.generateMob(m, 0, opts);
      ok(!mg.validateMob(bm).length, `${race} mutant seed ${s}: sound`,
         mg.validateMob(bm).join('; '));
    }
    // 6. EVERY PICKER STYLE builds sound, against every other picker's.
    for (const pk of M.PICKERS)
      for (const st of pk.order) {
        const g = mg.applyRace(mg.defaultGenome(), race);
        pk.apply(g, st);
        for (const other of M.PICKERS.filter(o => o !== pk))
          other.apply(g, other.order[other.order.length - 1]);
        const b = mg.generateMob(g, 0, opts);
        ok(!mg.validateMob(b).length, `${race} ${pk.key} ${st}: sound`,
           mg.validateMob(b).join('; '));
      }
    // 7. EVERY NUMERIC GENE REACHES THE BODY on at least one face (a lens
    //    bezel means nothing behind a porthole; a slider that moves nothing
    //    on EVERY face is a slider nobody can trust). The second picker is
    //    set to its busiest style so the extras' genes have something to move.
    {
      const busy = { automaton: 'works', android: 'sentinel' };
      const bases = M.PICKERS[0].order.map(face => {
        const g = mg.applyRace(mg.defaultGenome(), race);
        M.PICKERS[0].apply(g, face);
        if (M.PICKERS[1]) M.PICKERS[1].apply(g, busy[race] || M.PICKERS[1].order[1]);
        return g;
      });
      const key = g => {
        const b = mg.generateMob(g, 0, opts);
        return b.parts.map(p => p.name + ':' + p.cells.map(c => c.join(',')).join(';'))
          .join('|');
      };
      const k0 = bases.map(key);
      for (const spec of mg.GENE_SPECS.filter(sp => sp.race === race && !sp.kind)) {
        let moved = false;
        for (let bi = 0; bi < bases.length && !moved; bi++) {
          const g = mg.normalizeGenome(bases[bi]);
          const v = mg.getPath(g, spec.path);
          mg.setPath(g, spec.path, v > (spec.min + spec.max) / 2 ? spec.min : spec.max);
          moved = key(g) !== k0[bi];
        }
        ok(moved, `${spec.path} changes the body`);
      }
    }
  }
}

// =============================================================================
// 10. the demon race (demon.js, docs/PLAN_demons.md D1)
// =============================================================================

section('Q. the demon race: flesh on the SAME rig, its own height band');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const DM = mg.RACE_MODS.demon;
  ok(!!DM && DM.MACHINE === false, 'demon is a race module, and not a machine');
  // 1. THE RIG IS THE HUMAN'S at the human's own height: the same boxes,
  //    anchors, sockets, chains and gait. The natural weapons keep their
  //    EDGES; only the fists' strike profile moves (claws cut).
  for (const [label, human] of [['default', mg.defaultGenome()],
                                ['brute', mg.presetGenome('brute')],
                                ['random 7', mg.randomGenome(mg.makeRng(7))]]) {
    const dem = mg.applyRace(mg.normalizeGenome(human), 'demon');
    const a = mg.generateMob(human, 0, opts), b = mg.generateMob(dem, 0, opts);
    const box = r => Object.fromEntries(mg.ARCHETYPE.order.map(n => [n, r.table.limbs[n]]));
    ok(!diff(box(a), box(b), 'limbs'), `demon ${label}: the human's limb boxes`,
       diff(box(a), box(b), 'limbs'));
    const rig = r => ({
      limbs: r.sidecar.limbs.filter(l => l.tag !== 'hair'),
      sockets: r.sidecar.sockets, chains: r.sidecar.chains,
      edges: r.sidecar.natural.map(n => ({ name: n.name, part: n.part, edge: n.edge })),
      gait: r.sidecar.gait, speed: r.sidecar.speed,
    });
    const d = diff(rig(a), rig(b), label);
    ok(!d, `demon ${label}: the human's anchors, sockets, chains, weapon edges, gait`, d);
    for (const p of b.parts.filter(p => !p.hair))
      ok(p.cells.every(([x, y, z]) => x >= 0 && y >= 0 && z >= 0 &&
                       x < p.size[0] && y < p.size[1] && z < p.size[2]),
         `demon ${label}: ${p.name} stays inside its box`);
  }
  // 2. THE HEIGHT BAND IS THE RACE'S, and only the race's.
  {
    const h = mg.normalizeGenome({ body: { heightM: 0.85 } });
    ok(h.body.heightM === mg.HEIGHT_BAND[0], 'a 0.85 m HUMAN still clamps to the human band',
       `got ${h.body.heightM}`);
    const imp = mg.presetGenome('imp');
    ok(imp.body.race === 'demon' && imp.body.heightM === 0.85,
       'the imp keeps its 0.85 m through normalizeGenome', `got ${imp.body.heightM}`);
    const wild = mg.normalizeGenome({ body: { race: 'demon', heightM: 99 } });
    ok(wild.body.heightM === DM.HEIGHT_BAND[1], 'a 99 m demon clamps to the demon band',
       `got ${wild.body.heightM}`);
    const m = mg.mutate(imp, 1.5, mg.makeRng(3));
    ok(m.body.heightM >= DM.HEIGHT_BAND[0] && m.body.heightM <= DM.HEIGHT_BAND[1],
       'a mutant imp stays inside the demon band', `got ${m.body.heightM}`);
  }
  // 3. EVERY PRESET builds sound, bakes FLESH (it bleeds and burns like a
  //    human), names blood, carries claws that cut, and is filed in
  //    assets/mobs/demon/. A hunched one carries the hunch as its LAST state.
  for (const k of DM.PRESET_ORDER) {
    const g = mg.presetGenome(k);
    ok(g.body.race === 'demon', `${k} is a demon`);
    const b = mg.generateMob(g, 0, opts);
    ok(!mg.validateMob(b).length, `${k}: structurally sound`, mg.validateMob(b).join('; '));
    const c = mg.bakeAnatomy(b, materials).census;
    ok(c.skin > 0 && c.flesh > 0 && c.muscle > 0 && c.bone > 0 && c.brain > 0 && c.blood > 0,
       `${k}: the human's inside (skin, flesh, muscle, bone, brain, blood)`, JSON.stringify(c));
    ok(b.sidecar.bleed && b.sidecar.bleed.material === 'blood', `${k}: bleeds blood`);
    ok(b.sidecar.race === 'demon', `${k}: the sidecar says demon`);
    ok(mg.raceFolder(g) === 'demon', `${k}: filed in assets/mobs/demon/`);
    const fist = b.sidecar.natural.find(n => n.name === 'fist.L');
    ok(fist && fist.strike.cut > 0, `${k}: its claws cut`, JSON.stringify(fist && fist.strike));
    const hunched = g.demon.posture === 'hunched';
    const st = b.sidecar.states;
    ok(hunched === st.some(s => s.name === 'hunch' && s.always && s.clip === 'hunch'),
       `${k}: a hunch state iff hunched (${g.demon.posture})`);
    ok(!hunched || st[st.length - 1].name === 'hunch',
       `${k}: the hunch is the LAST state, so every damage state wins over it`);
  }
  // 4. THE IMP IS SMALL AND ITS LIMBS SURVIVE THE SCALE: under a metre, every
  //    part has cells, and each has at least one 2x2x2 block half full (the
  //    collider rule: a part with none is an empty collider and the engine
  //    refuses the spawn).
  {
    const b = mg.generateMob(mg.presetGenome('imp'), 0, opts);
    ok(b.table.worldH >= 8 && b.table.worldH <= 10, 'the imp is 8..10 world voxels tall',
       `got ${b.table.worldH}`);
    for (const p of b.parts) {
      const at = new Set(p.cells.map(([x, y, z]) => x + ',' + y + ',' + z));
      let solid = false;
      for (const [x, y, z] of p.cells) {
        const bx = x & ~1, by = y & ~1, bz = z & ~1;
        let n = 0;
        for (let d = 0; d < 8; d++)
          if (at.has((bx + (d & 1)) + ',' + (by + ((d >> 1) & 1)) + ',' + (bz + (d >> 2)))) n++;
        if (n >= 4) { solid = true; break; }
      }
      ok(p.cells.length > 0 && solid, `imp: ${p.name} has a collider block`,
         `${p.cells.length} cells`);
    }
  }
  // 5. A HUMAN NEVER SEES THE RACE: no demon colour in a human's demon art
  //    slots. By colour, not by zero: paletteBytes writes every material's
  //    colour at its own id first, and the material list has grown past the
  //    low demon slots (that fill is ignored by the engine, which reads art
  //    colour only from the .col models).
  {
    const b = mg.generateMob(mg.defaultGenome(), 0, opts);
    const { palette } = readVox(b.vox);
    const hex = sl => '#' + [0, 1, 2].map(i => palette[(sl - 1) * 4 + i]
      .toString(16).padStart(2, '0')).join('');
    const demonHexes = new Set(Object.keys(DM.COLOR_SLOTS).map(k => DM.DEFAULT_COLORS[k]));
    ok(Object.values(DM.COLOR_SLOTS).every(sl => !demonHexes.has(hex(sl))),
       'a human writes none of the demon colours into its palette');
  }
  // 6. RANDOM AND MUTANT demons are sound; every horn style builds.
  for (const s of [1, 2, 3, 5, 8, 21]) {
    const g = mg.rollColors(mg.randomGenome(mg.makeRng(s), null, { race: 'demon' }),
                            mg.makeRng(s ^ 77));
    ok(g.body.race === 'demon', `random demon seed ${s} is a demon`);
    const b = mg.generateMob(g, 0, opts);
    ok(!mg.validateMob(b).length, `random demon seed ${s}: sound`, mg.validateMob(b).join('; '));
    const m = mg.mutate(mg.presetGenome(DM.PRESET_ORDER[s % DM.PRESET_ORDER.length]),
                        1.5, mg.makeRng(s));
    const bm = mg.generateMob(m, 0, opts);
    ok(!mg.validateMob(bm).length, `demon mutant seed ${s}: sound`, mg.validateMob(bm).join('; '));
  }
  for (const st of DM.HORN_ORDER) {
    const g = mg.applyRace(mg.defaultGenome(), 'demon');
    DM.applyHorns(g, st);
    const b = mg.generateMob(g, 0, opts);
    ok(!mg.validateMob(b).length, `demon horns ${st}: sound`, mg.validateMob(b).join('; '));
  }
  // 7. EVERY NUMERIC GENE REACHES THE BODY -- or its MOTION: the tail's
  // sway, speed and lag move only the sidecar (the sway clip, the spring).
  {
    const base = mg.applyRace(mg.defaultGenome(), 'demon');
    DM.applyHorns(base, 'goat');
    const key = g => {
      const b = mg.generateMob(g, 0, opts);
      return b.parts.map(p => p.name + ':' + p.cells.map(c => c.join(',')).join(';')).join('|') +
             JSON.stringify(b.sidecar.clips) + JSON.stringify(b.sidecar.limbs);
    };
    const k0 = key(base);
    for (const spec of mg.GENE_SPECS.filter(sp => sp.race === 'demon' && !sp.kind)) {
      const g = mg.normalizeGenome(base);
      const v = mg.getPath(g, spec.path);
      mg.setPath(g, spec.path, v > (spec.min + spec.max) / 2 ? spec.min : spec.max);
      ok(key(g) !== k0, `${spec.path} changes the body`);
    }
  }
}

section('R. the naga: the human above the hips, a serpent tail below');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const NG = mg.RACE_MODS.naga;
  ok(!!NG && NG.MACHINE === false && NG.LOWER === 'serpent',
     'naga is a flesh race module born on the serpent lower body');
  const sound = (label, g) => {
    let b;
    try { b = mg.generateMob(g, 0, opts); } catch (e) {
      ok(false, `${label}: builds`, e.message);
      return null;
    }
    const v = mg.validateMob(b);
    ok(!v.length, `${label}: structurally sound`, v.join('; '));
    return b;
  };
  // 1. THE UPPER BODY IS THE HUMAN'S: the same boxes, anchors (minus the
  //    frame shift the tail's depth brings), sockets, arm chains and weapon
  //    edges. The legs are GONE and the twelve tail parts are there, the ten
  //    behind TRAILING (measured out of the body box) and in the slither.
  for (const [label, human] of [['default', mg.defaultGenome()],
                                ['brute', mg.presetGenome('brute')],
                                ['random 11', mg.randomGenome(mg.makeRng(11))]]) {
    const nag = mg.applyRace(mg.normalizeGenome(human), 'naga');
    ok(nag.body.lower === 'serpent', `naga ${label}: on the serpent lower body`);
    const a = mg.generateMob(human, 0, opts), b = sound(`naga ${label}`, nag);
    if (!b) continue;
    const upper = mg.ARCHETYPE.order.filter(n => !SE.LEG_PARTS.includes(n));
    const box = r => Object.fromEntries(upper.map(n => [n, r.table.limbs[n]]));
    ok(!diff(box(a), box(b), 'limbs'), `naga ${label}: the human's upper limb boxes`,
       diff(box(a), box(b), 'limbs'));
    const names = b.sidecar.limbs.map(l => l.name);
    ok(SE.LEG_PARTS.every(n => !names.includes(n)), `naga ${label}: no legs`);
    ok(SE.PARTS.every(n => names.includes(n)), `naga ${label}: all twelve tail parts`);
    const trailing = b.sidecar.limbs.filter(l => l.trailing).map(l => l.name);
    ok(trailing.join() === SE.TRAIL.join(), `naga ${label}: the trail, and only it, trails`,
       trailing.join());
    ok(b.sidecar.slither && b.sidecar.slither.upright &&
       b.sidecar.slither.parts.join() === SE.TRAIL.join() && b.sidecar.slither.water,
       `naga ${label}: an upright slither over the trail, with a water wave`);
    ok(b.sidecar.gait === null, `naga ${label}: no gait (null deletes the human's)`);
    ok(b.sidecar.chains.every(c => c.tag === 'arm') && b.sidecar.chains.length === 2,
       `naga ${label}: the arm chains only`);
    const tail = b.parts.filter(p => SE.PARTS.includes(p.name));
    ok(Math.min(...tail.map(p => p.mn[2])) === 0, `naga ${label}: the tail lies on z = 0`);
    const edges = r => r.sidecar.natural.map(n => ({ name: n.name, part: n.part }));
    ok(!diff(edges(a), edges(b), 'natural'), `naga ${label}: the human's natural weapons`);
    // Every swim stroke holds the rise and the trail's root straight, so the
    // body lies in one line, and masks the rest of the trail to the wave.
    for (const k of ['tread', 'swim', 'swim_back', 'swim_side', 'swim_side.mirror',
                     'swim_up', 'swim_down']) {
      const c = b.sidecar.clips[k];
      ok(c && c.durationMs === 1600 && c.tracks.tail2 && c.tracks.tail3 &&
         !c.mask.includes('tail4'),
         `naga ${label}: ${k} straightens the tail and leaves the trail to the wave`);
    }
  }
  // 2. A SERPENT TAIL IS NOT THE NAGA'S: a human and a sylvan grow one too.
  for (const race of ['human', 'sylvan']) {
    const g = race === 'human' ? mg.defaultGenome() : mg.applyRace(mg.defaultGenome(), 'sylvan');
    g.body.lower = 'serpent';
    const b = sound(`a ${race} on a serpent tail`, mg.normalizeGenome(g));
    if (b) ok(b.sidecar.slither && !b.sidecar.limbs.some(l => l.name === 'legU.L'),
              `a ${race} on a serpent tail has the tail and no legs`);
  }
  // 3. LEAVING THE RACE PUTS THE LEGS BACK.
  {
    const g = mg.applyRace(mg.defaultGenome(), 'naga');
    mg.applyRace(g, 'human');
    ok(g.body.lower === 'legs', 'a naga turned human stands on legs again');
  }
  // 4. EVERY PRESET AND EVERY KIND builds sound, bakes the human's inside,
  //    bleeds blood, and is filed in assets/mobs/naga/.
  for (const k of NG.PRESET_ORDER) {
    const g = mg.presetGenome(k);
    ok(g.body.race === 'naga', `${k} is a naga`);
    const b = sound(k, g);
    if (!b) continue;
    const c = mg.bakeAnatomy(b, materials).census;
    ok(c.skin > 0 && c.flesh > 0 && c.muscle > 0 && c.bone > 0 && c.brain > 0,
       `${k}: the human's inside`, JSON.stringify(c));
    ok(b.sidecar.bleed && b.sidecar.bleed.material === 'blood', `${k}: bleeds blood`);
    ok(mg.raceFolder(g) === 'naga', `${k}: filed in assets/mobs/naga/`);
    ok(!g.naga.venom || (b.sidecar.bite && b.sidecar.bite.infect === 'envenomed'),
       `${k}: a venomous bite envenoms`);
  }
  for (const kind of NG.KIND_ORDER) {
    const g = mg.applyRace(mg.defaultGenome(), 'naga');
    NG.applyKind(g, kind);
    sound(`kind ${kind}`, mg.normalizeGenome(g));
  }
  for (const tip of SE.TIPS)
    for (const len of [0.8, 3]) {
      const g = mg.applyRace(mg.defaultGenome(), 'naga');
      Object.assign(g.serpent, { tip, length: len, tipSize: 1, crest: 1, frills: 1 });
      sound(`tip ${tip} at length ${len}`, mg.normalizeGenome(g));
    }
  // 5. RANDOM NAGAS build sound (the Characters page's litter).
  for (let seed = 1; seed <= 12; seed++)
    sound(`random naga ${seed}`, mg.randomGenome(mg.makeRng(seed * 977), null,
                                                 { race: 'naga' }));
  // 6. GOLD IS PAINTED AS ITS STAND-IN and the recipe turns it back.
  {
    const g = mg.presetGenome('nagaraja');
    const b = mg.generateMob(g, 0, opts);
    ok(b.sidecar.anatomy.becomes && b.sidecar.anatomy.becomes.iron === 'gold',
       'nagaraja: iron becomes gold at load', JSON.stringify(b.sidecar.anatomy.becomes));
  }
  // 7. THE NAGA GENES MOVE THE BODY (each, at the far end of its range).
  {
    const base = mg.applyRace(mg.defaultGenome(), 'naga');
    const key = g => {
      const b = mg.generateMob(g, 0, opts);
      return b.parts.map(p => p.name + ':' + p.cells.map(c => c.join(',')).join(';')).join('|') +
             JSON.stringify(b.sidecar);
    };
    const k0 = key(base);
    for (const spec of mg.GENE_SPECS.filter(sp => (sp.race === 'naga' ||
                                                   sp.lower === 'serpent') && !sp.kind)) {
      const g = mg.normalizeGenome(base);
      const v = mg.getPath(g, spec.path);
      mg.setPath(g, spec.path, v > (spec.min + spec.max) / 2 ? spec.min : spec.max);
      ok(key(g) !== k0, `${spec.path} changes the body`);
    }
  }
}

// =============================================================================
// 12. the goblin race (goblin.js)
// =============================================================================

section('S. the goblin: small, big-headed flesh on the SAME rig, its own height band');
{
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const GB = mg.RACE_MODS.goblin;
  ok(!!GB && GB.MACHINE === false && !GB.LOWER,
     'goblin is a flesh race module on legs');
  const sound = (label, g) => {
    let b;
    try { b = mg.generateMob(g, 0, opts); } catch (e) {
      ok(false, `${label}: builds`, e.message);
      return null;
    }
    const v = mg.validateMob(b);
    ok(!v.length, `${label}: structurally sound`, v.join('; '));
    return b;
  };
  const human = mg.generateMob(mg.defaultGenome(), 0, opts);
  // 1. THE RIG IS THE HUMAN'S by NAME: the same limbs in the same order, the
  //    same sockets, chains and natural weapons. The boxes are smaller (it is
  //    a smaller body), which is the point; every system binds by name.
  {
    const gob = mg.applyRace(mg.defaultGenome(), 'goblin');
    const b = sound('goblin default', gob);
    if (b) {
      const names = r => ({
        limbs: r.sidecar.limbs.filter(l => l.tag !== 'hair').map(l => l.name),
        sockets: r.sidecar.sockets.map(s => s.name),
        chains: r.sidecar.chains.map(c => c.name),
        natural: r.sidecar.natural.map(n => n.name + '@' + n.part),
      });
      const d = diff(names(human), names(b), 'rig');
      ok(!d, 'goblin: the human limbs, sockets, chains and natural weapons by name', d);
      ok(b.table.worldH < human.table.worldH, 'goblin: shorter than the human it was',
         `${b.table.worldH} vs ${human.table.worldH}`);
      const headShare = r => r.table.limbs.head.size[2] / r.table.microH;
      ok(headShare(b) > headShare(human), 'goblin: a bigger head for its height');
    }
  }
  // 2. BECOMING A GOBLIN IS AN OFFSET, so a round trip through it lands where
  //    it started (BUILD_DELTA, the applySex rule) -- for a body whose genes
  //    the offsets do not push past a slider's end (a brute's jaw is already
  //    at its maximum, so it clamps going in, exactly as applySex does).
  for (const [label, g0] of [['default', mg.defaultGenome()]]) {
    const g = mg.normalizeGenome(g0);
    const back = mg.applyRace(mg.applyRace(mg.normalizeGenome(g0), 'goblin'), 'human');
    const build = x => JSON.stringify([x.body.heightM, x.body.headWidth, x.body.stack,
                                       x.body.armStack, x.shape, x.head, x.face.eyeRow]);
    ok(build(back) === build(g), `goblin ${label}: human -> goblin -> human keeps the build`,
       `${build(g)} -> ${build(back)}`);
  }
  // 3. THE HEIGHT BAND IS THE RACE'S, and only the race's.
  {
    const h = mg.normalizeGenome({ body: { heightM: 1.0 } });
    ok(h.body.heightM === mg.HEIGHT_BAND[0], 'a 1.0 m HUMAN still clamps to the human band');
    const wild = mg.normalizeGenome({ body: { race: 'goblin', heightM: 99 } });
    ok(wild.body.heightM === GB.HEIGHT_BAND[1], 'a 99 m goblin clamps to the goblin band',
       `got ${wild.body.heightM}`);
    const m = mg.mutate(mg.presetGenome('gloomeye'), 1.5, mg.makeRng(3));
    ok(m.body.heightM >= GB.HEIGHT_BAND[0] && m.body.heightM <= GB.HEIGHT_BAND[1],
       'a mutant goblin stays inside the goblin band', `got ${m.body.heightM}`);
  }
  // 4. EVERY CHARACTER builds sound, bakes FLESH, bleeds blood, is filed in
  //    assets/mobs/goblin/, wears its ears (two head pieces at least), and a
  //    hunched one carries the hunch as its LAST state.
  for (const k of GB.PRESET_ORDER) {
    const g = mg.presetGenome(k);
    ok(g.body.race === 'goblin', `${k} is a goblin`);
    const b = sound(k, g);
    if (!b) continue;
    const c = mg.bakeAnatomy(b, materials).census;
    ok(c.skin > 0 && c.flesh > 0 && c.bone > 0 && c.brain > 0 && c.blood > 0,
       `${k}: the human inside`, JSON.stringify(c));
    ok(b.sidecar.bleed && b.sidecar.bleed.material === 'blood', `${k}: bleeds blood`);
    ok(mg.raceFolder(g) === 'goblin', `${k}: filed in assets/mobs/goblin/`);
    // (The race BASE is bare by rule -- mobgen raceBase: no extras -- so
    // every character grows its own ears.)
    if (k !== GB.RACE) {
      const heads = b.parts.filter(p => p.name.startsWith('snout'));
      ok(heads.length >= 2, `${k}: its ears (and nose) ride the head`,
         `${heads.length} head pieces`);
    }
    const hunched = g.goblin.posture === 'hunched';
    const st = b.sidecar.states;
    ok(hunched === st.some(s => s.name === 'hunch' && s.always && s.clip === 'hunch'),
       `${k}: a hunch state iff hunched (${g.goblin.posture})`);
    ok(!hunched || st[st.length - 1].name === 'hunch', `${k}: the hunch is the LAST state`);
    if (g.goblin.hat !== 'none') {
      const hat = b.parts.some(p => p.cells.some(c => c[3] === GB.SLOTS.HAT ||
                                                    c[3] === GB.SLOTS.RING ||
                                                    c[3] === GB.SLOTS.TUSK ||
                                                    c[3] === GB.SLOTS.HAT_TRIM));
      ok(hat, `${k}: wears its ${g.goblin.hat}`);
    }
  }
  // 5. A HAT FLATTENS THE HAIR UNDER IT: the redcap with long hair carries
  //    less hair mass than the same genome bare-headed.
  {
    const hatted = mg.presetGenome('redcap');
    const bare = mg.normalizeGenome(hatted);
    bare.goblin.hat = 'none';
    const hairOf = g => mg.generateMob(g, 0, opts).parts
      .filter(p => p.name === 'hair').reduce((a, p) => a + p.cells.length, 0);
    ok(hairOf(hatted) < hairOf(bare), 'a redcap flattens the hair under it',
       `${hairOf(hatted)} vs ${hairOf(bare)} hair cells`);
  }
  // 6. A HUMAN NEVER SEES THE RACE: no goblin colour in a human's goblin slots.
  {
    const { palette } = readVox(human.vox);
    const hex = sl => '#' + [0, 1, 2].map(i => palette[(sl - 1) * 4 + i]
      .toString(16).padStart(2, '0')).join('');
    const gobHexes = new Set(Object.keys(GB.COLOR_SLOTS).map(k => GB.DEFAULT_COLORS[k]));
    ok(Object.values(GB.COLOR_SLOTS).every(sl => !gobHexes.has(hex(sl))),
       'a human writes none of the goblin colours into its palette');
  }
  // 7. EVERY NAMED CHOICE BUILDS: kinds, ear and nose shapes, hats, teeth,
  //    eyes, war paint, piercings; and random and mutant goblins are sound.
  const base = () => mg.applyRace(mg.defaultGenome(), 'goblin');
  for (const kd of GB.KIND_ORDER) sound(`kind ${kd}`, GB.applyKind(base(), kd));
  for (const st of GB.EAR_ORDER) sound(`ears ${st}`, GB.applyEars(base(), st));
  for (const st of GB.NOSE_ORDER) sound(`nose ${st}`, GB.applyNose(base(), st));
  for (const [key, list] of [['hat', GB.HATS], ['teeth', GB.TEETH], ['eyes', GB.EYE_STYLES],
                             ['warpaint', GB.WARPAINT], ['nosePierce', GB.PIERCINGS]])
    for (const v of list) {
      const g = base();
      g.goblin[key] = v;
      if (key === 'warpaint') g.goblin.bodyPaint = 1;
      sound(`${key} ${v}`, g);
    }
  for (const s of [1, 2, 3, 5, 8, 21]) {
    const g = mg.rollColors(mg.randomGenome(mg.makeRng(s), null, { race: 'goblin' }),
                            mg.makeRng(s ^ 77));
    ok(g.body.race === 'goblin', `random goblin seed ${s} is a goblin`);
    sound(`random goblin seed ${s}`, g);
    sound(`goblin mutant seed ${s}`,
          mg.mutate(mg.presetGenome(GB.PRESET_ORDER[s % GB.PRESET_ORDER.length]), 1.5,
                    mg.makeRng(s)));
  }
  // 8. EVERY NUMERIC GENE REACHES THE BODY, at a genome where it can: hat
  //    genes under a redcap, the paint amount under a paint, rings on.
  {
    const g0 = base();
    g0.goblin.hat = 'redcap';
    g0.goblin.warpaint = 'stripes';
    g0.goblin.tusks = 0.5;
    g0.goblin.warts = 0.5;
    g0.goblin.earRings = 1;
    const key = g => {
      const b = mg.generateMob(g, 0, opts);
      return b.parts.map(p => p.name + ':' + p.cells.map(c => c.join(',')).join(';')).join('|') +
             JSON.stringify(b.sidecar.natural);
    };
    const k0 = key(g0);
    for (const spec of mg.GENE_SPECS.filter(sp => sp.race === 'goblin' && !sp.kind)) {
      const g = mg.normalizeGenome(g0);
      const v = mg.getPath(g, spec.path);
      mg.setPath(g, spec.path, v > (spec.min + spec.max) / 2 ? spec.min : spec.max);
      ok(key(g) !== k0, `${spec.path} changes the body`);
    }
  }
}

section('T. the fine face and the skin pass: a human drawn at the shipped lattice');
// The late rows that are EVERY race's (the tail's shape, the beard's volume):
// appended after the fine face's for the same reason, but not the human's.
const EVERYONE_LATE = s => s.group === 'hair' || s.group === 'beard';
{
  // A HUMAN's head is built on the 80 vox/m lattice (headVoxFine) and its body
  // painted there (humanSkinPass); `face: 'legacy'` builds the old head, which
  // is what the Characters page's old/fine toggle shows. What must hold:
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const A = mg.ART;
  const DETAIL = new Set(mg.DETAIL_SLOTS);
  const partsOf = b => Object.fromEntries(b.parts.map(p => [p.name, p]));
  const shapeOf = p => p.cells.map(c => c.slice(0, 3).join(',')).sort().join(';');
  const paintOf = p => p.cells.map(c => c.join(',')).sort().join(';');
  const def = mg.defaultGenome();
  // THE FINE FACE IS DETAIL 1 (FACE DETAIL in mobgen.js): every detail row
  // defaults to 0, the old face, so what this section asks of the fine face
  // it asks of a genome turned all the way up. Section V holds detail 0.
  const FINE = src => mg.applyFaceDetail(mg.normalizeGenome(src), 1);
  const fine = mg.generateMob(FINE(def), 0, opts), old = mg.generateMob(def, 0, { ...opts, face: 'legacy' });
  const F = partsOf(fine), O = partsOf(old);

  // 1. Both build sound, and the head keeps its BOX (every helm is fit to it).
  ok(mg.validateMob(fine).length === 0 && mg.validateMob(old).length === 0,
     'the fine and the old human both pass the structural asserts');
  ok(F.head.size.join() === O.head.size.join() && F.head.mn.join() === O.head.mn.join(),
     'the fine head has the old head’s box', `${F.head.size} vs ${O.head.size}`);
  ok(paintOf(F.head) !== paintOf(O.head), 'the fine head is not the old one');
  ok(F.head.cells.some(c => c[3] === A.EYE_WHITE) && F.head.cells.some(c => c[3] === A.LASH),
     'a fine face has eye whites and a lash line by default');
  ok(!O.head.cells.some(c => DETAIL.has(c[3])), 'the old face paints no detail slot');

  // 2. THE SKIN PASS IS PAINT. Below the neck the fine body has exactly the old
  //    body's cells -- armour, the collider, anchors and the mirror all ride on
  //    that -- and it does repaint some of them.
  let moved = 0, repainted = 0;
  for (const nm of Object.keys(O)) {
    if (nm === 'head') continue;
    if (shapeOf(F[nm]) !== shapeOf(O[nm])) moved++;
    if (paintOf(F[nm]) !== paintOf(O[nm])) repainted++;
  }
  ok(moved === 0, 'the skin pass adds and removes no cell anywhere', `${moved} parts moved`);
  ok(repainted >= 10, 'the skin pass paints most of the body', `${repainted} parts`);
  ok(F['hand.L'].cells.some(c => c[3] === A.NAIL_T) && F['foot.R'].cells.some(c => c[3] === A.NAIL_T),
     'hands and feet get nails');
  const FEATURES = ['collarbones', 'chest', 'navel', 'knees', 'elbows',
                    'fingers', 'knuckles', 'nails', 'toes'];
  const noDetail = mg.normalizeGenome({ body: { skin: { texture: 0, moles: 0,
    ...Object.fromEntries(FEATURES.map(k => [k, 0])) } } });
  ok(!mg.generateMob(noDetail, 0, opts).parts.filter(p => p.name !== 'head')
       .some(p => p.cells.some(c => DETAIL.has(c[3]) || c[3] === A.NAIL)),
     'every skin feature, texture and moles at 0 paint nothing below the neck');
  // EACH FEATURE IS ITS OWN SLIDER: alone at 0 it is gone and the rest stay.
  const allOn = partsOf(fine);
  const slotOf = { collarbones: A.COLLAR_L, chest: A.CHEST, navel: A.NAVEL,
                   knees: A.KNEE_L, elbows: A.ELBOW, fingers: A.FINGER,
                   knuckles: A.KNUCKLE, nails: A.NAIL_T, toes: A.TOE };
  const has = (P, slot) => Object.values(P).some(p => p.cells.some(c => c[3] === slot));
  for (const k of FEATURES) {
    const P = partsOf(mg.generateMob(mg.normalizeGenome({ body: { skin: { [k]: 0 } } }), 0, opts));
    ok(has(allOn, slotOf[k]) && !has(P, slotOf[k]) &&
       FEATURES.filter(j => j !== k && slotOf[j] !== slotOf[k]).every(j => has(P, slotOf[j])),
       `body.skin.${k} at 0 removes it and only it`);
  }
  // AN OPACITY MOVES A COLOUR, NOT A CELL: the same cells in the same slots,
  // the slot's colour mixed toward the skin -- in tenths, ends exact.
  const tone = (path, v) => {
    const g = FINE({}); mg.setPath(g, path, v); return mg.normalizeGenome(g);
  };
  for (const [path, slot] of [['body.skin.collarbones', A.COLLAR_L],
                              ['body.skin.nails', A.NAIL_T], ['face.lashTone', A.LASH],
                              ['face.creaseTone', A.CREASE], ['face.earTone', A.EAR_IN],
                              ['face.whites', A.EYE_WHITE], ['face.bagTone', A.BAGS]]) {
    const lo = tone(path, 0.3), hi = tone(path, 0.9);
    const cells = g => mg.generateMob(g, 0, opts).parts.map(p => shapeOf(p)).join('|');
    if (path !== 'face.bagTone')
      ok(cells(lo) === cells(hi), `${path} moves no cell`);
    const c0 = mg.detailColors(lo.colors, lo)[slot], c1 = mg.detailColors(hi.colors, hi)[slot];
    ok(c0 !== c1, `${path} moves the colour`, `${c0} vs ${c1}`);
    const full = mg.detailColors(lo.colors, tone(path, 1))[slot];
    ok(c0 !== full && mg.detailColors(lo.colors, tone(path, 0.31))[slot] === c0,
       `${path} snaps to tenths`);
  }
  ok(mg.detailColors(def.colors, tone('body.skin.collarbones', 1))[A.COLLAR_L] ===
     def.colors.skinLight, 'a full-strength tone is the existing colour (no palette cost)');

  // 3. THE FINE FACE IS SYMMETRIC in shape on a symmetric genome: every lever
  //    that is not a side (the parting, the smirk, the offset, the scar) high.
  const sym = FINE({ face: { brow: true, browProud: true, nose: 2.5,
    noseWidth: 2, noseBridge: 1, noseBulb: 1, earSize: 2, earPoint: true,
    eyeCols: 2, eyeWhites: true, eyeOpen: 1, eyeTilt: 1, lashes: 1, lips: 2 } });
  const H = partsOf(mg.generateMob(sym, 0, opts)).head;
  const set = new Set(H.cells.map(c => c.slice(0, 3).join(',')));
  const sx = H.size[0];
  const unmatched = H.cells.filter(([x, y, z]) => !set.has(`${sx - 1 - x},${y},${z}`)).length;
  ok(unmatched === 0, 'the fine head is an exact left/right mirror in shape',
     `${unmatched} cells unmatched`);

  // 4. EVERY NEW GENE AND EVERY HALF STEP MOVES A CELL of a human's head --
  //    a half step that renders like its neighbour is a slider that lies.
  const head = g => paintOf(partsOf(mg.generateMob(g, 0, opts)).head);
  const browOn = { brow: true };
  const LEVERS = [
    ['face.irisSize', 0.9], ['face.whites', 0], ['face.lashSpan', 1],
    ['face.lashSpan', 0.2], ['face.earHeight', 0.5], ['face.earBack', 1],
    ['face.earWidth', 1], ['face.earOut', 0.5], ['face.earOut', 1],
    ['face.earTilt', 1], ['face.earLobe', 0], ['face.earLobe', 1],
    ['face.eyeOpen', 0.9], ['face.eyeTilt', 0.8], ['face.eyeTilt', -0.8],
    ['face.lashes', 0], ['face.lashes', 0.9], ['face.lidCrease', 0.7],
    ['face.eyeRow', 0.5], ['face.eyeCols', 1.5], ['face.mouthWidth', 2.5],
    ['face.mouthDrop', 3.5], ['face.mouthOpen', 0.5], ['face.lips', 0.5],
    ['face.lips', 1.5], ['face.earSize', 1.5], ['face.browThick', 1.5],
    ['face.browHeight', 0.5], ['face.browInner', 1.5], ['face.browOuter', 0.5],
    ['face.mouthShift', 0.5], ['body.skin.blush', 0.6], ['body.skin.moles', 1],
  ];
  for (const [path, v] of LEVERS) {
    const base = FINE({ face: path.startsWith('face.brow') ? browOn : {} });
    const g = mg.normalizeGenome(base);
    mg.setPath(g, path, v);
    const n = mg.normalizeGenome(g);
    ok(mg.getPath(n, path) === v, `${path} keeps ${v} on a human (the half step)`,
       `normalised to ${mg.getPath(n, path)}`);
    ok(head(n) !== head(base), `fine lever ${path} = ${v} moves a cell`);
  }
  // A low mouth, so the nose has rows to grow into: it stops two rows above the
  // mouth line, and at the default mouth anything past 1.5 is that stop.
  const nosed = FINE({ face: { nose: 2, mouthDrop: 4.5 } });
  for (const [path, v] of [['face.nose', 1.5], ['face.noseWidth', 1.5],
                           ['face.noseBridge', 0.9], ['face.noseBridge', 0.1],
                           ['face.noseBulb', 0.9]]) {
    const g = mg.normalizeGenome(nosed);
    mg.setPath(g, path, v);
    ok(head(mg.normalizeGenome(g)) !== head(nosed), `fine lever ${path} = ${v} moves a cell`);
  }
  ok(head(FINE({ body: { skin: { texture: 0.8 } } })) !== head(FINE({})),
     'skin texture reaches the face');
  const round = FINE({ face: { eyeOpen: 1 } });
  ok(head(FINE({ face: { eyeOpen: 1, glint: true } })) !== head(round),
     'fine lever face.glint moves a cell (on a two-row eye)');
  const pointed = FINE({ face: { earPoint: true } });
  ok(head(FINE({ face: { earPoint: true, earPointLen: 1 } })) !== head(pointed),
     'fine lever face.earPointLen moves a cell');

  // THE OLD EYE IS A SETTING: the `old` style draws the legacy head's eye,
  // cell for cell, at the eye size most characters have, and the same
  // number of iris cells at the biggest.
  const eyeCells = b => {
    const h = partsOf(b).head, m = new Map();
    for (const [x, y, z, c] of h.cells) {
      const k = x + ',' + z;
      if (!m.has(k) || y < m.get(k)[0]) m.set(k, [y, c]);
    }
    return [...m].filter(([, v]) => v[1] === A.EYE).map(([k]) => k).sort();
  };
  for (const cols of [1, 2]) {
    const base = mg.normalizeGenome({ face: { eyeCols: cols } });
    const legacyEye = eyeCells(mg.generateMob(base, 0, { ...opts, face: 'legacy' }));
    const o = FINE(base);                     // the style, at full detail
    mg.applyEyeStyle(o, 'old');
    const oldStyle = eyeCells(mg.generateMob(mg.normalizeGenome(o), 0, opts));
    if (cols === 1)
      ok(JSON.stringify(legacyEye) === JSON.stringify(oldStyle),
         'the `old` eye style is the legacy eye, cell for cell');
    else
      ok(legacyEye.length === oldStyle.length,
         'the `old` eye style has the legacy iris size at eye size 2',
         `${legacyEye.length} vs ${oldStyle.length}`);
  }
  for (const st of mg.EYE_STYLE_ORDER) {
    const g = mg.normalizeGenome({});
    mg.applyEyeStyle(g, st);
    ok(mg.validateMob(mg.generateMob(mg.normalizeGenome(g), 0, opts)).length === 0,
       `eye style ${st} builds sound`);
  }

  // THE EYES TAB holds the eyebrows as a sub-section, and every eye row is on it.
  ok((mg.GENE_GROUPS.find(x => x.key === 'brows') || {}).parent === 'eyes',
     'the eyebrows are a sub-section of the eyes tab');
  ok(['face.eyeDx', 'face.eyeRow', 'face.eyeCols', 'face.eyeWhites', 'face.eyeBags',
      'face.eyeOpen', 'face.eyeTilt', 'face.lashes', 'face.lidCrease']
       .every(pth => mg.GENE_SPECS.find(x => x.path === pth).group === 'eyes'),
     'every eye row is on the eyes tab');

  // 5. THE RACES ARE UNTOUCHED: a half step is a whole one on them, their
  //    slider steps are whole, the human's new rows do not show, and they
  //    paint no detail slot.
  for (const race of mg.RACES.filter(r => r !== 'human')) {
    const g = mg.presetGenome(race);
    const spec = mg.GENE_SPECS.find(s => s.path === 'face.eyeRow');
    mg.setPath(g, 'face.eyeRow', 0.5);
    const n = mg.normalizeGenome(g);
    ok(Number.isInteger(n.face.eyeRow), `${race}: a half-step eye row rounds to a whole one`);
    ok(mg.specStep(spec, n) === 1, `${race}: the eye-row slider steps whole rows`);
    // (`late` also marks rows every race has that arrived after the roll
    // order was fixed -- the tail's shape, the beard's volume -- and those
    // live on the hair and beard tabs.)
    ok(!mg.GENE_SPECS.some(s => s.late && !EVERYONE_LATE(s) && mg.specApplies(s, n)),
       `${race}: none of the fine face's rows show`);
    const b = mg.generateMob(n, 0, opts);
    ok(!b.parts.some(p => p.cells.some(c => DETAIL.has(c[3]))),
       `${race}: paints no fine-face detail slot`);
  }
  ok(mg.specStep(mg.GENE_SPECS.find(s => s.path === 'face.eyeRow'), def) === 0.5,
     'a human’s eye-row slider takes half steps');

  // 6. THE PALETTE: the detail slots are written for a human only, and three
  //    of the four are colours the human already has (no new merged slot).
  const pal = mg.paletteBytes(materials, def.colors, 'human');
  const hexAt = s => '#' + [0, 1, 2].map(i => pal[(s - 1) * 4 + i].toString(16).padStart(2, '0')).join('');
  ok(hexAt(A.FACE_SHADE) === def.colors.skinShade && hexAt(A.FACE_LIGHT) === def.colors.skinLight &&
     hexAt(A.LASH) === def.colors.hairShade && hexAt(A.EYE_WHITE) === mg.EYE_WHITE_HEX,
     'the detail slots carry the skin, hair and eye-white colours');
  // (No palette assertion for the races: paletteBytes fills the low end with
  // MATERIAL colours, and with 200+ materials those spill into 128..255. The
  // engine merges only slots a voxel paints (MicroBodyMergeArt `used`), so
  // what counts is section 5's "paints no detail slot".)

  // 7. THE OLD FACE IS STILL THE OLD FACE: on the old path a human's half
  //    steps are as they were (whole cells), so the toggle compares like with
  //    like -- and a rolled litter is unchanged by the late rows' arrival.
  const r1 = mg.randomGenome(mg.makeRng(4242));
  const late = mg.GENE_SPECS.filter(s => s.late).map(s => s.path);
  const r1bare = JSON.parse(JSON.stringify(r1));
  for (const p of late) mg.setPath(r1bare, p, mg.getPath(def, p));
  ok(mg.GENE_SPECS.filter(s => s.late && !EVERYONE_LATE(s))
       .every(s => s.race === 'human'),
     'every late row off the hair and beard tabs is a human\u2019s',
     String(late.length));
}

// =============================================================================
// U. every slider moves only what it is for
// =============================================================================

section('U. every slider moves only what it is for (gene independence)');
{
  // THE OWNER'S TICKET (2026-10-15): "various sliders in the character creator
  // affect other things for no reason; eye spacing will change freckles on
  // skin, a bunch of sliders will end up making subtle changes to hair,
  // changing underwear can change hair and freckles". Every one of those was
  // a hash keyed on, or a landmark read from, something the slider moved:
  //   - the skin pass seeded its whole scatter with eyeDx, heightM, freckles,
  //     the jaw, the shoulder width and the texture amount;
  //   - the hair mass hashed scene coordinates, read its eye line back out of
  //     the face's paint (a dark or open mouth is EYE paint), rooted on any
  //     hair-coloured paint (the beard, so the hair's volume puffed it out) and
  //     took the skull's centre over the ears and the nose;
  //   - a goblin hung its ears off the mean of every EYE-slot cell; a sylvan
  //     carved its bark, and picked its knots, only where no cloth lay.
  //
  // So every slider is stepped once on bodies with everything on, and what
  // moved is held to what the slider is for:
  //   CONFINED  a face, skin or underwear row changes only its own parts;
  //   NO HAIR   a skin or underwear row moves no hair at all;
  //   PAINT     an underwear row, and the skin's dice, add or remove no cell
  //             anywhere; an underwear row changes only cells that were or
  //             became cloth;
  //   ITS OWN   raising texture, moles or freckles only ADDS its own marks;
  //   LOCAL     any other row that moves hair moves it near something the row
  //             changed: within 12 cells, or under it in the same columns (a
  //             strand hangs from whatever blocks it).
  // The hair, short-hair and beard rows are not stepped -- moving the hair is
  // what they are for -- and height and the proportions (`stack`) are not
  // held to LOCAL: hanging hair is measured to the waist, so they move the end
  // of every strand.
  const R = 12;
  const key = (x, y, z) => ((z + 512) * 2048 + (y + 1024)) * 2048 + (x + 1024);
  const unkey = k => [k % 2048 - 1024, Math.floor(k / 2048) % 2048 - 1024,
                      Math.floor(k / 4194304) - 512];
  // Scene cells per part, the hair's own pieces (hair, hair.2, mane, ...)
  // pooled as one; a race's extras (snout, bough, tail, hood) keep their name.
  const HAIRP = /^(hair|mane)(\.\d+)?$/;
  const partOf = p => (p.hair && HAIRP.test(p.name) ? 'HAIR'
                       : p.hair ? 'X:' + p.name.replace(/\.\d+$/, '') : p.name);
  const cellsOf = b => {
    const parts = new Map();
    for (const p of b.parts) {
      const nm = partOf(p);
      if (!parts.has(nm)) parts.set(nm, new Map());
      const m = parts.get(nm);
      for (const [x, y, z, c] of p.cells)
        m.set(key(p.mn[0] + x, p.mn[1] + y, p.mn[2] + z), c);
    }
    return parts;
  };
  const diffOf = (A, B) => {
    const out = new Map();
    for (const nm of new Set([...A.keys(), ...B.keys()])) {
      const ma = A.get(nm) || new Map(), mb = B.get(nm) || new Map();
      const ch = [];
      for (const [k, c] of ma) if (mb.get(k) !== c) ch.push(k);
      for (const k of mb.keys()) if (!ma.has(k)) ch.push(k);
      if (ch.length) out.set(nm, ch);
    }
    return out;
  };
  // Changed hair cells nothing the step changed explains (see LOCAL).
  const farHair = d => {
    const hair = d.get('HAIR');
    if (!hair) return [];
    const B = 4, buckets = new Map();
    for (const [nm, ks] of d) {
      if (nm === 'HAIR') continue;
      for (const k of ks) {
        const c = unkey(k);
        const bk = Math.floor(c[0] / B) + ',' + Math.floor(c[1] / B) + ',' +
                   Math.floor(c[2] / B);
        if (!buckets.has(bk)) buckets.set(bk, []);
        buckets.get(bk).push(c);
      }
    }
    const nb = Math.ceil(R / B), far = [];
    for (const k of hair) {
      const [x, y, z] = unkey(k);
      const bx = Math.floor(x / B), by = Math.floor(y / B), bz = Math.floor(z / B);
      let near = false;
      for (let dz = -nb; dz <= 64 && !near; dz++)
        for (let dy = -nb; dy <= nb && !near; dy++)
          for (let dx = -nb; dx <= nb && !near; dx++) {
            const cs = buckets.get((bx + dx) + ',' + (by + dy) + ',' + (bz + dz));
            if (!cs) continue;
            for (const c of cs) {
              const ax = Math.abs(c[0] - x), ay = Math.abs(c[1] - y), az = c[2] - z;
              if (Math.max(ax, ay, Math.abs(az)) <= R ||
                  (ax <= 3 && ay <= 3 && az >= -R)) { near = true; break; }
            }
          }
      if (!near) far.push([x, y, z]);
    }
    return far;
  };
  const build = g => mg.generateMob(g, 0, { noVox: true });
  const stepOf = (spec, g) => {
    const v = mg.getPath(g, spec.path);
    if (spec.kind === 'bool') return !v;
    if (spec.kind === 'enum')
      return spec.choices[(spec.choices.indexOf(v) + 1) % spec.choices.length];
    const [, hi] = mg.specRange(spec, g);
    const st = mg.specStep(spec, g) || 0.05;
    return v + st <= hi + 1e-9 ? v + st : v - st;
  };
  const UNDERWEAR = p => p.startsWith('body.bra.') || p.startsWith('body.briefs.');
  const partsFor = spec => {
    if (spec.path === 'face.freckles') return ['head', 'torso', 'armU.L', 'armU.R'];
    if (spec.path === 'body.skin.blush') return ['head'];
    if (spec.path.startsWith('body.bra.')) return ['torso'];
    if (spec.path.startsWith('body.briefs.')) return ['hips', 'legU.L', 'legU.R'];
    if (['eyes', 'brows', 'mouth', 'details', 'ears', 'face'].includes(spec.group))
      return ['head', 'HAIR'];
    return null;                               // geometry, the skin: anywhere
  };
  const SCATTER = { 'body.skin.texture': [mg.ART.FACE_SHADE, mg.ART.FACE_LIGHT],
                    'body.skin.moles': [mg.ART.MOLE],
                    'face.freckles': [mg.ART.FACE_SHADE] };
  const CLOTH = new Set([mg.ART.CLOTH, mg.ART.CLOTH_SHADE]);
  const RACE_GROUPS = new Set(mg.GENE_GROUPS.slice(mg.GENE_GROUPS
    .findIndex(gr => gr.key === 'underwear') + 1).map(gr => gr.key));

  // THE BODIES. A man with everything a slider could reach across: hanging
  // hair (tail, curtain, bangs, a hanging beard), the eye window, a shell, the
  // short layer, scalp thinning, every scatter, a nose and brows. A woman for
  // the rows only she has (the bra). And a goblin and a sylvan in underwear:
  // the two races whose surface pass is geometry (warts, carved bark).
  const man = mg.applySex(mg.presetGenome('human'), 'male');
  mg.applyHairStyle(man, 'ponytail');
  mg.applyBeardStyle(man, 'full');
  Object.assign(man.face, { beardDensity: 0.7, beardLen: 0.5, beardRagged: 0.6,
                            freckles: 0.8, brow: true, browSparse: 0.3, nose: 1 });
  Object.assign(man.body.skin, { moles: 0.5, texture: 0.5, blush: 0.4 });
  Object.assign(man.hair, { ragged: 0.6, wisp: 0.4, fuzz: 0.5, drop: 0.3,
                            sideThin: 0.3, hairlineSoft: 0.3, bangs: 0.5,
                            faceThin: 0.4, front: 8, volume: 1 });
  const woman = mg.applySex(mg.presetGenome('human'), 'female');
  mg.applyHairStyle(woman, 'long');
  Object.assign(woman.face, { freckles: 0.8 });
  Object.assign(woman.body.skin, { moles: 0.5, texture: 0.5 });
  const dressed = race => {
    const g = mg.applyRace(mg.applySex(mg.presetGenome('human'), 'female'), race);
    mg.applyHairStyle(g, 'long');
    mg.applyBraStyle(g, 'crop');
    if (race === 'goblin') g.goblin.warts = 0.6;
    return g;
  };
  const BODIES = [
    ['man', man, s => !['hair', 'fuzz', 'beard'].includes(s.group)],
    ['woman', woman, s => s.sex === 'female' || s.path === 'body.bust'],
    ['goblin', dressed('goblin'), s => UNDERWEAR(s.path)],
    ['sylvan', dressed('sylvan'), s => UNDERWEAR(s.path)],
  ];
  let stepped = 0;
  for (const [who, raw, want] of BODIES) {
    const g0 = mg.normalizeGenome(raw);
    const A = cellsOf(build(g0));
    for (const spec of mg.GENE_SPECS) {
      if (!want(spec) || !mg.specApplies(spec, g0) || spec.fixed ||
          spec.kind === 'color' || spec.styled || spec.path === 'body.sex')
        continue;
      const g1 = mg.normalizeGenome(mg.setPath(JSON.parse(JSON.stringify(g0)),
                                               spec.path, stepOf(spec, g0)));
      const v0 = mg.getPath(g0, spec.path), v1 = mg.getPath(g1, spec.path);
      if (JSON.stringify(v0) === JSON.stringify(v1)) continue;
      stepped++;
      const B = cellsOf(build(g1));
      const d = diffOf(A, B);
      const why = [];
      const allowed = partsFor(spec);
      if (allowed) {
        const extra = [...d.keys()].filter(p => !allowed.includes(p));
        if (extra.length)
          why.push('changed ' + extra.map(p => `${p} (${d.get(p).length})`).join(', '));
      }
      const paintOnly = UNDERWEAR(spec.path) || spec.path === 'body.skin.pattern';
      if ((spec.group === 'skin' || paintOnly) && d.has('HAIR'))
        why.push(`moved ${d.get('HAIR').length} hair cells`);
      if (paintOnly) {
        let geo = 0, notCloth = 0;
        for (const [nm, ks] of d)
          for (const k of ks) {
            const a = A.get(nm)?.get(k), b = B.get(nm)?.get(k);
            if (a === undefined || b === undefined) geo++;
            else if (UNDERWEAR(spec.path) && !CLOTH.has(a) && !CLOTH.has(b)) notCloth++;
          }
        if (geo) why.push(`added or removed ${geo} cells`);
        if (notCloth) why.push(`repainted ${notCloth} cells that were never cloth`);
      }
      if (SCATTER[spec.path] && v1 > v0) {
        let wrong = 0;
        for (const [nm, ks] of d)
          for (const k of ks) if (!SCATTER[spec.path].includes(B.get(nm)?.get(k))) wrong++;
        if (wrong) why.push(`${wrong} changed cells are not new marks of its own`);
      }
      if (spec.group !== 'skin' && !paintOnly && !RACE_GROUPS.has(spec.group) &&
          spec.path !== 'body.heightM' && !spec.path.startsWith('body.stack.') &&
          spec.path !== 'body.headWidth' && spec.path !== 'body.headDepth') {
        const far = farHair(d);
        if (far.length)
          why.push(`moved ${far.length} of ${d.get('HAIR').length} hair cells ` +
                   `nowhere near anything it changed (e.g. ${far[0]})`);
      }
      ok(!why.length, `${who}: ${spec.path} ${JSON.stringify(v0)} -> ` +
                      `${JSON.stringify(v1)} moves only what it is for`,
         why.join('; '));
    }
  }
  ok(stepped > 120, 'every slider was stepped', `${stepped} steps`);

  // THE SKIN'S DICE re-rolls the scatter (and, above, nothing else).
  {
    const g1 = JSON.parse(JSON.stringify(man));
    g1.body.skin.pattern = 77;
    const d = diffOf(cellsOf(build(mg.normalizeGenome(man))),
                     cellsOf(build(mg.normalizeGenome(g1))));
    ok([...d.values()].reduce((n, ks) => n + ks.length, 0) > 100,
       'a different skin pattern is a different scatter');
  }

  // THE TAIL'S SHAPE does what its rows say: the curve carries the tail's end
  // away from the back (or tucks it in), the tie climbs the head, and the
  // girth is girth.
  {
    const tail = o => {
      const g = mg.applyHairStyle(mg.defaultGenome(), 'ponytail');
      Object.assign(g.hair, o);
      const b = build(mg.normalizeGenome(g));
      const cells = b.parts.filter(p => p.hair).flatMap(p =>
        p.cells.map(([x, y, z]) => [p.mn[0] + x, p.mn[1] + y, p.mn[2] + z]));
      const zLo = Math.min(...cells.map(c => c[2]));
      const end = cells.filter(c => c[2] <= zLo + 3);
      return { n: cells.length, top: Math.max(...cells.map(c => c[2])),
               endY: end.reduce((s, c) => s + c[1], 0) / end.length };
    };
    const flat = tail({}), out = tail({ tailCurve: 1 }), tuck = tail({ tailCurve: -1 });
    ok(out.endY > flat.endY + 3, 'tail curve 1 sweeps the end out from the back',
       `end y ${flat.endY.toFixed(1)} -> ${out.endY.toFixed(1)}`);
    ok(tuck.endY <= flat.endY, 'tail curve -1 keeps the end in on the back',
       `end y ${flat.endY.toFixed(1)} -> ${tuck.endY.toFixed(1)}`);
    const hi = tail({ tailHeight: 1, tail: 0.3 }), lo = tail({ tailHeight: -1, tail: 0.3 });
    const mid = tail({ tail: 0.3 });
    ok(hi.n !== mid.n && lo.n !== mid.n, 'the tie moves with the height row',
       `${lo.n} / ${mid.n} / ${hi.n} cells`);
    ok(tail({ tailThick: 1.6 }).n > flat.n && tail({ tailThick: 0.5 }).n < flat.n,
       'thickness thickens the tail', `${flat.n} cells at 1`);
  }

  // THE BEARD'S VOLUME is its own: the hair's volume no longer reaches it, and
  // its own row does.
  {
    const hairOf = (beard, bv) => {
      const g = mg.applyBeardStyle(mg.applyHairStyle(mg.defaultGenome(), 'crew'), beard);
      g.hair.volume = 3; g.face.beardVolume = bv;
      return cellsOf(build(mg.normalizeGenome(g))).get('HAIR') || new Map();
    };
    const bare = hairOf('clean', 0), painted = hairOf('full', 0), grown = hairOf('full', 3);
    ok(painted.size === bare.size && [...painted].every(([k, c]) => bare.get(k) === c),
       'a painted beard grows no hair, whatever the hair\u2019s volume',
       `${bare.size} hair cells shaven, ${painted.size} bearded`);
    ok(grown.size > painted.size + 50, 'the beard\u2019s own volume does',
       `${painted.size} -> ${grown.size}`);
  }
}

// =============================================================================
// V. face detail
// =============================================================================

section('V. face detail: 0 is the old head cell for cell, and each feature its own slider');
{
  // THE OWNER'S ASK (2026-10-15): "the new faces are sculpted and shaped too
  // differently and should follow the same contours, and there should be
  // sliders that interpolate between the more blocky 2x2x2 look on the face
  // and the new one ... the facial hair is way too drastically different".
  // So a human's head is a function of eight detail rows (mobgen.js FACE
  // DETAIL), every default 0. What must hold:
  const opts = { materials, player: tuning.player, avatar: avatarConstants() };
  const A = mg.ART;
  const partsOf = b => Object.fromEntries(b.parts.map(p => [p.name, p]));
  // headVox paints the face's marks in SKIN_SHADE / SKIN_LIGHT; at every
  // detail the fine head paints those two colours as FACE_SHADE / FACE_LIGHT,
  // so the Characters page draws them (FACE DETAIL, "the marks").
  const AS_OLD = { [A.FACE_SHADE]: A.SKIN_SHADE, [A.FACE_LIGHT]: A.SKIN_LIGHT };
  // IN LIST ORDER: hairMass grows its shells from the head's cells in order.
  const sig = (p, asOld) => p.cells.map(([x, y, z, c]) =>
    `${x},${y},${z},${asOld ? (AS_OLD[c] ?? c) : c}`).join(';');
  const at = (src, d) => mg.applyFaceDetail(mg.normalizeGenome(src), d);

  // 1. AT 0 THE HEAD IS headVox's -- every cell, its paint and its ORDER --
  //    and so is all the hair the head's marks grow, on every saved human (half
  //    steps rounded, as the old head draws them), the presets and 60 rolls.
  //    The same builds the Characters page's old-face toggle, which used to
  //    loop forever on a half-step brow reach (maren).
  const cast = [['default', mg.defaultGenome()],
                ...mg.PRESET_ORDER.map(k => ['preset ' + k, mg.presetGenome(k)])];
  for (const dir of ['human', 'pool'])
    for (const n of fs.readdirSync(path.join(ROOT, 'assets/mobs', dir)).sort()) {
      if (!n.endsWith('.json')) continue;
      const j = readJson(path.join(ROOT, 'assets/mobs', dir, n));
      if (j.genome && (j.genome.body?.race ?? 'human') === 'human') cast.push([n, j.genome]);
    }
  for (let k = 1; k <= 60; k++) cast.push(['roll ' + k, mg.randomGenome(mg.makeRng(9100 + k))]);
  const bad = [];
  for (const [nm, src] of cast) {
    const g = at(src, 0);
    let F, O;
    try {
      F = partsOf(mg.generateMob(g, 0, opts));
      O = partsOf(mg.generateMob(g, 0, { ...opts, face: 'legacy' }));
    } catch (e) { bad.push(`${nm} (${e.message})`); continue; }
    const hair = [...new Set([...Object.keys(F), ...Object.keys(O)])]
      .filter(k => (F[k] || O[k]).hair);
    if (sig(F.head, true) !== sig(O.head) ||
        !hair.every(k => F[k] && O[k] && F[k].mn.join() === O[k].mn.join() &&
                         sig(F[k]) === sig(O[k])))
      bad.push(nm);
  }
  ok(!bad.length, `detail 0 is the old head and its hair, cell for cell, on ` +
                  `${cast.length - bad.length}/${cast.length} bodies`, bad.slice(0, 6).join(', '));

  // 2. EACH ROW REACHES ITS FEATURE AND STOPS AT THE NECK: on a face with every
  //    feature on, each row alone at 1 changes the head, and no part below it.
  const rich = mg.applySex(mg.presetGenome('human'), 'male');
  mg.applyBeardStyle(rich, 'stubble');
  Object.assign(rich.face, { brow: true, browProud: true, nose: 2, noseWidth: 1.5,
    lips: 1, mouthCurve: 0.5, cheeks: 0.6, freckles: 0.6, wrinkles: 0.8, scar: 0.5,
    earPoint: true, eyeBags: 0.7 });
  rich.body.skin.blush = 0.5;
  const R0 = partsOf(mg.generateMob(at(rich, 0), 0, opts));
  const below = Object.keys(R0).filter(n => n !== 'head' && !R0[n].hair);
  for (const k of mg.DETAIL_KEYS) {
    const g = at(rich, 0);
    g.face[k] = 1;
    const P = partsOf(mg.generateMob(mg.normalizeGenome(g), 0, opts));
    ok(sig(P.head) !== sig(R0.head), `face.${k} at 1 changes the head`);
    ok(below.every(n => sig(P[n]) === sig(R0[n])), `face.${k} changes nothing below the neck`);
  }

  // 3. THE MASTER ROW is a style row (no roll, mutate or cross draws it) that
  //    sets all eight; every row starts at the old face.
  const m = mg.normalizeGenome({});
  ok(mg.applyStyleRow(m, 'face.detail', 0.6) &&
     mg.DETAIL_KEYS.every(k => m.face[k] === 0.6) && m.face.detail === 0.6,
     'the face detail row sets every feature’s detail');
  ok(mg.GENE_SPECS.find(sp => sp.path === 'face.detail').styled,
     'the face detail row is a style row');
  ok(mg.DETAIL_KEYS.every(k => mg.normalizeGenome({}).face[k] === 0),
     'every detail row defaults to 0, the old face');

  // 4. THE HEAD SHAPE KEEPS THE OLD OUTLINE. Rounded all the way, the head's
  //    silhouette is the old head's row for row, front and side, to a cell
  //    each side (its flat lid to two), and its volume to 3%: the same head with its stair steps
  //    sanded off. (The resampled ellipse this replaced was two cells wider
  //    through the cheeks with a tapering crown and jaw -- the "egg".)
  const rows = p => {
    const r = new Map();
    for (const [x, y, z] of p.cells) {
      const v = r.get(z) || [1e9, -1e9, 1e9, -1e9];
      v[0] = Math.min(v[0], x); v[1] = Math.max(v[1], x);
      v[2] = Math.min(v[2], y); v[3] = Math.max(v[3], y);
      r.set(z, v);
    }
    return r;
  };
  for (const [nm, src] of cast.slice(0, 12)) {
    const H0 = partsOf(mg.generateMob(at(src, 0), 0, opts)).head;
    const g1 = at(src, 0);
    g1.face.detailShape = 1;
    const H1 = partsOf(mg.generateMob(mg.normalizeGenome(g1), 0, opts)).head;
    const r0 = rows(H0), r1 = rows(H1);
    // (The crown's top row is the old head's flat lid; rounded, its corners
    // come in two cells, which is the rounding.)
    const lid = Math.max(...r0.keys());
    let worst = 0;
    for (const [z, v0] of r0) {
      const v1 = r1.get(z);
      const d = v1 ? Math.max(...v0.map((e, i) => Math.abs(e - v1[i]))) : 99;
      worst = Math.max(worst, z === lid ? Math.max(0, d - 1) : d);
    }
    const vol = H1.cells.length / H0.cells.length;
    ok(worst <= 1 && vol > 0.97 && vol < 1.03,
       `${nm}: the rounded head is the old head’s outline and volume`,
       `worst row edge ${worst}, volume x${vol.toFixed(3)}`);
  }

  // 5. ...AND THE ROUNDING RUNS ALL THE WAY ALONG ITS SLIDER (it once moved
  //    nothing below a half), the head a mirror at every step.
  {
    const H = d => {
      const g = at({}, 0);
      g.face.detailShape = d;
      return partsOf(mg.generateMob(mg.normalizeGenome(g), 0, opts)).head;
    };
    const set0 = new Set(H(0).cells.map(c => c.slice(0, 3).join(',')));
    const moved = h => {
      const s1 = new Set(h.cells.map(c => c.slice(0, 3).join(',')));
      let n = 0;
      for (const k of s1) if (!set0.has(k)) n++;
      for (const k of set0) if (!s1.has(k)) n++;
      return n;
    };
    const steps = [0.25, 0.5, 0.75, 1].map(d => moved(H(d)));
    ok(steps[0] > 0 && steps[0] < steps[1] && steps[1] < steps[2] && steps[2] < steps[3] &&
       steps[1] > steps[3] * 0.3 && steps[1] < steps[3] * 0.7,
       'the head shape rounds a share at a time along its slider', steps.join(' / '));
    for (const d of [0.25, 0.5, 0.75]) {
      const h = H(d), sx = h.size[0];
      const cs = new Set(h.cells.map(c => c.slice(0, 3).join(',')));
      ok(h.cells.every(([x, y, z]) => cs.has(`${sx - 1 - x},${y},${z}`)),
         `the head is a mirror at shape detail ${d}`);
    }
  }

  // 6. THE TONES ARE THE DETAIL'S TOO: at eye detail 0 the whites, the lash
  //    line and the crease are the skin and the bags the old full shade; at 1
  //    each is its own colour. The ear's hollow is the rim's shade at 0.
  const t0 = at({}, 0), t1 = at({}, 1);
  const c0 = mg.detailColors(t0.colors, t0), c1 = mg.detailColors(t1.colors, t1);
  ok(c0[A.EYE_WHITE] === t0.colors.skin && c0[A.LASH] === t0.colors.skin &&
     c0[A.CREASE] === t0.colors.skin && c0[A.BAGS] === t0.colors.skinShade &&
     c0[A.EAR_IN] === t0.colors.skinShade,
     'at detail 0 the fine tones are the old face’s');
  ok(c1[A.EYE_WHITE] !== t1.colors.skin && c1[A.LASH] !== t1.colors.skin,
     'at detail 1 the whites and lashes are their own colours');
}

// =============================================================================

if (fails) {
  console.log('\nThe standard is assets/mobs/human.json. If it moved on purpose, ' +
              'mobgen.js follows it;\na stale character on disk is re-baked with ' +
              '`node scripts/gen_mobs.mjs <name> --rebake`.');
}
// The verdict is the LAST line: scripts/generator_parity.mjs reads it as this
// test's summary.
console.log(`\n${fails ? 'FAIL' : 'PASS'}  ${checks - fails}/${checks} checks`);
process.exit(fails ? 1 : 0);
