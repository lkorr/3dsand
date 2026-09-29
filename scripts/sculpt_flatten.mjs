/* sculpt_flatten.mjs -- FLATTEN a rectangle of a map's ground, headless.
 *
 * The World map page's sculpt brush (Environment -> World map -> sculpt,
 * mode "flatten") is the human surface for this; this is the same edit as a
 * command, for the places a brush stroke is the wrong tool: a whole clearing
 * to level for a village, an exact height, a record in a script of what was
 * done. It writes the SAME file the page does --
 * assets/worldmap/<map>/sculpt.svsculpt, through the page's own
 * parseSculpt / serializeSculpt (assets/editor/map.js) -- so the page opens
 * the result and a person keeps brushing on top of it.
 *
 *   node scripts/sculpt_flatten.mjs <map> --rect x0,z0,x1,z1 --y <ground top>
 *        [--feather 96] [--strength 1] [--passes 2] [--keep-grain 0] [--dry]
 *
 *   --rect      world voxels, inclusive: the area brought to the height
 *   --y         the ground TOP the area is brought to (a house placed on it
 *               has its floor, its ref's pos.y, one above)
 *   --feather   voxels outside the rect over which the pull fades to nothing
 *               (smoothstep), so the edit meets the untouched ground softly
 *   --strength  0..1: 1 = exactly flat; 0.8 keeps a fifth of the old swells
 *   --passes    heightmap -> correct -> repeat: the sediment wedge and the
 *               houses' own pad ramps react to the new slope, so a second
 *               pass takes out what the first could not see (default 2)
 *   --keep-grain R  level the ground's AVERAGE over +-R voxels instead of every
 *               sample column (0, the default: exactly flat at the samples).
 *               The layer is one sample every 8 voxels, so an exact flatten
 *               cancels the ground's fine grain AT the samples only and leaves
 *               its residue between them: a plane of one- and two-voxel steps
 *               every few voxels, dirt showing on every riser -- measured on
 *               Harrowby's clearing, where it read as churned ground. With R
 *               (24 is a good start) the correction is smooth, the tilt and the
 *               swells go, and the ground keeps the same fine texture as the
 *               untouched forest round it.
 *   --dry       report what would change, write nothing
 *
 * HOW. The ground the engine generates is procedural + the sculpt offset
 * (worldgen.wgsl landAt; src/sim/worldmap.h "THE SCULPT LAYER"), one offset
 * sample every 8 voxels, bilinear between. Each pass asks the ENGINE for the
 * heights (`sandvox --heightmap`, CPU only, the map's current layer in) at
 * exactly the sample columns and moves each sample by (y - h) * w. The heights
 * are read WITHOUT the map's houses (SANDVOX_HEIGHTMAP_BARE): a house levels
 * its own pad to its floor, so reading through it would leave the ground under
 * it unsculpted, and its pad's ramp would then drop into a ditch at the edge.
 * Flattening before or after placing the houses gives the same layer.
 *
 * MOVES THE WORLD HASH of that map (the ground changed). The harness map is
 * never touched by the game's content, so no gate moves unless it names the
 * map.
 */
import { readFileSync, writeFileSync, existsSync, unlinkSync, mkdirSync } from 'fs';
import { spawnSync } from 'child_process';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const MAP = await import(pathToFileURL(join(ROOT, 'assets/editor/map.js')).href);

const args = process.argv.slice(2);
const opt = (f, d) => { const i = args.indexOf(f); return i >= 0 ? args[i + 1] : d; };
const map = args[0];
if (!map || map.startsWith('--') || !opt('--rect') || opt('--y') === undefined) {
  console.error('usage: node scripts/sculpt_flatten.mjs <map> --rect x0,z0,x1,z1 --y <ground top> ' +
                '[--feather 96] [--strength 1] [--passes 2] [--keep-grain 0] [--dry]');
  process.exit(2);
}
const [x0, z0, x1, z1] = opt('--rect').split(',').map(Number);
const Y = Number(opt('--y'));
const feather = Math.max(0, Number(opt('--feather', 96)));
const strength = Math.min(1, Math.max(0, Number(opt('--strength', 1))));
const grainK = Math.max(0, Math.round(Number(opt('--keep-grain', 0)) / 8));   // the window, in samples
const passes = Math.max(1, Number(opt('--passes', 2)) | 0);
const dry = args.includes('--dry');
if (![x0, z0, x1, z1, Y].every(Number.isFinite) || x1 < x0 || z1 < z0) {
  console.error('bad --rect / --y'); process.exit(2);
}
const exe = join(ROOT, 'build/Release/sandvox.exe');
if (!existsSync(exe)) { console.error('no build/Release/sandvox.exe: build first (bash scripts/build.sh)'); process.exit(2); }
const file = join(ROOT, 'assets/worldmap', map, 'sculpt.svsculpt');
if (!existsSync(join(ROOT, 'assets/worldmap', map, 'map.json'))) { console.error('no map "' + map + '"'); process.exit(2); }

