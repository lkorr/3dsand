#!/usr/bin/env node
/* gen_mobs.mjs — author-side bake: a GENOME -> assets/mobs/<name>.{vox,json}
 *
 *   node scripts/gen_mobs.mjs peasant --preset stocky
 *   node scripts/gen_mobs.mjs peasant --genome my.json     # a saved genome file
 *   node scripts/gen_mobs.mjs peasant --rebake             # from the sidecar's
 *                                                          # own `genome` block
 *   node scripts/gen_mobs.mjs peasant --random --seed 12
 *   node scripts/gen_mobs.mjs --litter 6 --parents guard,peasant --seed 3
 *   node scripts/gen_mobs.mjs pale_guard --variant-of guard --saturation 0.5 \
 *                                        --tint '#b9c3ae' --tint-amount 0.2
 *   node scripts/gen_mobs.mjs peasant --preset waif --dry   # report, write nothing
 *   node scripts/gen_mobs.mjs --list                        # what is in the pool
 *
 * THIS IS THE HEADLESS HALF of the Characters page, and it calls the SAME
 * `generateMob` the browser does (assets/editor/mobgen.js) — the arrangement
 * bake_trees.mjs already uses, for the same reason: a build machine has no
 * browser, and two implementations of "what a character looks like" is exactly
 * the drift this shape exists to prevent.
 *
 * TWO WAYS TO SAVE A CHARACTER, and the choice is not cosmetic:
 *
 *   1. A NEW BODY (--preset / --genome / --random / cross) writes a .vox and a
 *      sidecar. It is 200 KB of art and it stops tracking the human the moment
 *      it lands, INCLUDING the anatomy baked into its interior.
 *   2. A COLOUR VARIANT (--variant-of) writes a ~20-line sidecar with
 *      `extends` + a `palette` filter and NO .vox at all — assets/mobs/
 *      zombie.json is the shipped example. It keeps tracking its base, costs
 *      nothing, and cannot diverge in what is under its skin.
 *
 * Use (2) whenever the shape is unchanged. The catch, stated because it decides
 * the UI too: `palette` is a GLOBAL filter (saturation / brightness / tint), so
 * it cannot say "red hair, unchanged skin". PER-FEATURE colour lives in the
 * `.col` model inside the .vox, so a per-feature recolour is a new body — there
 * is no third option, because a def cannot override only the colour models of
 * someone else's file.
 *
 * ANATOMY IS BAKED BEFORE THE FILE IS WRITTEN, in this process, through
 * `mobgen.bakeAnatomy` — the same call the tuner's Characters page makes on
 * save. A generated body is solid `skin` at every depth until then, which is
 * not a finished character. `--no-anatomy` writes the raw body instead, for
 * looking at the generator's own output; `scripts/anatomize_mob.mjs` is still
 * the way to re-bake a mob that is already on disk.
 *
 * Writing a NEW mob does not move the world hash — nothing spawns it until a
 * spawner names it. Re-baking one the game already spawns does, and that is a
 * one-command rebaseline (`--selftest --rebaseline`), not an investigation.
 */
import { readFileSync, writeFileSync, existsSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const mg = await load('assets/editor/mobgen.js');
const sc = await load('assets/editor/sidecar.js');

const MOBS = join(ROOT, 'assets/mobs');
const readJson = p => JSON.parse(readFileSync(p, 'utf8'));
// Declared up here rather than beside resolvedBase(): the writes below run at
// module top level, before the helper section, and a `let` down there is in
// its temporal dead zone by the time the first character is written.
let baseCache = null;

// ---- arguments --------------------------------------------------------------
const argv = process.argv.slice(2);
// Options that take a VALUE, so the value is not mistaken for the name
// positional. Declared before `positional` reads it.
const OPTS_WITH_VALUES = new Set(['preset', 'genome', 'seed', 'out', 'litter',
                                  'parents', 'variant-of', 'saturation',
                                  'brightness', 'tint', 'tint-amount', 'sigma',
                                  'behavior']);
const flag = n => argv.includes('--' + n);
const opt = (n, d = null) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d;
};
const positional = argv.filter((a, i) =>
  !a.startsWith('--') && !(i > 0 && argv[i - 1].startsWith('--') &&
                           OPTS_WITH_VALUES.has(argv[i - 1].slice(2))));

const seed = parseInt(opt('seed', '0'), 10) | 0;
const outDir = opt('out', MOBS);
const dry = flag('dry');

// ---- the contracts this generator reads, out of the files that declare them -
const materials = readJson(join(ROOT, 'assets/materials/materials.json')).materials;
const tuning = readJson(join(ROOT, 'assets/materials/tuning.json'));

/** The four gait constants avatar.cpp owns. mobgen.js carries fallbacks so the
 *  browser can run without the C++ in hand; the bake reads the real thing, and
 *  scripts/test_mobgen.mjs fails if the two ever disagree. */
