/* shot_tree.mjs — render a generated tree to a PNG, from Node, with no browser.
 *
 * WHY THIS EXISTS. Tree work is a LOOK problem, and every proxy for "does it
 * look right" lies: voxel counts do not distinguish a crown of readable lobes
 * from a solid ball at the same fill fraction, and a run-length total does not
 * distinguish a chewed rim from noise. The tuner's Trees tab is the real
 * authoring view, but it needs a browser and a human; this is the one that can
 * be looked at from a terminal in half a second, and it is what the presets
 * were tuned against.
 *
 *   node scripts/shot_tree.mjs oak                     # -> build/tree_oak.png
 *   node scripts/shot_tree.mjs oak 1 --out x.png       # variant 1
 *   node scripts/shot_tree.mjs --all                   # every species, contact sheet
 *
 * It renders the SAME cells the atlas bakes (generateTree's output), lit by a
 * plain sun so the baked shade ramp is visible as itself rather than hidden
 * under the engine's tonemap.
 */
import { readFileSync, writeFileSync, mkdirSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';
import { deflateSync } from 'zlib';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const T = await import(pathToFileURL(join(ROOT, 'assets/editor/treegen.js')).href);
const MATS = JSON.parse(readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8'));

// name -> [ [r,g,b] x3 ] palette, exactly the three colours the engine's
// `paletteColor` picks between on the state nibble.
const PAL = new Map();
MATS.materials.forEach(m => {
  const hex = s => { const v = parseInt(String(s).replace('#', ''), 16);
                     return [(v >> 16) & 255, (v >> 8) & 255, v & 255]; };
  PAL.set(m.id, [hex(m.colors[0]), hex(m.colors[1] || m.colors[0]),
                 hex(m.colors[2] || m.colors[0])]);
});

// ---------------------------------------------------------------- PNG writer
const CRC = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) { let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c; }
  return t;
})();
function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 255] ^ (c >>> 8);
  return ~c >>> 0;
}
function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'ascii');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
function writePNG(path, w, h, rgb) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0))]));
}

// ------------------------------------------------------------------ renderer
const SUN = (() => { const v = [0.45, 0.78, 0.44];
  const l = Math.hypot(...v); return v.map(x => x / l); })();

/** DDA a ray through the cell grid; return the first non-empty cell + face. */
function trace(cells, nx, ny, nz, o, d) {
  // clip to the box
  let t0 = 0, t1 = 1e9;
  const dim = [nx, ny, nz];
  for (let a = 0; a < 3; a++) {
    if (Math.abs(d[a]) < 1e-9) { if (o[a] < 0 || o[a] > dim[a]) return null; continue; }
    let ta = (0 - o[a]) / d[a], tb = (dim[a] - o[a]) / d[a];
    if (ta > tb) { const s = ta; ta = tb; tb = s; }
    t0 = Math.max(t0, ta); t1 = Math.min(t1, tb);
  }
  if (t1 <= t0) return null;
  let t = t0 + 1e-4;
  let x = Math.floor(o[0] + d[0] * t), y = Math.floor(o[1] + d[1] * t),
      z = Math.floor(o[2] + d[2] * t);
  const st = [d[0] > 0 ? 1 : -1, d[1] > 0 ? 1 : -1, d[2] > 0 ? 1 : -1];
  const td = [Math.abs(1 / d[0]), Math.abs(1 / d[1]), Math.abs(1 / d[2])];
  const tm = [0, 0, 0];
  const p = [o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t];
  for (let a = 0; a < 3; a++) {
    const c = [x, y, z][a];
    tm[a] = Math.abs(d[a]) < 1e-9 ? 1e9
        : (d[a] > 0 ? (c + 1 - p[a]) / d[a] : (p[a] - c) / -d[a]) + t;
  }
  let face = 0;
  for (let i = 0; i < 3000; i++) {
    if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) {
      if (t > t1) return null;
    } else {
      const w = cells[(z * ny + y) * nx + x];
      if (w) return { w: w, face: face, t: t };
    }
    if (tm[0] < tm[1] && tm[0] < tm[2]) { x += st[0]; t = tm[0]; tm[0] += td[0]; face = 0; }
    else if (tm[1] < tm[2]) { y += st[1]; t = tm[1]; tm[1] += td[1]; face = 1; }
    else { z += st[2]; t = tm[2]; tm[2] += td[2]; face = 2; }
    if (t > t1 + 1) return null;
  }
  return null;
}

