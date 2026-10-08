// test_maptiles.mjs — the World map page's v2 plane model (assets/editor/maptiles.js)
// without a browser: the SVT1 codec, absent = default, the brushes touch only
// the tiles under them, undo swaps restore byte-exact, a save sends only the
// dirty tiles and deletes an all-default one, listed-but-unloaded tiles are
// never written.
//
//   node scripts/test_maptiles.mjs
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const T = await import(pathToFileURL(join(ROOT, 'assets/editor/maptiles.js')).href);

let fail = 0;
const ok = (c, m) => { console.log((c ? 'PASS  ' : 'FAIL  ') + m); if (!c) fail++; };

// A 600 x 600-post height plane (3 x 3 tiles, the last row/column partial) and
// its 150 x 150 biome plane, extent centred on the origin.
const json = {
  version: 2, extent: {min: [-19200, -19200], size: [38336, 38336]}, siteCellLog2: 6,
  planes: {height: {log2: 6, tile: 256, default: -600}, biome: {log2: 8, tile: 256, default: 'ocean'}},
  biomes: ['ocean', 'meadow', 'alpine'],
};
const Hp = T.makePlane('height', json), Bp = T.makePlane('biome', json);
ok(Hp.posts[0] === 600 && Hp.tilesX === 3 && Hp.def === -600, `height plane ${Hp.posts[0]} posts, ${Hp.tilesX} tiles a side, default ${Hp.def}`);
ok(Bp.posts[0] === 150 && Bp.tilesX === 1 && Bp.def === 0, `biome plane ${Bp.posts[0]} cells, default index ${Bp.def} (ocean)`);
ok(T.heightAt(Hp, 123, -456) === -600 && T.biomeAt(Bp, 0, 0) === 0, 'an absent tile reads the default');

// codec round trip
const t = T.newTile(Hp);
for (let i = 0; i < t.length; i++) t[i] = ((i * 7919) % 64001) - 32000;
const bytes = T.encodeTile(Hp, 2, 1, t);
ok(bytes.length === 32 + 65536 * 2 && new DataView(bytes.buffer).getUint32(0, true) === 0x31545653 &&
   String.fromCharCode(...bytes.slice(0, 4)) === 'SVT1', 'height tile is 131104 bytes and starts with the bytes "SVT1"');
const back = T.decodeTile(Hp, bytes.buffer, 2, 1);
ok(back.every((v, i) => v === t[i]), 'height tile round-trips exactly');
let threw = '';
try { T.decodeTile(Hp, bytes.buffer, 1, 1); } catch (e) { threw = e.message; }
ok(/is not \(1,1\)/.test(threw), 'a tile at the wrong (tx, tz) is refused: ' + threw);
try { threw = ''; T.decodeTile(Bp, bytes.buffer, 2, 1); } catch (e) { threw = e.message; }
ok(/plane kind/.test(threw), 'a height tile is refused as a biome tile: ' + threw);

// a raise dab touches only the tiles under it, and undo restores exactly
const before = new Map();
const r = T.heightDab(Hp, -19200 + 300 * 64, -19200 + 100 * 64, {mode: 'raise', radius: 640, amount: 50}, before);
ok(r.changed > 0 && Hp.dirty.size === 1 && Hp.dirty.has(T.tileKey(1, 0)) && before.size === 1,
   `a 64 m raise inside tile (1,0) dirties only it: ${[...Hp.dirty].map(T.tileXZ).join(' ')}`);
const seam = T.makePlane('height', json);
T.heightDab(seam, -19200 + 256 * 64, -19200 + 256 * 64, {mode: 'raise', radius: 640, amount: 5}, null);
ok(seam.dirty.size === 4, `a dab on the corner of four tiles dirties all four (${seam.dirty.size})`);
const peak = T.heightAt(Hp, -19200 + 300 * 64, -19200 + 100 * 64);
ok(Math.abs(peak - (-550)) < 1e-3, 'the centre post rose by the amount: ' + peak);
const redo = T.swapTiles(Hp, before);
ok(T.heightAt(Hp, -19200 + 300 * 64, -19200 + 100 * 64) === -600 && Hp.tiles.size === 0, 'undo puts the plane back (no tile left in memory)');
T.swapTiles(Hp, redo);
ok(Math.abs(T.heightAt(Hp, -19200 + 300 * 64, -19200 + 100 * 64) + 550) < 1e-3, 'redo raises it again');

// the save payload: only dirty tiles; an all-default dirty tile is a delete
let pay = T.dirtyPayload(Hp);
ok(pay.length === Hp.dirty.size && pay.every(e => e.bytes && e.bytes.length === 131104), `payload = ${pay.length} dirty tiles, all bytes`);
T.markSaved(Hp, pay);
ok(Hp.dirty.size === 0 && Hp.onDisk.size === pay.length, 'after the save nothing is dirty and the tiles are on disk');
const b2 = new Map();
T.heightDab(Hp, -19200 + 300 * 64, -19200 + 100 * 64, {mode: 'set', radius: 4000, target: -600, k: 1}, b2);
pay = T.dirtyPayload(Hp);
ok(pay.length > 0 && pay.every(e => e.bytes === null), 'setting the hill back to the default makes the save a DELETE of its tiles');

// biome brush: the cell under the cursor at least, never a blocked tile
const bb = new Map();
T.biomeDab(Bp, 0, 0, 1, 2, bb);
ok(T.biomeAt(Bp, 10, 10) === 2 && bb.size === 1 && bb.get(0) === null, 'a 0.1 m biome dab paints the cell under it (tile was absent)');
const L = T.makePlane('height', json);
L.onDisk.add(T.tileKey(0, 0));
const rr = T.heightDab(L, -19200 + 10 * 64, -19200 + 10 * 64, {mode: 'raise', radius: 300, amount: 10}, new Map());
ok(rr.changed === 0 && rr.skipped > 0 && !L.tiles.size, `a listed-but-unloaded tile is never written (${rr.skipped} samples skipped)`);

// smooth pulls a spike down; mip + range
const S = T.makePlane('height', json);
T.heightDab(S, 0, 0, {mode: 'raise', radius: 64, amount: 1000}, null);
const h0 = T.heightAt(S, -19200 + 300 * 64, -19200 + 300 * 64);
T.heightDab(S, -19200 + 300 * 64, -19200 + 300 * 64, {mode: 'smooth', radius: 300, k: 1}, null);
ok(T.heightAt(S, -19200 + 300 * 64, -19200 + 300 * 64) < h0, 'smooth pulls a spike down');
const [lo, hi] = T.planeRange(S);
ok(lo === -600 && hi > -600, `plane range ${lo}..${hi.toFixed(1)}`);
const m3 = T.mipLevel(S, T.tileKey(1, 1), 3);
ok(m3.length === 32 * 32, 'mip level 3 is 32x32');

console.log(fail ? `test_maptiles: ${fail} FAILED` : 'test_maptiles: OK');
process.exit(fail ? 1 : 0);
