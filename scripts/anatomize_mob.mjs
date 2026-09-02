/* anatomize_mob.mjs — bake a mob's `anatomy` recipe into its .vox.
 *
 *   node scripts/anatomize_mob.mjs human            # apply human.json's recipe
 *   node scripts/anatomize_mob.mjs human --dry      # plan + report, write nothing
 *   node scripts/anatomize_mob.mjs human --depth    # also print a depth census
 *
 * The recipe is `assets/mobs/<mob>.json` -> "anatomy" (see
 * assets/editor/anatomy.js for the format); a mob without one is refused
 * rather than given the human default, because a bake is a commit-sized edit
 * to authored art and the tuner's "Apply recipe" button is the place to try
 * a recipe out first. The .vox is rewritten through the SAME writer the
 * editor's Save uses (tightenPrefab -> prefabToVoxModels -> writeVox), with
 * the file's own palette (its art colours live there and must survive).
 *
 * Only MATERIALS change. The script asserts the tight rebase is a no-op, so
 * the sidecar's anchors stay valid and nothing here can move a limb.
 */
import { readFileSync, writeFileSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const VOX = await load('assets/editor/vox.js');
const ANA = await load('assets/editor/anatomy.js');

const args = process.argv.slice(2);
const flags = new Set(args.filter(a => a.startsWith('--')));
const name = args.find(a => !a.startsWith('--'));
if (!name) {
  console.error('usage: node scripts/anatomize_mob.mjs <mob> [--dry] [--depth]');
  process.exit(2);
}

const voxPath = join(ROOT, 'assets/mobs', name + '.vox');
const jsonPath = join(ROOT, 'assets/mobs', name + '.json');
const side = JSON.parse(readFileSync(jsonPath, 'utf8'));
if (!side.anatomy) {
  console.error(`${name}.json has no "anatomy" block — nothing to bake. ` +
    'Author one (assets/editor/anatomy.js documents the format) or use the ' +
    "tuner's Apply recipe button, which falls back to the stock human recipe.");
  process.exit(1);
}

const materials = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const matId = ANA.materialIds(materials);
const matName = id => (materials[id - 1] && materials[id - 1].id) || ('#' + id);

const buf = readFileSync(voxPath);
const parsed = VOX.readVox(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
if (!parsed.prefab) { console.error('no models in ' + voxPath); process.exit(1); }
for (const w of parsed.warnings) console.log('  warn: ' + w);
const prefab = parsed.prefab;

const plan = ANA.planAnatomy(prefab, side.anatomy, matId);
if (plan.report.unresolved.length) {
  console.error('recipe names materials that do not exist: ' +
    plan.report.unresolved.join(', '));
  process.exit(1);
}

if (flags.has('--depth')) {
  const census = {};
  for (const d of plan.field.depth)
    if (d !== ANA.DEPTH_EMPTY) census[d] = (census[d] || 0) + 1;
  console.log('depth census (union of all limbs):',
    Object.entries(census).map(([d, n]) => `${d}:${n}`).join(' '));
}

console.log(`${name}: ${prefab.models.length} models, ${plan.report.total} voxels to rewrite`);
for (const [limb, hist] of Object.entries(plan.report.perLimb))
  console.log('  ' + limb.padEnd(8) +
    Object.entries(hist).map(([m, n]) => `${m} ${n}`).join(', '));

if (flags.has('--dry')) { console.log('(dry run — nothing written)'); process.exit(0); }
if (!plan.report.total) { console.log('already baked — nothing to write'); process.exit(0); }

ANA.applyPlan(prefab, plan);

// Post-bake census, the number a reviewer wants: what the whole body is now.
const after = {};
for (const m of prefab.models)
  for (const v of m.grid.data) if (v) after[matName(v)] = (after[matName(v)] || 0) + 1;
console.log('body now: ' + Object.entries(after).sort((a, b) => b[1] - a[1])
  .map(([m, n]) => `${m} ${n}`).join(', '));

const { prefab: tight, shift } = VOX.tightenPrefab(prefab);
if (shift.x || shift.y || shift.z || tight.models.length !== prefab.models.length) {
  console.error(`refusing to write: the tight rebase moved content by ` +
    `${shift.x},${shift.y},${shift.z} (or dropped a model) — the input was not ` +
    'in the engine\'s canonical form and the sidecar anchors would drift');
  process.exit(1);
}
const rt = VOX.prefabRoundTripTest(tight, parsed.palette);
if (!rt.ok) { console.error('round-trip failed: ' + rt.error); process.exit(1); }
const bytes = VOX.writeVox(VOX.prefabToVoxModels(tight), parsed.palette, { scene: true });
writeFileSync(voxPath, bytes);
console.log(`wrote ${voxPath} (${bytes.length} bytes)`);