export function renderTree(res, W, H, yaw) {
  const { x: nx, y: ny, z: nz } = res.dim;
  const cells = res.cells;
  const c = [nx / 2, ny / 2, nz / 2];
  const radius = Math.max(nx, ny, nz) * 0.72;
  const ya = yaw === undefined ? 0.7 : yaw;
  const eye = [c[0] + Math.sin(ya) * radius * 2.2, c[1] + radius * 0.55,
               c[2] + Math.cos(ya) * radius * 2.2];
  const fwd = (() => { const v = [c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]];
    const l = Math.hypot(...v); return v.map(x => x / l); })();
  const right = (() => { const v = [-fwd[2], 0, fwd[0]];
    const l = Math.hypot(...v) || 1; return v.map(x => x / l); })();
  // up = right x fwd, in that order. The other order puts the sky at the
  // bottom of the frame, which reads as "the generator makes upside-down
  // trees" and costs a debugging session.
  const up = [right[1] * fwd[2] - right[2] * fwd[1],
              right[2] * fwd[0] - right[0] * fwd[2],
              right[0] * fwd[1] - right[1] * fwd[0]];
  const half = radius * 1.05;

  const img = Buffer.alloc(W * H * 3);
  const FACE_SHADE = [0.72, 1.0, 0.85];
  for (let py = 0; py < H; py++) {
    const v = (0.5 - (py + 0.5) / H) * 2 * half;
    for (let px = 0; px < W; px++) {
      const u = ((px + 0.5) / W - 0.5) * 2 * half * (W / H);
      const o = [eye[0] + right[0] * u + up[0] * v,
                 eye[1] + right[1] * u + up[1] * v,
                 eye[2] + right[2] * u + up[2] * v];
      const hit = trace(cells, nx, ny, nz, o, fwd);
      const i = (py * W + px) * 3;
      if (!hit) {
        // A flat backdrop, deliberately mid-grey: a dark one hides the shade
        // ramp's dark tier and a white one hides the lit tier.
        const g = 44 + Math.round(26 * (1 - py / H));
        img[i] = g; img[i + 1] = g + 4; img[i + 2] = g + 10;
        continue;
      }
      const mat = hit.w & 0xFFF, state = (hit.w >> 12) & 15;
      const nm = res.names[mat - 1];
      const pal = PAL.get(nm) || [[255, 0, 255], [255, 0, 255], [255, 0, 255]];
      const col = pal[state % 3];
      const k = FACE_SHADE[hit.face] * (0.55 + 0.45 * SUN[1]);
      for (let ch = 0; ch < 3; ch++)
        img[i + ch] = Math.min(255, Math.round(col[ch] * k));
    }
  }
  return img;
}

// ---------------------------------------------------------------------- main
const args = process.argv.slice(2);
mkdirSync(join(ROOT, 'build'), { recursive: true });
const DIR = join(ROOT, 'assets', 'trees');
const loadParams = n => JSON.parse(readFileSync(join(DIR, n + '.json'), 'utf8'));

if (args.includes('--all')) {
  const names = readdirSync(DIR).filter(f => f.endsWith('.json'))
      .map(f => f.slice(0, -5)).sort();
  const CW = 300, CH = 360, cols = 5;
  const rows = Math.ceil(names.length / cols);
  const W = CW * cols, H = CH * rows;
  const sheet = Buffer.alloc(W * H * 3, 30);
  names.forEach((n, i) => {
    const res = T.generateTree(loadParams(n), 0);
    const tile = renderTree(res, CW, CH, 0.7);
    const cx = (i % cols) * CW, cy = Math.floor(i / cols) * CH;
    for (let y = 0; y < CH; y++)
      tile.copy(sheet, ((cy + y) * W + cx) * 3, y * CW * 3, (y + 1) * CW * 3);
    console.log(n.padEnd(12), res.dim.x + 'x' + res.dim.y + 'x' + res.dim.z,
                'leaf', res.meta.leafCount, 'wood', res.meta.woodCount);
  });
  const out = args[args.indexOf('--all') + 1] &&
      !args[args.indexOf('--all') + 1].startsWith('--')
      ? args[args.indexOf('--all') + 1] : join(ROOT, 'build', 'trees_all.png');
  writePNG(out, W, H, sheet);
  console.log('wrote ' + out);
} else {
  const name = args[0] || 'oak';
  const seed = parseInt(args[1], 10) || 0;
  const oi = args.indexOf('--out');
  const out = oi >= 0 ? args[oi + 1] : join(ROOT, 'build', 'tree_' + name + '.png');
  const res = T.generateTree(loadParams(name), seed);
  console.log(name, res.dim.x + 'x' + res.dim.y + 'x' + res.dim.z,
              'leaf', res.meta.leafCount, 'wood', res.meta.woodCount,
              'stems', res.meta.stems, 'clumps', res.meta.clumps);
  writePNG(out, 620, 760, renderTree(res, 620, 760, 0.7));
  console.log('wrote ' + out);
}
