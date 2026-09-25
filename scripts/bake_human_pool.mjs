#!/usr/bin/env node
/* bake_human_pool.mjs — the engine's RANDOM HUMAN pool.
 *
 *   node scripts/bake_human_pool.mjs            # (re)bake assets/mobs/pool/
 *   node scripts/bake_human_pool.mjs --count 24 # a bigger pool
 *   node scripts/bake_human_pool.mjs --dry      # build + check, write nothing
 *
 * WHY A POOL AND NOT A GENERATOR IN THE ENGINE. The character generator is
 * assets/editor/mobgen.js: 3,500 lines of JavaScript that the Characters page
 * and gen_mobs.mjs share. The F1 Spawn tab's "random human" needs a body the
 * engine can load, and porting the generator to C++ would be a second copy of
 * it that drifts. So the variety is baked here, once, and the engine picks one
 * at spawn time and rolls everything that IS a runtime choice on top of it
 * (weapon, outfit, dye, behaviour).
 *
 * WHY A SUBDIRECTORY. LoadMobDefs scans assets/mobs non-recursively, so the
 * pool costs nothing at startup and never appears in the creature combo. A
 * pool body becomes a def the first time something asks for `pool/<stem>` by
 * name (MobSystem::PoolDef, called from FindOrComposeDef), which is also how a
 * save, a network peer or a hot reload gets it back.
 *
 * EXACT STOCK COLOURS (rollColors jitter 0): every body's art is merged into
 * one 255-entry palette in the engine, and jittered colours would spend about
 * a dozen fresh slots per body. Stock sets are shared between bodies.
 *
 * Half the pool is each sex, alternating; each body is the first seed at or
 * after its slot's whose random roll came out that sex, so the pool is a pure
 * function of --count and --seed.
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync,
         unlinkSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const mg = await load('assets/editor/mobgen.js');
const sc = await load('assets/editor/sidecar.js');

const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d;
};
const COUNT = Math.max(2, parseInt(opt('count', '20'), 10) | 0);
const SEED = parseInt(opt('seed', '7100'), 10) | 0;
const dry = argv.includes('--dry');

const MOBS = join(ROOT, 'assets/mobs');
const POOL = join(MOBS, 'pool');
const readJson = p => JSON.parse(readFileSync(p, 'utf8'));
const materials = readJson(join(ROOT, 'assets/materials/materials.json')).materials;
const tuning = readJson(join(ROOT, 'assets/materials/tuning.json'));

// Same gait constants gen_mobs.mjs reads out of avatar.cpp.
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
const GEN = { materials, player: tuning.player, avatar: avatarConstants() };

// The base every pool body is a diff against: assets/mobs/human.json, resolved
// from the MAIN mob directory (the pool's `extends` resolves there too).
const base = sc.resolveSidecar(stem => readJson(join(MOBS, stem + '.json')),
  mg.BASE_MOB, name => {
    const p = join(MOBS, 'effects', name + '.json');
    return existsSync(p) ? readJson(p) : null;
  });

if (!dry) mkdirSync(POOL, { recursive: true });
const written = new Set();
const colours = new Set();
let seed = SEED;
for (let i = 0; i < COUNT; i++) {
  const want = i % 2 === 0 ? 'female' : 'male';
  let g = null;
  for (let tries = 0; tries < 64; tries++, seed++) {
    const r = mg.makeRng(seed);
    const cand = mg.rollColors(mg.randomGenome(r), mg.makeRng(seed ^ 0x5bf03635),
                               { jitter: 0 });
    if (cand.body.sex !== want) continue;
    g = cand;
    seed++;
    break;
  }
  if (!g) throw new Error(`no ${want} roll within 64 seeds of ${seed}`);
  const stem = `${want === 'female' ? 'f' : 'm'}${String((i >> 1) + 1).padStart(2, '0')}`;
  const name = 'pool_' + stem;
  g.name = name;
  const built = mg.generateMob(g, 0, { ...GEN, name });
  const complaints = mg.validateMob(built);
  if (complaints.length) {
    // A pool body is spawned blind from a panel: one that fails the structural
    // asserts is skipped, not shipped. The next seed replaces it.
    console.error(`${stem}: REFUSED (${complaints[0]}), rolling again`);
    i--;
    continue;
  }
  mg.bakeAnatomy(built, materials);
  for (const c of Object.values(g.colors)) colours.add(c);
  for (const c of Object.values(mg.wispColors(g.colors))) colours.add(c);
  const doc = mg.thinSidecar(built.sidecar, base, name);
  console.log(`${stem}: ${g.body.sex}, ${g.body.heightM.toFixed(2)} m, ` +
              `${g.hair.style}${g.face.beard > 0 ? ', beard' : ''}, ` +
              `${built.vox.length} B art`);
  written.add(stem);
  if (dry) continue;
  writeFileSync(join(POOL, stem + '.vox'), built.vox);
  writeFileSync(join(POOL, stem + '.json'), JSON.stringify(doc, null, 2) + '\n');
}
// A smaller re-bake must not leave the old tail behind for the engine to pick.
if (!dry)
  for (const f of readdirSync(POOL)) {
    const stem = f.replace(/\.(vox|json)$/, '');
    if (!written.has(stem)) { unlinkSync(join(POOL, f)); console.log(`removed ${f}`); }
  }
console.log(`${written.size} bodies, ${colours.size} distinct colours ` +
            '(the engine merges art into one 255-entry palette)' +
            (dry ? ' -- --dry: wrote nothing' : ` -> ${POOL}`));
