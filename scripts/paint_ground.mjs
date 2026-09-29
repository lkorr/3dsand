/* paint_ground.mjs -- PAINT the ground of a map along a path: a gravel track,
 * a dirt lane, a flagstone apron. Headless and scriptable.
 *
 * The ground outside a house is not any structure's to edit, so the in-game
 * editor's voxel tools (which edit an OPEN HOUSE) cannot reach it. It belongs
 * to the map's EDIT LAYER (assets/worldedits/<layer>.svedit, named by map.json
 * `editLayer`; src/sim/worldedit.h): a sparse, GROUND-RELATIVE patch the
 * engine applies to every chunk it generates, through the MutationQueue. The
 * World map page's voxel view paints the same file by hand; this paints it
 * along a polyline, which is what a path is.
 *
 *   node scripts/paint_ground.mjs <map> --mat gravel --width 12 --path x,z x,z [x,z ...]
 *        [--depth 1] [--ragged 2] [--layer <name>] [--erase] [--dry]
 *
 *   --path     world-voxel columns; the path runs straight between them
 *   --width    voxels across (a footpath is 8-12, a lane 16-20)
 *   --mat      a materials.json id (gravel, dirt, flagstone, ...)
 *   --depth    how many cells down from the ground's top are replaced (1)
 *   --ragged   voxels the edge wanders in and out (a counter hash of the
 *              column, so the same command paints the same cells), 0 = ruled
 *   --layer    the edit layer; default: the one map.json names, else
 *              "<map>_ground", which is then written into map.json
 *   --erase    take those columns OUT of the layer instead
 *
 * GROUND-RELATIVE (SVED v2): each cell is stored as an offset from the
 * ground at its column (dy 0 = the top ground cell, -1 the one under it), so
 * the path lies ON the ground wherever the terrain, the sculpt layer or a
 * house's pad puts it, and follows it if they change. Later strokes win over
 * earlier ones in a column they share.
 *
 * MOVES THE WORLD HASH of that map (a layer puts voxels in the world). No
 * gate's map names a layer, so no gate moves.
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (f, d) => { const i = args.indexOf(f); return i >= 0 ? args[i + 1] : d; };
const map = args[0];
const pathAt = args.indexOf('--path');
if (!map || map.startsWith('--') || pathAt < 0) {
  console.error('usage: node scripts/paint_ground.mjs <map> --mat gravel --width 12 --path x,z x,z [x,z ...] ' +
                '[--depth 1] [--ragged 2] [--layer name] [--erase] [--dry]');
  process.exit(2);
}
const pts = [];
for (let i = pathAt + 1; i < args.length && !args[i].startsWith('--'); i++) {
  const [x, z] = args[i].split(',').map(Number);
  if (!Number.isFinite(x) || !Number.isFinite(z)) { console.error('bad path point "' + args[i] + '"'); process.exit(2); }
  pts.push([x, z]);
}
if (pts.length < 1) { console.error('--path needs at least one x,z'); process.exit(2); }
const erase = args.includes('--erase');
const dry = args.includes('--dry');
const width = Math.max(1, Number(opt('--width', 10)));
const depth = Math.max(1, Number(opt('--depth', 1)) | 0);
const ragged = Math.max(0, Number(opt('--ragged', 2)));
const matName = opt('--mat', 'gravel');

const mapDir = join(ROOT, 'assets/worldmap', map);
const mapJsonPath = join(mapDir, 'map.json');
if (!existsSync(mapJsonPath)) { console.error('no map "' + map + '"'); process.exit(2); }
const mapText = readFileSync(mapJsonPath, 'utf8');
const mapJson = JSON.parse(mapText);
const layer = opt('--layer', mapJson.editLayer || `${map}_ground`);
if (!/^[a-z0-9_]+$/.test(layer)) { console.error('layer names are lowercase words: "' + layer + '"'); process.exit(2); }

// Material id = index in materials.json + 1 (air is id 0 and not in the file).
const mats = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const mi = mats.findIndex(m => m.id === matName);
if (!erase && mi < 0) { console.error('no material "' + matName + '" in materials.json'); process.exit(2); }
const matId = mi + 1;

// The counter hash (housegen.js's): stable per column, no Math.random.
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0; x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0; x = Math.imul(x, 0x846ca68b) >>> 0;
  return (x ^ (x >>> 16)) >>> 0;
}
const hash = (a, b, c) => mix32((mix32((mix32(a | 0) ^ (b | 0)) >>> 0) ^ (c | 0)) >>> 0);

// ---- the SVED file (src/sim/worldedit.cpp is the authority) ----------------
const MAGIC = 0x44455653, FLAG_RELATIVE = 1;
const file = join(ROOT, 'assets/worldedits', layer + '.svedit');
const cols = new Map();   // "x,z" -> [{dy, word}]
let seed = 1337;
if (existsSync(file)) {
  const b = readFileSync(file), d = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (d.getUint32(0, true) !== MAGIC) { console.error(file + ': not an SVED layer'); process.exit(1); }
  const ver = d.getUint32(4, true), flags = d.getUint32(20, true);
  if (ver !== 2 || !(flags & FLAG_RELATIVE)) {
    console.error(file + ': a v1 (absolute) layer -- open it on the World map page and save it once to convert');
    process.exit(1);
  }
  seed = d.getUint32(16, true);
  let p = 32;
  const n = d.getUint32(8, true);
  for (let i = 0; i < n; i++) {
    const x = d.getInt32(p, true), z = d.getInt32(p + 4, true), k = d.getUint32(p + 8, true);
    p += 12;
    const col = [];
    for (let j = 0; j < k; j++) { col.push({dy: d.getInt32(p, true), word: d.getUint32(p + 4, true)}); p += 8; }
    cols.set(x + ',' + z, col);
  }
}

// ---- the stroke -------------------------------------------------------------
const segDist = (px, pz, a, b) => {
  const dx = b[0] - a[0], dz = b[1] - a[1];
  const L2 = dx * dx + dz * dz;
  const t = L2 > 0 ? Math.max(0, Math.min(1, ((px - a[0]) * dx + (pz - a[1]) * dz) / L2)) : 0;
  const qx = a[0] + t * dx - px, qz = a[1] + t * dz - pz;
  return Math.sqrt(qx * qx + qz * qz);
};
const half = width / 2;
let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
for (const [x, z] of pts) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); z0 = Math.min(z0, z); z1 = Math.max(z1, z); }
const reach = Math.ceil(half + ragged + 1);
let painted = 0;
for (let z = Math.floor(z0 - reach); z <= Math.ceil(z1 + reach); z++) {
  for (let x = Math.floor(x0 - reach); x <= Math.ceil(x1 + reach); x++) {
    let d = Infinity;
    if (pts.length === 1) d = segDist(x + 0.5, z + 0.5, pts[0], pts[0]);
    for (let i = 0; i + 1 < pts.length; i++) d = Math.min(d, segDist(x + 0.5, z + 0.5, pts[i], pts[i + 1]));
    // The edge wanders by up to `ragged`, smoothly-ish: the hash of the 4x4
    // block the column is in, so the edge moves in short runs, not per voxel.
    const edge = half + (ragged ? ((hash(x >> 2, z >> 2, 0x9a7) % 1000) / 1000 - 0.5) * 2 * ragged : 0);
    if (d > edge) continue;
    const key = x + ',' + z;
    if (erase) { if (cols.delete(key)) painted++; continue; }
    const col = [];
    for (let k = 0; k < depth; k++) {
      // State nibble = the palette variant (0..2), as worldgen jitters a solid.
      const v = hash(x, z, k) % 3;
      col.push({dy: -k, word: (matId | (v << 12)) >>> 0});
    }
    cols.set(key, col);
    painted++;
  }
}

// ---- write: columns in (x, z) order, cells by dy (the engine's order) ------
const keys = [...cols.keys()].map(k => k.split(',').map(Number)).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
let cellCount = 0;
for (const [x, z] of keys) cellCount += cols.get(x + ',' + z).length;
const out = Buffer.alloc(32 + keys.length * 12 + cellCount * 8);
out.writeUInt32LE(MAGIC, 0); out.writeUInt32LE(2, 4); out.writeUInt32LE(keys.length, 8);
out.writeUInt32LE(cellCount, 12); out.writeUInt32LE(seed, 16); out.writeUInt32LE(FLAG_RELATIVE, 20);
let p = 32;
for (const [x, z] of keys) {
  const col = cols.get(x + ',' + z).slice().sort((a, b) => a.dy - b.dy);
  out.writeInt32LE(x, p); out.writeInt32LE(z, p + 4); out.writeUInt32LE(col.length, p + 8); p += 12;
  for (const c of col) { out.writeInt32LE(c.dy, p); out.writeUInt32LE(c.word, p + 4); p += 8; }
}
console.log(`${erase ? 'erased' : 'painted'} ${painted} columns${erase ? '' : ' of ' + matName}; layer "${layer}" ` +
            `now ${keys.length} columns, ${cellCount} cells`);
if (dry) { console.log('dry run: nothing written'); process.exit(0); }
mkdirSync(dirname(file), {recursive: true});
writeFileSync(file, out);
// Name the layer on the map (a text edit, so the file keeps its formatting).
if (mapJson.editLayer !== layer) {
  let t = mapText;
  if (typeof mapJson.editLayer === 'string') t = t.replace(/"editLayer":\s*"[^"]*"/, `"editLayer": "${layer}"`);
  else t = t.replace(/("name":\s*"[^"]*",)/, `$1\n  "editLayer": "${layer}",`);
  if (JSON.parse(t).editLayer !== layer) { console.error('could not write editLayer into map.json'); process.exit(1); }
  writeFileSync(mapJsonPath, t);
  console.log(`map.json: editLayer = "${layer}"`);
}
console.log('wrote ' + file);
