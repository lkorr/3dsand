/* paint_cellar.mjs -- THE HARROWBY SMITHY CELLAR (docs/PLAN_demons.md D2).
 *
 *   node scripts/paint_cellar.mjs [map=default] [--dry]
 *
 * A structure stamp cannot carve below ground (worldgen.wgsl: template air
 * leaves the world alone, kStampSinkMax = 8), so a cellar is the map's EDIT
 * LAYER (assets/worldedits/<layer>.svedit, named by map.json `editLayer`;
 * src/sim/worldedit.h, DESIGN.md §9c.4) -- the same file paint_ground.mjs
 * paints the village paths into. This writes every column of the cellar's
 * footprint, owning each one outright: re-running it rewrites the same
 * columns with the same cells, so it is the way to MOVE or RESHAPE the
 * cellar, and the way to re-resolve material ids after a materials.json
 * merge renumbers the tail (the layer stores ids; this script resolves them
 * by NAME every run).
 *
 * GROUND-RELATIVE (SVED v2): a cell is stored as dy from the ground at its
 * column. Every column here is inside the smithy's levelled pad, where the
 * ground's top cell is the house's pos.y - 1 = 200 (structures.h fact 2), so
 * dy = y - 200 and the cellar is rigid. Gate harrowby-cellar reads the real
 * voxels back and fails if the pad (and so the cellar) ever shears.
 *
 * WHAT IS DOWN THERE (world voxels; the smithy is refs/harrowby.json
 * harrowby/smithy, (700, 201, 3460) yaw 270; its bedroom is x 670..731,
 * z 3468..3503):
 *   - a 7 x 7 m room, air x 664..733, z 3434..3503, y 169..197 (2.9 m), cobble
 *     walls, a flagstone floor (top y 168), a stone ceiling (y 198) under the
 *     house's own footing and floor (199, 200), three timber joists;
 *   - the STAIR: a steep cellar stair (rise 2, tread 2 voxels; the player
 *     steps 6) down the bedroom's back wall, 1 m wide (z 3494..3503), from
 *     the floor opening at x 680..700 down westward to the floor at x 670.
 *     A timber rail along the opening's north edge; you step in from its
 *     east end (x 701), beside the bed;
 *   - the CIRCLE: a salt ring (centre 705, 3462; cells 14 <= r < 16, one
 *     cell tall, two wide: closed for a 4- or 8-connected flood), and just
 *     outside it (radius ~19) a pile of sulfur (NE), a pile of iron (NW) and
 *     quicksilver in a cobble dish (SE), well apart (quicksilver + sulfur
 *     makes cinnabar where they touch);
 *   - CANDLES (tallow + candle_flame, a light that does not burn) at the four
 *     cardinal points of the ring (radius 21), in three corners, on the
 *     lectern;
 *   - the LECTERN west of the ring with the open BOOK on it: the `readable`
 *     ref harrowby/smithy_notes (props.dialogue osric_notes) sits on the
 *     book's cell.
 *
 * MOVES THE WORLD HASH of the default map (a layer puts voxels in the world).
 * No gate's map names this layer except the content gates that load the
 * default map on purpose (village-harrowby, harrowby-cellar).
 */
import { readFileSync, writeFileSync, existsSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const map = args[0] && !args[0].startsWith('--') ? args[0] : 'default';
const dry = args.includes('--dry');

const mapJson = JSON.parse(readFileSync(join(ROOT, 'assets/worldmap', map, 'map.json'), 'utf8'));
const layer = mapJson.editLayer;
if (!layer) { console.error(`map "${map}" names no editLayer`); process.exit(2); }

// Material id = index in materials.json + 1 (air is 0 and not in the file).
const mats = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const M = name => {
  const i = mats.findIndex(m => m.id === name);
  if (i < 0) { console.error(`no material "${name}" in materials.json`); process.exit(2); }
  return i + 1;
};
const AIR = 0, STONE = M('stone'), COBBLE = M('cobble'), FLAG = M('flagstone'), TIMBER = M('timber'),
      PLANK = M('plank'), SALT = M('salt'), SULFUR = M('sulfur'), IRON = M('iron'),
      QUICK = M('quicksilver'), TALLOW = M('tallow'), FLAME = M('candle_flame'), LEATHER = M('leather'),
      LINEN = M('linen');
const LIQUIDS = new Set([QUICK]);

// The counter hash (paint_ground.mjs's): stable per cell, no Math.random.
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0; x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0; x = Math.imul(x, 0x846ca68b) >>> 0;
  return (x ^ (x >>> 16)) >>> 0;
}
const hash = (a, b, c) => mix32((mix32((mix32(a | 0) ^ (b | 0)) >>> 0) ^ (c | 0)) >>> 0);
// The word: material + state nibble. A liquid's nibble is its fullness code
// (7 = 8/8, full); anything else gets worldgen's 0..2 palette variant.
const word = (m, x, y, z) => m === AIR ? 0 : (m | ((LIQUIDS.has(m) ? 7 : hash(x, y * 7 + 3, z) % 3) << 12)) >>> 0;

