/* Data gate for assets/editor/anatomy.js — the depth field, the layer
 * schedule, garments, speckle, idempotence — on a synthetic prefab, and then
 * on the real human: every limb must have a bone core.
 *
 * Node-only; anatomy.js and vox.js are pure.
 *   node scripts/test_anatomy.mjs
 */
import { readFileSync, readdirSync, existsSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const VOX = await load('assets/editor/vox.js');
const ANA = await load('assets/editor/anatomy.js');
const SC = await load('assets/editor/sidecar.js');

let fails = 0;
const ok = (cond, what) => {
  if (cond) { console.log('  ok   ' + what); return true; }
  console.log('  FAIL ' + what); fails++; return false;
};

const MAT = { skin: 51, linen: 111, flesh: 117, muscle: 118, bone: 56, blood: 32, brain: 122 };

// A 9x9x9 solid block split into two models along y: the lower 4 rows and
// the upper 5. The seam faces are each model's OWN faces, so they read as
// surface; the rows under them keep the union's depth.
function block() {
  const mk = (name, oy, h) => {
    const g = VOX.makeGrid({ x: 9, y: h, z: 9 });
    g.data.fill(MAT.skin);
    return { name, offset: { x: 0, y: oy, z: 0 }, dim: { x: 9, y: h, z: 9 }, grid: g };
  };
  return { size: { x: 9, y: 9, z: 9 }, models: [mk('low', 0, 4), mk('high', 4, 5)] };
}

/* ---- 1. depth field ------------------------------------------------------ */
console.log('depth field');
{
  const p = block();
  const f = ANA.unionDepth(p);
  ok(ANA.depthAt(f, p.models[0], 0, 0, 0) === 0, 'corner is depth 0');
  ok(ANA.depthAt(f, p.models[0], 4, 3, 4) === 0, 'seam row centre is depth 0 (a model face is surface)');
  ok(ANA.depthAt(f, p.models[1], 4, 0, 4) === 0, 'the other side of the seam is surface too');
  ok(ANA.depthAt(f, p.models[0], 4, 2, 4) === 2, 'one under the seam keeps the union depth (2, not 1)');
  ok(ANA.depthAt(f, p.models[1], 4, 0, 4) === 0, 'the block centre sits on the seam: depth 0');
  ok(ANA.depthAt(f, p.models[1], 4, 1, 4) === 3, 'one above centre is depth 3');
  ok(ANA.maxDepth(f) === 3, 'max depth 3 (the only depth-4 cell is the seam face)');
  ok(ANA.depthAt(f, p.models[0], -1, 0, 0) === ANA.DEPTH_EMPTY, 'outside reads empty');
  const { cells } = ANA.layerCells(f, p.models[0], 0);
  ok(cells.length === 9 * 9 * 4 - 7 * 7 * 2, `low model has ${cells.length} surface cells (both faces + 4 rings)`);
}

/* ---- 2. layer schedule --------------------------------------------------- */
console.log('layers');
{
  const layers = ANA.layersFor(ANA.DEFAULT_ANATOMY, 'legU.L');
  ok(layers.length === 4 && layers[3].depth === null, 'default has 4 layers, open-ended core');
  ok(ANA.layerIndexAt(layers, 0) === 0, 'depth 0 -> skin');
  ok(ANA.layerIndexAt(layers, 1) === 1, 'depth 1 -> flesh');
  ok(ANA.layerIndexAt(layers, 2) === 2, 'depth 2 -> muscle');
  ok(ANA.layerIndexAt(layers, 3) === 3 && ANA.layerIndexAt(layers, 40) === 3, 'depth 3+ -> bone');
  const head = ANA.layersFor(ANA.DEFAULT_ANATOMY, 'head');
  ok(ANA.layerIndexAt(head, 2) === 2 && ANA.layerIndexAt(head, 3) === 2 &&
     head[2].material === 'bone', 'head override: skull at depth 2..3');
  ok(ANA.layerIndexAt(head, 4) === 3 && head[3].material === 'brain', 'brain inside the skull');
}

/* ---- 3. plan on the synthetic block -------------------------------------- */
console.log('plan');
{
  const p = block();
  // Paint every voxel so the art-clearing rule is exercised.
  for (const m of p.models) VOX.gridColorLayer(m.grid).fill(200);
  // A garment patch on the top face.
  for (let x = 2; x < 7; x++) for (let z = 2; z < 7; z++) VOX.gridSet(p.models[1].grid, x, 4, z, MAT.linen);
  const recipe = {
    layers: [
      { material: 'skin', depth: 1, keep: true },
      { material: 'flesh', depth: 1 },
      { material: 'muscle', depth: 1, speckle: { material: 'blood', fraction: 0.25 } },
      { material: 'bone' },
    ],
    garments: ['linen'],
  };
  const plan = ANA.planAnatomy(p, recipe, MAT);
  ok(plan.report.unresolved.length === 0, 'every material resolved');
  const surface = 9 * 9 * 9 - 7 * 7 * 7;
  ok(plan.report.total === 9 * 9 * 9 - surface - 2 * 7 * 7, `rewrites every interior voxel, seam faces excepted (${plan.report.total})`);
  ANA.applyPlan(p, plan);
  const at = (mi, x, y, z) => VOX.gridGet(p.models[mi].grid, x, y, z);
  const art = (mi, x, y, z) => VOX.gridColorGet(p.models[mi].grid, x, y, z);
  ok(at(0, 0, 0, 0) === MAT.skin && art(0, 0, 0, 0) === 200, 'surface kept, paint kept');
  ok(at(0, 1, 1, 1) === MAT.flesh && art(0, 1, 1, 1) === 0, 'depth 1 is flesh, paint cleared');
  ok(at(1, 4, 1, 4) === MAT.bone, 'one above the seam is bone (union depth 3)');
  ok(at(0, 4, 3, 4) === MAT.skin && art(0, 4, 3, 4) === 200, 'the model seam is skin with its paint, not bone');
  ok(at(0, 4, 2, 4) === MAT.muscle || at(0, 4, 2, 4) === MAT.blood, 'one under the seam is the muscle layer (union depth 2)');
  ok(at(1, 4, 3, 4) === MAT.skin, 'under the garment patch: skin, not flesh');
  ok(at(1, 4, 2, 4) === MAT.muscle || at(1, 4, 2, 4) === MAT.blood, 'two under the garment: muscle (schedule unchanged)');
  let blood = 0, muscleLayer = 0;
  for (const m of p.models) for (const v of m.grid.data) {
    if (v === MAT.blood) blood++;
    if (v === MAT.blood || v === MAT.muscle) muscleLayer++;
  }
  ok(blood > 0 && blood < muscleLayer / 2, `speckle: ${blood} blood of ${muscleLayer} in the muscle layer (~25%)`);
  const again = ANA.planAnatomy(p, recipe, MAT);
  ok(again.report.total === 0, 'applying the recipe again is a no-op');
  // A face baked as interior by an older recipe is repaired, not kept.
  VOX.gridSet(p.models[0].grid, 4, 3, 4, MAT.bone);
  VOX.gridColorLayer(p.models[0].grid)[4 + 3 * 9 + 4 * 81] = 0;
  const repair = ANA.planAnatomy(p, recipe, MAT);
  ok(repair.report.total === 1 && repair.edits[0].mats[0] === MAT.skin, 'a kept face that is bone is rewritten to skin');
  ANA.applyPlan(p, repair);
  ok(at(0, 4, 3, 4) === MAT.skin, 'repaired');
  const bad = ANA.planAnatomy(block(), { layers: [{ material: 'skin', keep: true, depth: 1 }, { material: 'chrome' }] }, MAT);
  ok(bad.report.unresolved.includes('chrome') && bad.report.total === 0, 'an unknown material is reported, not written');
}

/* ---- 4. the real human --------------------------------------------------- */
console.log('human.vox');
{
  const buf = readFileSync(join(ROOT, 'assets/mobs/human.vox'));
  const parsed = VOX.readVox(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
  const side = JSON.parse(readFileSync(join(ROOT, 'assets/mobs/human.json'), 'utf8'));
  const materials = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
  const matId = ANA.materialIds(materials);
  ok(!!side.anatomy, 'human.json carries an anatomy recipe');
  const recipe = side.anatomy || ANA.DEFAULT_ANATOMY;
  const plan = ANA.planAnatomy(parsed.prefab, recipe, matId);
  ok(plan.report.unresolved.length === 0, 'recipe materials exist: ' + (plan.report.unresolved.join(', ') || 'all'));
  ok(plan.report.total === 0, `committed human.vox is baked (${plan.report.total} voxels differ from the recipe)`);
  const bone = matId.bone;
  let limbsWithBone = 0, painted = 0, paintedDeep = 0;
  for (const m of parsed.prefab.models) {
    let n = 0;
    for (let i = 0; i < m.grid.data.length; i++) {
      if (m.grid.data[i] === bone) n++;
      if (m.grid.color && m.grid.color[i]) {
        painted++;
        const x = i % m.dim.x, y = ((i / m.dim.x) | 0) % m.dim.y, z = (i / (m.dim.x * m.dim.y)) | 0;
        if (ANA.depthAt(plan.field, m, x, y, z) > 0) paintedDeep++;
      }
    }
    if (n) limbsWithBone++;
  }
  ok(limbsWithBone === parsed.prefab.models.length, `every limb has a bone core (${limbsWithBone}/${parsed.prefab.models.length})`);
  ok(painted > 0 && paintedDeep === 0, `paint lives on the surface only (${painted} painted, ${paintedDeep} buried)`);
}

/* ---- 4b. the HEART (2026-10-12, docs/PLAN_projectiles.md package A) -------
 *
 * The chest's organ, baked as a stand-in and rewritten to `heart` at load
 * (DEFAULT_ANATOMY limbs.torso + `becomes`). Pinned here because the C++
 * charge (materials.json heart.organHp) is calibrated against this exact
 * count: 15% of 84 cells takes the human's 180-hp torso to 0. A recipe edit
 * that moves the count has to move that figure with it. */
console.log('human heart');
{
  const materials = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
  const matId = ANA.materialIds(materials);
  const side = JSON.parse(readFileSync(join(ROOT, 'assets/mobs/human.json'), 'utf8'));
  const recipe = side.anatomy;
  const STAND = 'mushroom_large';
  ok(recipe.becomes && recipe.becomes[STAND] === 'heart' &&
     ANA.DEFAULT_ANATOMY.becomes && ANA.DEFAULT_ANATOMY.becomes[STAND] === 'heart',
     'the stand-in becomes the heart at load (human.json and DEFAULT_ANATOMY)');
  ok(matId[STAND] > 0 && matId[STAND] <= 127, `${STAND} is paintable in a .vox (id ${matId[STAND]})`);
  ok(matId.heart > 127 && matId.heart <= 255,
     `heart (id ${matId.heart}) is past the .vox palette but inside a micro brick's 8-bit material`);
  const heartDef = materials[matId.heart - 1];
  ok(heartDef && heartDef.organHp > 0, `heart carries an organHp (${heartDef && heartDef.organHp})`);
  const brainDef = materials[matId.brain - 1];
  ok(brainDef && brainDef.organHp === 3.5, `brain keeps the 3.5 it had as gore.brainHpPerVoxel (${brainDef && brainDef.organHp})`);

  const read = () => {
    const b = readFileSync(join(ROOT, 'assets/mobs/human.vox'));
    return VOX.readVox(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)).prefab;
  };
  const baked = read();
  // The same body baked WITHOUT the heart rule: what the heart cells would be.
  const noHeart = JSON.parse(JSON.stringify(recipe));
  noHeart.limbs.torso.carve = noHeart.limbs.torso.carve.filter(c => c.material !== STAND);
  const bare = read();
  ANA.applyPlan(bare, ANA.planAnatomy(bare, noHeart, matId));
  const stand = matId[STAND];
  let heart = 0, elsewhere = 0, notFlesh = 0, left = 0, sumZ = 0;
  let torsoDim = null;
  baked.models.forEach((m, mi) => {
    const d = m.dim;
    for (let i = 0; i < m.grid.data.length; i++) {
      if (m.grid.data[i] !== stand) continue;
      if (m.name !== 'torso') { elsewhere++; continue; }
      torsoDim = d;
      heart++;
      if (bare.models[mi].grid.data[i] !== matId.flesh) notFlesh++;
      const x = i % d.x, z = (i / (d.x * d.y)) | 0;
      if (x + 0.5 > d.x / 2) left++;
      sumZ += z + 0.5;
    }
  });
  ok(heart === 84, `the stock human's heart is 84 cells (${heart}) -- heart.organHp is calibrated to it`);
  ok(elsewhere === 0, `the heart is in the torso only (${elsewhere} elsewhere)`);
  ok(notFlesh === 0, `every heart cell was organ fill: no rib, sternum, spine or muscle wall lost to it (${notFlesh})`);
  ok(heart > 0 && left / heart >= 0.6 && left < heart,
     `left of the midline, not all of it (${left}/${heart} on the +x, left, side)`);
  ok(torsoDim && heart > 0 && sumZ / heart > torsoDim.z * 0.4,
     `toward the front of the chest (mean z ${(sumZ / Math.max(1, heart)).toFixed(2)} of ${torsoDim && torsoDim.z})`);
}

/* ---- 5. AND EVERY OTHER BAKED BODY ---------------------------------------
 *
 * Section 4 pins the human, which is the body the recipe was written for and
 * the one least likely to go stale. What goes stale is a CHARACTER: it carries
 * its own 200 KB of art, its recipe now arrives through `extends`, and editing
 * the human's recipe therefore changes what its interior SHOULD be without
 * touching a byte of what it IS.
 *
 * The engine resolves that at load now (src/game/anatomy_resolve.cpp), so a
 * stale body is no longer a wrong body in the GAME — but it is still a wrong
 * body in the Models tab's peel, in any census read off disk, and in this file.
 * Running the planner over every .vox is ~20 ms a body and says exactly which
 * one and by how much; a stored hash would say only that something moved.
 */
console.log('every baked body');
{
  const mobsDir = join(ROOT, 'assets/mobs');
  const materials = JSON.parse(
    readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
  const matId = ANA.materialIds(materials);
  const readStem = stem =>
    JSON.parse(readFileSync(join(mobsDir, stem + '.json'), 'utf8'));
  const readEffect = name => {
    const p = join(mobsDir, 'effects', name + '.json');
    return existsSync(p) ? JSON.parse(readFileSync(p, 'utf8')) : null;
  };
  let bodies = 0;
  for (const f of readdirSync(mobsDir).sort()) {
    if (!f.endsWith('.vox')) continue;
    const stem = f.slice(0, -4);
    // THE RESOLVED SIDECAR, not the file on disk: a character's recipe is
    // inherited and an effect may override it, so asking the raw file would
    // compare the art against a recipe the engine does not use.
    let side;
    try { side = SC.resolveSidecar(readStem, stem, readEffect); }
    catch (e) { ok(false, `${stem}.json resolves: ${e.message}`); continue; }
    if (!side.anatomy) continue;      // no recipe, no claim (critter, dummy)
    bodies++;
    const buf = readFileSync(join(mobsDir, f));
    const parsed = VOX.readVox(
      buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
    const plan = ANA.planAnatomy(parsed.prefab, side.anatomy, matId);
    ok(plan.report.unresolved.length === 0,
       `${stem}: every material its recipe names exists` +
       (plan.report.unresolved.length
          ? ': MISSING ' + plan.report.unresolved.join(', ') : ''));
    ok(plan.report.total === 0,
       `${stem}.vox is in sync with its resolved recipe ` +
       `(${plan.report.total} voxels differ; re-bake with ` +
       `\`node scripts/anatomize_mob.mjs ${stem}\`)`);
  }
  ok(bodies > 1, `there is more than one baked body to check (${bodies})`);
}

console.log(fails ? `\n${fails} FAILED` : '\nall ok');
process.exit(fails ? 1 : 0);
