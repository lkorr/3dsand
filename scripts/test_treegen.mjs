/* test_treegen.mjs — the tree generator's own gate.
 *
 * WHAT IT GUARDS. treegen.js is the only voxelizer, and its output is baked
 * into an ENGINE INPUT (.svtree). So the two properties that matter are not
 * "does it look nice" — the tuner and scripts/shot_tree.mjs answer that — but:
 *
 *   1. DETERMINISM. Same params + same seed -> byte-identical cells, run
 *      after run and machine after machine. Without it a re-bake silently
 *      moves the world hash and every rebaseline is a lie.
 *   2. ROUND TRIP. The .svtree encoder and the decoder agree cell for cell,
 *      through the exact column/run path the WGSL sampler takes. This is the
 *      test that catches an off-by-one in the run packing, which would
 *      otherwise show up as a forest of sheared trees after a 50 s selftest.
 *
 * plus preset sanity (each shipped species generates, fits its declared
 * bounds, has wood and leaves where it should, and stays inside the format's
 * hard limits).
 *
 *   node scripts/test_treegen.mjs
 *
 * Node-only; treegen.js is pure, so nothing here needs a browser.
 */
import { readFileSync, existsSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const T = await load('assets/editor/treegen.js');
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8'));
const MATNAMES = new Set(MATS.materials.map(m => m.id));

let fails = 0, count = 0;
const ok = (cond, what) => {
  count++;
  if (cond) { console.log('  ok   ' + what); return true; }
  console.log('  FAIL ' + what); fails++; return false;
};

/** FNV-1a over the cell words — a stable fingerprint of a whole tree. */
function fnv(cells) {
  let h = 0x811c9dc5;
  for (let i = 0; i < cells.length; i++) {
    h ^= cells[i] & 0xFF; h = Math.imul(h, 0x01000193) >>> 0;
    h ^= (cells[i] >>> 8) & 0xFF; h = Math.imul(h, 0x01000193) >>> 0;
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

const DIR = join(ROOT, 'assets', 'trees');
const species = existsSync(DIR)
    ? readdirSync(DIR).filter(f => f.endsWith('.json')).map(f => f.slice(0, -5)).sort()
    : [];
const paramsOf = n => JSON.parse(readFileSync(join(DIR, n + '.json'), 'utf8'));

/* ---- 1. determinism ------------------------------------------------------ */
console.log('\n-- determinism --');
{
  const p = paramsOf(species.includes('oak') ? 'oak' : species[0]);
  const a = T.generateTree(p, 7);
  const b = T.generateTree(p, 7);
  ok(fnv(a.cells) === fnv(b.cells), `same params+seed -> same cells (${fnv(a.cells)})`);
  ok(a.dim.x === b.dim.x && a.dim.y === b.dim.y && a.dim.z === b.dim.z,
     'same params+seed -> same dimensions');
  const c = T.generateTree(p, 8);
  ok(fnv(c.cells) !== fnv(a.cells), 'a different seed -> a different tree');

  // PATH KEYING, not a sequential stream. Changing one leaf parameter must not
  // reshuffle the SKELETON — that is what makes the sliders usable, and it is
  // the property a sequential RNG silently destroys.
  const q = JSON.parse(JSON.stringify(p));
  q.foliage.density = Math.max(0.2, (q.foliage.density || 0.8) - 0.15);
  const d = T.generateTree(q, 7);
  const skelA = a.skeleton.stems.map(s => s.pathHash).join(',');
  const skelD = d.skeleton.stems.map(s => s.pathHash).join(',');
  ok(skelA === skelD,
     'a foliage-only change leaves the skeleton bit-identical');
  ok(fnv(d.cells) !== fnv(a.cells), '...but does change the voxels');
}

/* ---- 2. .svtree round trip ----------------------------------------------- */
console.log('\n-- .svtree round trip --');
{
  const name = species.includes('oak') ? 'oak' : species[0];
  const p = paramsOf(name);
  const seeds = [0, 1];
  const baked = T.bakeAtlas(p, seeds);
  const A = T.readAtlas(baked.buf);
  ok(A.variants.length === seeds.length, `${A.variants.length} variants in the file`);
  ok(A.names.length > 0 && A.names.every(n => MATNAMES.has(n)),
     'every material name in the file exists in materials.json: ' + A.names.join(', '));

  let mismatch = 0, checked = 0, nonAir = 0;
  for (let vi = 0; vi < seeds.length; vi++) {
    const t = T.generateTree(p, seeds[vi]);
    const v = A.variants[vi];
    ok(v.nx === t.dim.x && v.ny === t.dim.y && v.nz === t.dim.z,
       `variant ${vi} dims match (${v.nx}x${v.ny}x${v.nz})`);
    ok(v.anchorX === t.anchor.x && v.anchorZ === t.anchor.z,
       `variant ${vi} trunk anchor matches (${v.anchorX},${v.anchorZ})`);
    // EVERY cell, not a sample: the decode is the shader's own path and a
    // one-in-a-million column bug is a sheared tree somebody has to find in a
    // screenshot. A 100x100x200 grid is 2M compares and takes ~1 s.
    const local = new Map();
    t.names.forEach((n, i) => local.set(i + 1, A.names.indexOf(n) + 1));
    for (let z = 0; z < v.nz; z++)
      for (let y = 0; y < v.ny; y++)
        for (let x = 0; x < v.nx; x++) {
          const src = t.cells[(z * t.dim.y + y) * t.dim.x + x];
          const want = src ? ((local.get(src & 0xFFF) & 0xFFF) | (src & 0xF000)) : 0;
          const got = A.cellAt(vi, x, y, z);
          checked++;
          if (want) nonAir++;
          if (want !== got) mismatch++;
        }
  }
  ok(mismatch === 0,
     `${checked.toLocaleString()} cells decode identically (${nonAir.toLocaleString()} non-air)`);
}

/* ---- 3. preset sanity ---------------------------------------------------- */
console.log('\n-- shipped species --');
ok(species.length >= 5, `${species.length} species in assets/trees/`);
for (const name of species) {
  const p = paramsOf(name);
  const r = T.generateTree(p, 0);
  const m = r.meta;
  const fine =
      !m.clipped &&
      r.dim.x > 2 && r.dim.y > 4 && r.dim.z > 2 &&
      r.dim.y <= 512 &&                       // the run encoder's 9-bit y0
      m.above === Math.min(m.above, r.dim.y) &&
      m.reachXZ > 0 && m.woodCount > 0 &&
      // Every species except the dead one must carry foliage; the dead one
      // must carry none, or its whole point is gone.
      (name === 'dead' ? m.leafCount === 0 : m.leafCount > 200) &&
      r.names.every(n => MATNAMES.has(n));
  ok(fine, `${name}: ${r.dim.x}x${r.dim.y}x${r.dim.z} reach ${m.reachXZ} ` +
           `wood ${m.woodCount} leaf ${m.leafCount} ` +
           (m.truncated ? '(TRUNCATED) ' : '') + (m.clipped ? '(CLIPPED) ' : ''));

  // The reach/above metadata is what the engine's candidate reject trusts. A
  // value that is too small SHEARS the tree, silently, in the world only.
  let maxR = 0, maxY = 0;
  for (let z = 0; z < r.dim.z; z++)
    for (let y = 0; y < r.dim.y; y++)
      for (let x = 0; x < r.dim.x; x++)
        if (r.cells[(z * r.dim.y + y) * r.dim.x + x]) {
          maxR = Math.max(maxR, Math.abs(x - r.anchor.x), Math.abs(z - r.anchor.z));
          maxY = Math.max(maxY, y + 1);
        }
  ok(m.reachXZ >= maxR && m.above >= maxY,
     `${name}: declared reach ${m.reachXZ} >= actual ${maxR}, ` +
     `above ${m.above} >= actual ${maxY}`);
}

/* ---- 4. the committed atlases are current -------------------------------- */
// A .svtree that no longer matches its .json is the worst failure mode here:
// the editor shows one tree and the world grows another, and nothing else in
// the repo notices. Cheap to check, so check it.
console.log('\n-- committed atlases --');
for (const name of species) {
  const path = join(DIR, name + '.svtree');
  if (!existsSync(path)) { ok(false, `${name}.svtree is missing — run scripts/bake_trees.mjs`); continue; }
  const disk = readFileSync(path);
  const fresh = Buffer.from(T.bakeAtlas(paramsOf(name)).buf);
  ok(disk.length === fresh.length && disk.equals(fresh),
     `${name}.svtree matches ${name}.json (${(disk.length / 1024).toFixed(0)} KiB)`);
}

console.log(`\n${count - fails}/${count} checks passed`);
process.exit(fails ? 1 : 0);