// ---- the plan (world voxels) -----------------------------------------------
const GROUND = 200;                       // the pad's top cell (dy 0)
const FLOOR = 168, CEIL = 198;            // floor top / ceiling cell
const X0 = 664, X1 = 733, Z0 = 3434, Z1 = 3503;   // air
const WX0 = X0 - 1, WX1 = X1 + 1, WZ0 = Z0 - 1, WZ1 = Z1 + 1;   // walls
// The stair: rise 2 per tread of 2, k = 0..15 (k 15 is the floor). Tread k
// covers x [699 - 2k, 700 - 2k] and its top cell is 198 - 2k.
const SZ0 = 3494, SZ1 = 3503, SX_TOP = 700, OPEN_X0 = 680;
const stepTop = x => {             // top solid cell of the stair at column x, or null
  if (x > SX_TOP) return null;
  const k = Math.floor((SX_TOP - x) / 2);
  return k >= 15 ? FLOOR : CEIL - 2 * k;
};
const RAIL_Z = SZ0 - 1;            // the rail stands on the bedroom floor
const CX = 705, CZ = 3462;         // the circle
const JOISTS = [690, 706, 722];

// cell -> material, filled in layers (later writes win), then emitted.
const cells = new Map();           // "x,y,z" -> mat
const put = (x, y, z, m) => cells.set(`${x},${y},${z}`, m);
const box = (x0, x1, y0, y1, z0, z1, m) => {
  for (let x = x0; x <= x1; x++) for (let y = y0; y <= y1; y++) for (let z = z0; z <= z1; z++) put(x, y, z, m);
};

// 1. the room: shell, then air.
box(WX0, WX1, FLOOR - 1, CEIL, WZ0, WZ1, COBBLE);    // walls (and under the floor)
box(WX0, WX1, CEIL, CEIL, WZ0, WZ1, STONE);          // ceiling
box(X0, X1, FLOOR, FLOOR, Z0, Z1, FLAG);             // floor
box(X0, X1, FLOOR + 1, CEIL - 1, Z0, Z1, AIR);       // the room
for (const jx of JOISTS) box(jx, jx, CEIL - 1, CEIL - 1, Z0, SZ0 - 1, TIMBER);

// 2. the stair along the back wall, and its opening in the bedroom floor.
for (let x = X0; x <= SX_TOP; x++) {
  const top = stepTop(x);
  for (let z = SZ0; z <= SZ1; z++) {
    box(x, x, FLOOR, top - 1, z, z, STONE);
    put(x, top, z, FLAG);                                // flagstone treads
    const open = x >= OPEN_X0;                           // the floor opening
    for (let y = top + 1; y <= (open ? GROUND : CEIL - 1); y++) put(x, y, z, AIR);
  }
}
// The rail: a timber bar 0.9 m up on posts, along the opening's north edge,
// and a post at its west end. The east end (x 701) is the way in.
for (let x = OPEN_X0 - 1; x <= SX_TOP; x++) {
  put(x, GROUND + 9, RAIL_Z, TIMBER);
  if ((x - OPEN_X0 + 1) % 7 === 0 || x === SX_TOP) box(x, x, GROUND + 1, GROUND + 8, RAIL_Z, RAIL_Z, TIMBER);
}
for (let z = SZ0; z <= SZ1; z++) put(OPEN_X0 - 1, GROUND + 9, z, TIMBER);
box(OPEN_X0 - 1, OPEN_X0 - 1, GROUND + 1, GROUND + 8, SZ1, SZ1, TIMBER);   // its post at the wall

// 3. the circle: a salt annulus one cell tall, two wide.
const Y = FLOOR + 1;
for (let x = CX - 16; x <= CX + 16; x++)
  for (let z = CZ - 16; z <= CZ + 16; z++) {
    const dx = x + 0.5 - (CX + 0.5), dz = z + 0.5 - (CZ + 0.5);
    const r = Math.sqrt(dx * dx + dz * dz);
    if (r >= 14 && r < 16) put(x, Y, z, SALT);
  }

// 4. the piles (just outside the ring, inside D3's band).
const pile = (px, pz, m) => {   // 3x3 + 1 on top: small, low, at rest
  for (let dx = -1; dx <= 1; dx++) for (let dz = -1; dz <= 1; dz++) put(px + dx, Y, pz + dz, m);
  put(px, Y + 1, pz, m);
};
pile(CX + 14, CZ - 14, SULFUR);   // NE: casting out
pile(CX - 14, CZ - 14, IRON);     // NW: touch
// SE: quicksilver in a cobble dish (it is a liquid and would run).
for (let dx = -2; dx <= 2; dx++)
  for (let dz = -2; dz <= 2; dz++) {
    const rim = Math.abs(dx) === 2 || Math.abs(dz) === 2;
    put(CX + 14 + dx, Y, CZ + 14 + dz, rim ? COBBLE : QUICK);
  }