const SP = 8, T = 32;
const key = (tx, tz) => (tx + 32768) * 65536 + (tz + 32768);
const load = () => existsSync(file) ? MAP.parseSculpt(new Uint8Array(readFileSync(file)).buffer) : new Map();
let tiles = load();
const get = (sx, sz) => { const t = tiles.get(key(sx >> 5, sz >> 5)); return t ? t[(sz & 31) * T + (sx & 31)] : 0; };
const set = (sx, sz, v) => {
  const k = key(sx >> 5, sz >> 5);
  let t = tiles.get(k);
  if (!t) { t = new Float32Array(T * T); tiles.set(k, t); }
  t[(sz & 31) * T + (sx & 31)] = v;
};

// The sample columns the edit touches (multiples of 8), the rect grown by the feather.
const sx0 = Math.floor((x0 - feather) / SP), sx1 = Math.ceil((x1 + feather) / SP);
const sz0 = Math.floor((z0 - feather) / SP), sz1 = Math.ceil((z1 + feather) / SP);
const n = Math.max(sx1 - sx0 + 1, sz1 - sz0 + 1);
// --heightmap samples column i at cx - span/2 + (i + 0.5) * span/res; with
// span = 8 * res that is a multiple of 8 when cx - span/2 = 8 * sx0 - 4.
const res = Math.min(1024, n + (n & 1));
const span = SP * res;
const cx = SP * sx0 - 4 + span / 2, cz = SP * sz0 - 4 + span / 2;
const weight = (x, z) => {
  const dx = Math.max(x0 - x, x - x1, 0), dz = Math.max(z0 - z, z - z1, 0);
  const d = Math.sqrt(dx * dx + dz * dz);
  if (d <= 0) return strength;
  if (feather <= 0 || d >= feather) return 0;
  const u = 1 - d / feather;
  return strength * u * u * (3 - 2 * u);
};

const hmPath = join(ROOT, 'build', 'sculpt_flatten_heights.bin');
mkdirSync(dirname(hmPath), {recursive: true});
for (let pass = 1; pass <= passes; pass++) {
  // The heights with the layer as it now stands (a pass's own writes land
  // on disk first, so the engine sees them).
  const r = spawnSync('bash', [join(ROOT, 'scripts/run.sh'), exe, '--heightmap', `${cx},${cz},${span},${res}`,
                               '--heightmap-out', hmPath],
                      {cwd: ROOT, env: {...process.env, SANDVOX_MAP: map, SANDVOX_HEIGHTMAP_BARE: '1', SANDVOX_NO_CRASH_DIALOG: '1'},
                       encoding: 'utf8'});
  if (r.status !== 0 || !existsSync(hmPath)) { console.error('--heightmap failed:\n' + r.stdout + r.stderr); process.exit(1); }
  const b = readFileSync(hmPath), d = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (d.getUint32(0, true) !== 0x4D485653 || d.getUint32(8, true) !== res) { console.error('unexpected heightmap'); process.exit(1); }
  let moved = 0, worst = 0, sum = 0, cnt = 0;
  // --keep-grain: every sample's height averaged over the +-grainK samples
  // round it (clipped at the read grid's edge, which the feather covers).
  const hAt = (i, j) => d.getInt32(48 + (j * res + i) * 8, true);
  const hSmooth = (i, j) => {
    if (grainK <= 0) return hAt(i, j);
    let a = 0, n = 0;
    for (let jj = Math.max(0, j - grainK); jj <= Math.min(res - 1, j + grainK); jj++)
      for (let ii = Math.max(0, i - grainK); ii <= Math.min(res - 1, i + grainK); ii++) { a += hAt(ii, jj); n++; }
    return a / n;
  };
  for (let j = 0; j < res; j++) {
    const sz = sz0 + j;
    if (sz > sz1) break;
    for (let i = 0; i < res; i++) {
      const sx = sx0 + i;
      if (sx > sx1) break;
      const w = weight(sx * SP, sz * SP);
      if (w <= 0) continue;
      const h = hSmooth(i, j);
      const dy = (Y - h) * w;
      if (Math.abs(dy) >= 0.5) moved++;
      worst = Math.max(worst, Math.abs(Y - h) * (w >= strength ? 1 : 0));
      if (w >= strength) { sum += Math.abs(Y - h); cnt++; }
      set(sx, sz, get(sx, sz) + dy);
    }
  }
  console.log(`pass ${pass}: ${moved} samples moved; inside the rect the ground was off by ` +
              `${cnt ? (sum / cnt).toFixed(1) : 0} voxels on average, ${worst} at worst`);
  if (dry) break;
  const bytes = MAP.serializeSculpt(tiles);
  if (bytes) writeFileSync(file, bytes); else if (existsSync(file)) unlinkSync(file);
  tiles = load();   // what the file holds (rounded to whole voxels) is the truth
}
console.log(dry ? 'dry run: nothing written' : `wrote ${file}`);
