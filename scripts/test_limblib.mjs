/* Round-trip test for the limb library format (assets/editor/limblib.js).
 *
 * Takes a real mob out of assets/mobs/, saves a limb subtree the way the
 * editor would, reads it back, places it on a DIFFERENT creature, and asserts
 * the two things the format exists to preserve: the voxels are bit-identical,
 * and the joint lands where it was aimed.
 *
 * Node-only; the editor modules it imports are pure (no DOM, no fetch).
 *   node scripts/test_limblib.mjs
 */
import { readFileSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
// pathToFileURL, not a bare path: on Windows an absolute path starts with a
// drive letter, which the ESM loader reads as an unknown URL scheme.
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const VOX = await load('assets/editor/vox.js');
const LIB = await load('assets/editor/limblib.js');

let fails = 0;
const ok = (cond, what) => {
  if (cond) { console.log('  ok   ' + what); return true; }
  console.log('  FAIL ' + what); fails++; return false;
};

const loadMob = name => {
  const buf = readFileSync(join(ROOT, 'assets/mobs', name + '.vox'));
  const parsed = VOX.readVox(buf.buffer.slice(buf.byteOffset,
                                              buf.byteOffset + buf.byteLength));
  const side = JSON.parse(readFileSync(join(ROOT, 'assets/mobs', name + '.json'),
                                       'utf8'));
  return { doc: parsed.prefab, palette: parsed.palette, limbs: side.limbs || [] };
};

/* ---- 1. subtree walk ---------------------------------------------------- */
const asha = loadMob('asha');
console.log(`asha: ${asha.doc.models.length} models, ${asha.limbs.length} limbs`);

const arm = asha.limbs.find(l => /^armU/.test(l.name));
if (!arm) { console.log('no armU limb in asha — test needs updating'); process.exit(1); }
const kin = LIB.subtreeNames(asha.limbs, arm.name);
console.log(`\nsubtree of ${arm.name}: ${kin.join(' > ')}`);
ok(kin[0] === arm.name, 'root comes first');
ok(kin.length > 1, 'the arm has descendants');
// Parents must precede children, or placement re-parents onto a limb that is
// not there yet.
for (let i = 0; i < kin.length; i++) {
  const l = asha.limbs.find(x => x.name === kin[i]);
  if (l && l.parent && kin.includes(l.parent))
    ok(kin.indexOf(l.parent) < i, `${l.parent} precedes ${l.name}`);
}

/* ---- 2. save -> .vox bytes -> read back ---------------------------------- */
const part = LIB.partFromPrefab(asha.doc, asha.limbs, arm.name, { from: 'asha' });
console.log(`\npart: ${part.names.join(', ')}  dim ${part.json.meta.dim.join('x')}`);
ok(part.json.v === LIB.PART_VERSION, 'version stamped');
ok(part.json.limbs.length === part.names.length, 'one json limb per model');

// anchorLocal must be relative to the PART, not the creature. asha's arm sits
// far from the prefab origin, so a stored prefab anchor would be obviously
// larger than the part's own box.
const rootEntry = part.json.limbs.find(l => l.name === arm.name);
if (Array.isArray(arm.anchor)) {
  ok(!!rootEntry.anchorLocal, 'root carries anchorLocal');
  const within = rootEntry.anchorLocal.every((v, i) =>
    v >= -8 && v <= part.json.meta.dim[i] + 8);
  ok(within, `anchorLocal ${JSON.stringify(rootEntry.anchorLocal)} is inside ` +
             `the part box ${JSON.stringify(part.json.meta.dim)} ` +
             `(creature anchor was ${JSON.stringify(arm.anchor)})`);
}
// Unknown keys must survive: asha's limbs carry `spring`, `severImpactSpeed`.
const src = asha.limbs.find(l => l.name === arm.name);
for (const k of Object.keys(src)) {
  if (['name', 'parent', 'anchor'].includes(k)) continue;
  ok(JSON.stringify(rootEntry[k]) === JSON.stringify(src[k]),
     `verbatim key "${k}" survives the save`);
}

const palette = VOX.paletteFromMaterials([]);
const bytes = VOX.writeVox(VOX.prefabToVoxModels(part.prefab), palette,
                           { scene: true });
console.log(`wrote ${bytes.length} bytes`);
const back = LIB.readPart(bytes.buffer.slice(bytes.byteOffset,
                                             bytes.byteOffset + bytes.byteLength),
                          part.json);
ok(back.root === arm.name, 'root survives the round trip');
ok(back.prefab.models.length === part.prefab.models.length,
   `model count survives (${back.prefab.models.length})`);

// Voxels must be bit-identical, cell for cell.
let cellsChecked = 0, mismatch = 0;
for (const m of part.prefab.models) {
  const r = back.prefab.models.find(x => x.name === m.name);
  if (!r) { mismatch++; continue; }
  if (r.dim.x !== m.dim.x || r.dim.y !== m.dim.y || r.dim.z !== m.dim.z) {
    console.log(`    dim differs on ${m.name}: ` +
                `${JSON.stringify(m.dim)} vs ${JSON.stringify(r.dim)}`);
    mismatch++; continue;
  }
  for (let i = 0; i < m.grid.data.length; i++) {
    cellsChecked++;
    if (m.grid.data[i] !== r.grid.data[i]) mismatch++;
  }
}
ok(mismatch === 0, `${cellsChecked} voxel cells identical after round trip`);

/* ---- 3. wear it on a different creature ---------------------------------- */
const mina = loadMob('mina');
const target = mina.limbs.find(l => /^armU/.test(l.name));
console.log(`\nswapping onto mina's ${target.name} ` +
            `(anchor ${JSON.stringify(target.anchor)})`);

const taken = new Set(mina.doc.models.map(m => m.name));
for (const n of LIB.subtreeNames(mina.limbs, target.name)) taken.delete(n);
const renames = LIB.uniqueNames(back, taken);
LIB.renameWithin(back, renames);

const at = LIB.placementFor(back, target.anchor);
const entries = LIB.sidecarEntries(back, at, target.parent);
const newRoot = entries.find(e => e.name === back.root);

// THE POINT OF THE WHOLE FORMAT: the incoming joint lands on the target joint.
ok(JSON.stringify(newRoot.anchor) === JSON.stringify(target.anchor),
   `root anchor lands exactly on the target joint ` +
   `${JSON.stringify(newRoot.anchor)}`);
ok(newRoot.parent === target.parent,
   `root re-parented to "${newRoot.parent}" (was "${target.parent}")`);
// Children keep pointing inside the part, not at asha's limbs.
for (const e of entries.slice(1))
  ok(entries.some(x => x.name === e.parent),
     `"${e.name}" parents to "${e.parent}", which came with the part`);
// Relative geometry is rigid: every limb keeps its offset from the root.
const rootModel = back.prefab.models.find(m => m.name === back.root);
for (const m of back.prefab.models) {
  if (VOX.isArtLayerName(m.name) || m.name === back.root) continue;
  const orig = part.prefab.models.find(
    x => (renames.get(x.name) || x.name) === m.name);
  if (!orig) continue;
  const origRoot = part.prefab.models.find(x => x.name === arm.name);
  const d0 = ['x', 'y', 'z'].map(a => orig.offset[a] - origRoot.offset[a]);
  const d1 = ['x', 'y', 'z'].map(a => m.offset[a] - rootModel.offset[a]);
  ok(JSON.stringify(d0) === JSON.stringify(d1),
     `"${m.name}" keeps its offset from the root ${JSON.stringify(d1)}`);
}

/* ---- 4. name collision --------------------------------------------------- */
// A SWAP frees the names it is about to overwrite, so an arm replacing an arm
// of the same name keeps that name — renaming there would leave the creature
// with "armU.L.2" for no reason and break clips that address the limb by name.
const renamedInSwap = [...renames.entries()].filter(([f, t]) => f !== t);
ok(renamedInSwap.length === 0,
   'a swap onto the same names keeps them (the old subtree frees them)');
ok(new Set([...renames.values()]).size === renames.size,
   'renames are unique among themselves');

// ADD is the other case: nothing is freed, so every colliding name must move.
const addTaken = new Set(mina.doc.models.map(m => m.name));
const back2 = LIB.readPart(bytes.buffer.slice(bytes.byteOffset,
                                              bytes.byteOffset + bytes.byteLength),
                           part.json);
const addRenames = LIB.uniqueNames(back2, addTaken);
LIB.renameWithin(back2, addRenames);
const moved = [...addRenames.entries()].filter(([f, t]) => f !== t);
ok(moved.length === back2.limbs.length,
   `adding beside the originals renames all ${moved.length}: ` +
   moved.map(([f, t]) => f + '->' + t).join(', '));
for (const to of addRenames.values())
  ok(!mina.doc.models.some(m => m.name === to),
     `"${to}" does not collide with a limb mina already has`);
// The rename must reach the parent pointers too, or the added arm hangs off
// mina's OWN forearm instead of its own.
const addEntries = LIB.sidecarEntries(back2, { x: 0, y: 0, z: 0 }, 'torso');
for (const e of addEntries.slice(1))
  ok(addEntries.some(x => x.name === e.parent),
     `added "${e.name}" parents to "${e.parent}", inside the part`);
ok(back2.prefab.models.every(m => addRenames.has(VOX.isArtLayerName(m.name)
     ? VOX.artLayerBase(m.name) : m.name) === false ||
     !mina.doc.models.some(x => x.name === m.name)),
   'no grafted model name collides with an existing one');

/* ---- 5. art colour survives a foreign palette ----------------------------
 *
 * No shipped mob is painted, so this case is built by hand — and it is the
 * one that fails SILENTLY. An art index is only meaningful beside the palette
 * it was allocated from, so a limb painted in a file whose palette differs
 * from the target's arrives the wrong colour unless every index is resolved
 * to a hex and re-allocated. This asserts the hexes, not the indices.
 */
console.log('\nart colour across palettes:');
{
  const dim = { x: 2, y: 2, z: 2 };
  const g = VOX.makeGrid(dim);
  g.data.fill(1);
  const srcArt = new VOX.ArtPalette(['#ff0000', '#00ff00', '#0000ff']);
  const col = VOX.gridColorLayer(g);
  const want = ['#ff0000', '#00ff00', '#0000ff', '#ff0000',
                '#00ff00', '#0000ff', '#ff0000', '#00ff00'];
  want.forEach((hex, i) => { col[i] = srcArt.alloc(hex); });

  const srcPal = VOX.paletteFromMaterials([]);
  srcArt.writeInto(srcPal);
  const b = VOX.writeVox(VOX.prefabToVoxModels(
      { size: dim, models: [{ name: 'painted', offset: { x: 0, y: 0, z: 0 }, dim, grid: g }] }),
    srcPal, { scene: true });
  const rd = VOX.readVox(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
  const m = rd.prefab.models.find(x => x.name === 'painted');
  ok(!!m && !!m.grid.color, 'the painted layer round-trips as a .col model');

  // The TARGET document's palette is deliberately different: it already holds
  // two unrelated colours, so the source indices mean something else here.
  const dstArt = new VOX.ArtPalette(['#123456', '#abcdef']);
  const remap = new Map();
  const mapped = [...m.grid.color].map(idx => {
    if (!idx) return null;
    if (!remap.has(idx)) remap.set(idx, dstArt.alloc(VOX.paletteColor(rd.palette, idx)));
    return dstArt.colorAt(remap.get(idx));
  });
  ok(JSON.stringify(mapped) === JSON.stringify(want),
     `every voxel keeps its colour through the remap ` +
     `(${[...new Set(mapped)].join(' ')})`);
  ok([...remap.values()].every(v => v !== 0), 'no index remapped to "unpainted"');
  const idxChanged = [...remap.entries()].some(([f, t]) => f !== t);
  ok(idxChanged, 'the indices genuinely differ between the two palettes ' +
     `(${[...remap.entries()].map(([f, t]) => f + '->' + t).join(' ')}) ` +
     '— so a raw copy WOULD have recoloured the limb');
}

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
