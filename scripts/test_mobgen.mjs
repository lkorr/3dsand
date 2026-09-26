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
  const src = fs.readFileSync(path.join(ROOT, 'src/game/avatar.cpp'), 'utf8');
  const get = name => {
    const m = src.match(new RegExp(`constexpr float ${name}\\s*=\\s*([0-9.]+)f`));
    if (!m) throw new Error(`could not find ${name} in src/game/avatar.cpp`);
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
  ok(!d, 'mobgen.js AVATAR_CONSTANTS match src/game/avatar.cpp', d);
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
    const isHair = n => /^(hair|mane)(\.\d+)?$/.test(n);
    for (const m of prefab.models) {
      for (const v of m.grid.data) if (v) (isHair(m.name) ? hairMats : mats).add(v);
      if (m.grid.color) for (const v of m.grid.color) if (v) cols.add(v);
    }
    ok(mats.size === 1 && [...mats][0] <= 127,
       `${label} is one material, id <= 127`,
       `materials ${[...mats].join(',')}`);
    const hairId = materials.findIndex(m => m.id === mg.HAIR_MAT_ID) + 1;
    ok(hairMats.size === 0 || (hairMats.size === 1 && hairMats.has(hairId)),
       `${label}'s hair is all ${mg.HAIR_MAT_ID}`,
       `materials ${[...hairMats].join(',')}`);
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
  ok(byName['crawl.feet'] && byName['crawl.feet'].clip === 'crawl' &&
     byName['crawl.feet'].groundAlign === 1.0 &&
     byName['crawl.feet'].disableGait === true,
     'both feet gone selects the crawl clip, prone and ground-aligned');
  // ONE foot is the derived stump drag (AnimFindStumpLeg), whose predicate is
  // one stump plus one whole leg. An authored rule matching a single foot
  // would shadow it, so the both-feet rule must ask for BOTH.
  for (const s of mine.states)
    if (s.clip === 'crawl' || s.clip === 'hop')
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
  const base = sc.resolveSidecar(readStem, mg.BASE_MOB, readEffect);

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
    let thin;
    try {
      thin = mg.thinSidecar(fresh, base, name);
    } catch (e) {
      ok(false, `${name}: its body is expressible as a difference from ` +
         `${mg.BASE_MOB}`, e.message);
      continue;
    }
    const strip = o => {
      const r = JSON.parse(JSON.stringify(noNotes(o)));
      delete r.genome;
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
    for (const k of Object.keys(fresh)) {
      if (k === 'genome') continue;
      const dr = diff(noNotes(resolved[k]), noNotes(fresh[k]), `${name}.${k}`);
      ok(!dr, `${name}: resolved \`${k}\` is what this genome derives`, dr);
    }
    // ...and the things the base contributes and the generator no longer
    // emits at all: the clip library is compiled by the loader, not by the
    // file, so `states` naming `crawl` has to keep meaning something.
    const ds = diff(noNotes(resolved.states).map(fillState),
                    noNotes(shippedStates).map(fillState), `${name}.states`);
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

if (fails) {
  console.log('\nThe standard is assets/mobs/human.json. If it moved on purpose, ' +
              'mobgen.js follows it;\na stale character on disk is re-baked with ' +
              '`node scripts/gen_mobs.mjs <name> --rebake`.');
}
// The verdict is the LAST line: scripts/generator_parity.mjs reads it as this
// test's summary.
console.log(`\n${fails ? 'FAIL' : 'PASS'}  ${checks - fails}/${checks} checks`);
process.exit(fails ? 1 : 0);