function avatarConstants() {
  const src = readFileSync(join(ROOT, 'src/game/avatar.cpp'), 'utf8');
  const get = name => {
    const m = src.match(new RegExp(`constexpr float ${name}\\s*=\\s*([0-9.]+)f`));
    if (!m) throw new Error(`could not find ${name} in src/game/avatar.cpp`);
    return parseFloat(m[1]);
  };
  return { swingTravelFrac: get('kSwingTravelFrac'),
           maxLeadLegLengths: get('kMaxLeadLegLengths'),
           minSwingScale: get('kMinSwingScale'),
           minSwingSeconds: get('kMinSwingSeconds') };
}
const GEN_OPTS = { materials, player: tuning.player, avatar: avatarConstants() };

// ---- --list ----------------------------------------------------------------
if (flag('list')) {
  const rows = [];
  for (const f of readdirSync(MOBS).filter(n => n.endsWith('.json')).sort()) {
    const stem = f.slice(0, -5);
    let j;
    try { j = readJson(join(MOBS, f)); } catch { continue; }
    if (!j.limbs && !j.extends) continue;          // behaviors.json et al
    rows.push({
      name: stem,
      kind: j.extends ? `variant of ${j.extends}` :
            existsSync(join(MOBS, stem + '.vox')) ? 'body' : 'sidecar only',
      genome: j.genome ? 'yes' : '-',
      palette: j.palette ? 'filter' : '-',
    });
  }
  const w = k => Math.max(k.length, ...rows.map(r => String(r[k]).length));
  const cols = ['name', 'kind', 'genome', 'palette'];
  const line = r => cols.map(c => String(r[c]).padEnd(w(c))).join('  ');
  console.log(line({ name: 'name', kind: 'kind', genome: 'genome',
                     palette: 'palette' }));
  for (const r of rows) console.log(line(r));
  console.log(`\n${rows.length} defs. "genome: yes" can be re-baked with ` +
              '--rebake and bred from on the Characters page.');
  process.exit(0);
}

// ---- the colour-variant path (no .vox) -------------------------------------
if (opt('variant-of')) {
  const base = opt('variant-of');
  const name = positional[0];
  if (!name) die('a variant needs a name: gen_mobs.mjs <name> --variant-of <base>');
  if (!existsSync(join(MOBS, base + '.json')))
    die(`no such base def: assets/mobs/${base}.json`);
  const pal = {};
  if (opt('saturation')) pal.saturation = Number(opt('saturation'));
  if (opt('brightness')) pal.brightness = Number(opt('brightness'));
  if (opt('tint')) {
    pal.tint = opt('tint');
    pal.tintAmount = Number(opt('tint-amount', '0.2'));
  }
  if (!Object.keys(pal).length)
    die('a variant with no palette filter is just a rename — pass at least one ' +
        'of --saturation / --brightness / --tint');
  const doc = {
    '//': `A COLOUR VARIANT OF ${base}. No .vox: \`extends\` pulls in ` +
          `${base}.json's whole rig (limbs, chains, clips, states, sockets, ` +
          'anatomy) and `model` follows it to the same art, so this creature ' +
          'keeps tracking its base as the base is edited — including what is ' +
          'under its skin, which is BAKED into the .vox and would silently ' +
          'diverge in a copy. Written by scripts/gen_mobs.mjs --variant-of.',
    extends: base,
    '//palette': 'A FILTER over the base art palette, not a colour table. ' +
                 'GLOBAL by construction: it cannot say "red hair, unchanged ' +
                 'skin" — a per-feature recolour is a new body, because the ' +
                 'colours live in the `.col` models inside the .vox and a def ' +
                 'cannot override only those.',
    palette: pal,
  };
  if (opt('behavior')) doc.behavior = opt('behavior');
  const p = join(outDir, name + '.json');
  console.log(`${name}: variant of ${base}, ` +
              Object.entries(pal).map(([k, v]) => `${k} ${v}`).join(', '));
  if (dry) { console.log('(--dry: wrote nothing)'); process.exit(0); }
  writeFileSync(p, JSON.stringify(doc, null, 2) + '\n');
  console.log(`wrote ${rel(p)} (${JSON.stringify(doc).length} bytes, no .vox)`);
  process.exit(0);
}

// ---- a litter --------------------------------------------------------------
if (opt('litter')) {
  const n = parseInt(opt('litter'), 10);
  const parents = (opt('parents') || '').split(',').filter(Boolean)
    .map(loadGenomeByName);
  const sigma = Number(opt('sigma', '0.5'));
  if (!parents.length) die('--litter needs --parents <mob>[,<mob>...]');
  const stem = positional[0] || (opt('parents').split(',')[0] + '_kid');
  for (let i = 0; i < n; i++) {
    const rng = mg.makeRng(seed * 1000 + i);
    const g = parents.length === 1 ? mg.mutate(parents[0], sigma, rng)
                                   : mg.cross(parents, rng, { sigma });
    write(`${stem}${i + 1}`, g);
  }
  process.exit(0);
}