// 5. candles: tallow and a flame.
const candle = (x, z, y = Y, h = 3) => {
  for (let k = 0; k < h; k++) put(x, y + k, z, TALLOW);
  put(x, y + h, z, FLAME);
};
candle(CX, CZ - 21); candle(CX, CZ + 21); candle(CX - 21, CZ); candle(CX + 21, CZ);   // N S W E
candle(X0 + 2, Z0 + 2); candle(X1 - 2, Z0 + 2); candle(X1 - 2, SZ0 - 3);              // corners

// 6. the lectern and the book (west of the ring, facing it).
const LX = 676, LZ = 3462, LTOP = Y + 11;
box(LX, LX + 1, Y, LTOP - 1, LZ - 1, LZ, TIMBER);           // post
box(LX - 2, LX + 2, LTOP, LTOP, LZ - 3, LZ + 2, PLANK);     // top board
box(LX - 1, LX + 1, LTOP + 1, LTOP + 1, LZ - 2, LZ + 1, LEATHER);   // the covers
box(LX - 1, LX + 1, LTOP + 2, LTOP + 2, LZ - 2, LZ - 1, LINEN);     // two open pages
box(LX - 1, LX + 1, LTOP + 2, LTOP + 2, LZ + 1, LZ + 1, LINEN);
candle(LX + 2, LZ + 2, LTOP + 1, 1);
const BOOK = [LX, LTOP + 2, LZ - 1];   // the readable ref's pos

// ---- columns ----------------------------------------------------------------
const cols = new Map();   // "x,z" -> [{dy, word}]
for (const [k, m] of cells) {
  const [x, y, z] = k.split(',').map(Number);
  const key = x + ',' + z;
  if (!cols.has(key)) cols.set(key, []);
  cols.get(key).push({dy: y - GROUND, word: word(m, x, y, z)});
}

// ---- merge into the layer (src/sim/worldedit.cpp is the authority) ----------
const MAGIC = 0x44455653, FLAG_RELATIVE = 1;
const file = join(ROOT, 'assets/worldedits', layer + '.svedit');
const all = new Map();
let seed = 1337;
if (existsSync(file)) {
  const b = readFileSync(file), d = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (d.getUint32(0, true) !== MAGIC || d.getUint32(4, true) !== 2 || !(d.getUint32(20, true) & FLAG_RELATIVE)) {
    console.error(file + ': not a v2 (ground-relative) SVED layer'); process.exit(1);
  }
  seed = d.getUint32(16, true);
  let p = 32;
  const n = d.getUint32(8, true);
  for (let i = 0; i < n; i++) {
    const x = d.getInt32(p, true), z = d.getInt32(p + 4, true), k = d.getUint32(p + 8, true);
    p += 12;
    const col = [];
    for (let j = 0; j < k; j++) { col.push({dy: d.getInt32(p, true), word: d.getUint32(p + 4, true)}); p += 8; }
    all.set(x + ',' + z, col);
  }
}
let replaced = 0;
for (const [k, col] of cols) { if (all.has(k)) replaced++; all.set(k, col); }
const keys = [...all.keys()].map(k => k.split(',').map(Number)).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
let cellCount = 0;
for (const [x, z] of keys) cellCount += all.get(x + ',' + z).length;
const out = Buffer.alloc(32 + keys.length * 12 + cellCount * 8);
out.writeUInt32LE(MAGIC, 0); out.writeUInt32LE(2, 4); out.writeUInt32LE(keys.length, 8);
out.writeUInt32LE(cellCount, 12); out.writeUInt32LE(seed, 16); out.writeUInt32LE(FLAG_RELATIVE, 20);
let p = 32;
for (const [x, z] of keys) {
  const col = all.get(x + ',' + z).slice().sort((a, b) => a.dy - b.dy);
  out.writeInt32LE(x, p); out.writeInt32LE(z, p + 4); out.writeUInt32LE(col.length, p + 8); p += 12;
  for (const c of col) { out.writeInt32LE(c.dy, p); out.writeUInt32LE(c.word, p + 4); p += 8; }
}
console.log(`cellar: ${cells.size} cells in ${cols.size} columns (${replaced} columns replaced); ` +
            `layer "${layer}" now ${keys.length} columns, ${cellCount} cells; book at ${BOOK.join(',')}`);
if (dry) { console.log('dry run: nothing written'); process.exit(0); }
writeFileSync(file, out);
console.log('wrote ' + file);