// ---- one character ---------------------------------------------------------
{
  const name = positional[0];
  if (!name) {
    console.error(readFileSync(fileURLToPath(import.meta.url), 'utf8')
      .split('\n').slice(1, 30).map(l => l.replace(/^ ?\*? ?/, '')).join('\n'));
    process.exit(2);
  }
  let genome;
  if (flag('rebake')) genome = loadGenomeByName(name);
  else if (opt('genome')) genome = mg.normalizeGenome(readJson(opt('genome')));
  else if (opt('preset')) genome = mg.presetGenome(opt('preset'));
  else if (flag('random')) genome = mg.rollColors(mg.randomGenome(mg.makeRng(seed)),
                                                  mg.makeRng(seed ^ 0x5bf03635));
  else genome = mg.defaultGenome();
  if (flag('roll-colors')) genome = mg.rollColors(genome, mg.makeRng(seed));
  write(name, genome);
}

// ---- helpers ---------------------------------------------------------------
function die(msg) { console.error(msg); process.exit(2); }
function rel(p) { return p.replace(ROOT + '\\', '').replace(ROOT + '/', ''); }

/** The genome a saved character was built from. Every body this script writes
 *  carries its own genome in its sidecar, which is what makes "load it back
 *  into the sliders and breed from it" possible at all — the alternative is
 *  reverse-engineering a genome out of voxels, which is not possible. */
function loadGenomeByName(name) {
  const p = join(MOBS, name + '.json');
  if (!existsSync(p)) die(`no such def: ${rel(p)}`);
  const j = readJson(p);
  if (!j.genome)
    die(`${name}.json has no "genome" block, so there is nothing to re-bake ` +
        'or breed from. It was hand-authored or predates the generator; start ' +
        'from a --preset instead.');
  return mg.normalizeGenome(j.genome);
}

/** The base every character extends, resolved — read from the SAME directory
 *  the character is written to, so a `--out` sandbox diffs against the base
 *  that will actually sit beside it. Cached: a litter of thirty asks thirty
 *  times and the answer cannot change mid-run. */
function resolvedBase() {
  if (baseCache) return baseCache;
  if (!existsSync(join(outDir, mg.BASE_MOB + '.json')))
    die(`no ${mg.BASE_MOB}.json in ${rel(outDir)} — a character is a diff ` +
        'against its base, so the base has to be there to diff against');
  const read = stem => readJson(join(outDir, stem + '.json'));
  const readEffect = name => {
    const p = join(outDir, 'effects', name + '.json');
    return existsSync(p) ? readJson(p) : null;
  };
  baseCache = sc.resolveSidecar(read, mg.BASE_MOB, readEffect);
  return baseCache;
}

function write(name, genome) {
  const built = mg.generateMob(genome, seed, { ...GEN_OPTS, name });
  const complaints = mg.validateMob(built);
  const s = built.stats;
  console.log(`${name}: ${s.worldH} world voxels, ${s.parts} parts, ` +
              `${s.painted} painted micro voxels, ` +
              `rideHeight ${built.sidecar.gait.rideHeight}, ` +
              `walk ${built.sidecar.clips.walk.durationMs} ms`);
  if (complaints.length) {
    // A refusal, not a warning. These are the failures a preview cannot show
    // (an anchor outside its limb, an eye row off the face), and a body that
    // fails them looks fine standing still and comes apart when it walks.
    console.error(`  REFUSED: ${complaints.length} structural problem(s):`);
    for (const c of complaints.slice(0, 6)) console.error(`    ${c}`);
    process.exit(1);
  }
  // Before the write, not after: a half-finished asset should never exist on
  // disk, and the bake can REFUSE (a recipe naming a material that does not
  // exist, a tight rebase that would drift the anchors).
  if (!flag('no-anatomy')) {
    let baked;
    try {
      baked = mg.bakeAnatomy(built, materials);
    } catch (e) {
      console.error(`  anatomy bake REFUSED: ${e.message}`);
      process.exit(1);
    }
    console.log('  anatomy: ' + Object.entries(baked.census)
      .sort((a, b) => b[1] - a[1]).map(([m, n]) => `${m} ${n}`).join(', '));
  }
  if (dry) { console.log('  (--dry: wrote nothing)'); return; }
  const vp = join(outDir, name + '.vox'), jp = join(outDir, name + '.json');
  // THE FILE IS THE DIFF. `built.sidecar` is the whole creature; what is
  // written is only what this body makes different from the base it extends,
  // so the rig keeps tracking the human instead of freezing a copy of it (see
  // mobgen.thinSidecar). The .vox is still the whole body: art is art.
  const doc = mg.thinSidecar(built.sidecar, resolvedBase(), name);
  writeFileSync(vp, built.vox);
  writeFileSync(jp, JSON.stringify(doc, null, 2) + '\n');
  console.log(`  wrote ${rel(vp)} (${built.vox.length} bytes) and ${rel(jp)} ` +
              `(${JSON.stringify(doc).length} B of difference against ` +
              `${JSON.stringify(built.sidecar).length} B of resolved rig)`);
  if (flag('no-anatomy'))
    console.log('  --no-anatomy: the body is solid skin at every depth. Run ' +
                `\`node scripts/anatomize_mob.mjs ${name}\` before shipping it.`);
}
