/* breed.js — the Characters page: a POOL, a TWEAK pane and a LITTER grid.
 *
 * INTERACTIVE EVOLUTION, and that is the whole algorithm: a human is the
 * fitness function. There is no scoring, no tournament and no generation
 * counter — you roll a litter, keep what looks like a person, and breed from
 * it. Everything hard is in the generator (assets/editor/mobgen.js) and in
 * this page's job of showing you thirty bodies fast enough to judge them.
 *
 * WHY THE THUMBNAILS ARE 2D CANVAS AND NOT three.js
 * -------------------------------------------------
 * A litter is N rigged bodies at ~30k voxels each. N live three.js scenes is N
 * WebGL contexts, N render loops and a browser that gives up around 16
 * contexts; one shared scene re-posed per thumbnail is a frame of work per
 * cell and a page that stutters while you are trying to compare faces.
 *
 * So a thumbnail is a SOFTWARE VOXEL PAINTER: a dense byte grid, three flat
 * shades picked by which face is exposed, and one fillRect per visible cell.
 * No context, no lighting, ~3 ms a body. The cost of not having real lighting
 * is a flat three-tone shade, which at this resolution is what the art is
 * anyway (see shadeBack in mobgen.js).
 *
 * IT PAINTS THE SHIPPED LATTICE, not the authored one. It used to halve every
 * coordinate back to the 4-micro lattice the builders work in, and that was
 * wrong for one specific and fatal reason: the SHOULDER ROUND lives between
 * the two lattices. See the banner on drawBody. The version of this page that
 * shipped the round drew rounded and unrounded bodies as literally the same
 * picture.
 *
 * TWO CACHES, and they are not the same cache. `buildCache` stops a body being
 * GENERATED twice (22 ms); `thumbCache` stops it being PAINTED twice (3 ms),
 * which matters because render() rebuilds the page for any state change at all
 * and there can be 48 litter cells on it. This banner claimed the second one
 * existed from the day it was written, and it did not.
 *
 * WHAT A SAVE WRITES, and why there are two buttons
 * -------------------------------------------------
 *   Save character  -> assets/mobs/<name>.vox + .json, through /api/model.
 *                      A new body. 200 KB of art that stops tracking the human,
 *                      including the anatomy baked into its interior.
 *   Save recolour   -> assets/mobs/<name>.json ONLY, with `extends` + a
 *                      `palette` filter and no art at all (zombie.json is the
 *                      shipped example). Keeps tracking its base for ever.
 *
 * The recolour path is the right one whenever the SHAPE is unchanged, and it is
 * deliberately limited to what the engine can actually do: `palette` is a
 * GLOBAL filter (saturation / brightness / tint), so it cannot say "red hair,
 * unchanged skin". Per-feature colour lives in the `.col` models inside the
 * .vox, and a def cannot override only those — so a per-feature recolour is a
 * new body, and the page says so rather than pretending otherwise.
 *
 * ANATOMY IS BAKED BY THE SAVE, in the browser. A body straight out of the
 * generator is solid `skin` at every depth: no flesh, no muscle, no blood, no
 * bone, no skull. It walks and it burns, but a sword through it finds skin all
 * the way down and every wound material the gore system is authored against is
 * absent — so an un-baked body is not a finished character, and a page that
 * wrote one would be a page that cannot finish the job it exists for.
 * `anatomy.js` is a pure module and the Models tab's Apply-recipe button
 * already runs it here, so there was never anything CLI-only about the step;
 * `mobgen.bakeAnatomy` is the same call `scripts/gen_mobs.mjs` makes, and
 * `scripts/anatomize_mob.mjs` remains the way to re-bake a mob already on disk.
 *
 * NOTHING ON THIS PAGE NEEDS A TERMINAL. Roll, tweak, breed, save — the asset
 * that lands in assets/mobs/ is finished and the game will spawn it as it is.
 */

'use strict';

import * as mg from './mobgen.js';
import * as SC from './sidecar.js';

let H = null;              // host hooks
let root = null;           // our section element
let built = false;

// ---- state ------------------------------------------------------------------
const S = {
  genome: mg.defaultGenome(),
  name: 'newcomer',
  locks: new Set(),        // gene paths held fixed by mutate/cross/randomize
  pool: [],                // [{name, genome|null, kind, extendsName}]
  parents: [],             // names or genomes chosen as breeding stock
  litter: [],              // [{genome, seed, label, canvas}]
  litterCount: 12,
  sigma: 0.6,
  seed: 1,
  source: 'mutate',        // 'random' | 'mutate' | 'cross'
  baseFor: '',             // the def a colour variant extends
  status: '',
  dirty: false,            // mirrors the host pill, so mayDiscard can read it
  variant: { saturation: 1, brightness: 1, tint: '#b9c3ae', tintAmount: 0 },
  // The tweak preview's camera. Lives here, not on the canvas, because render()
  // rebuilds the pane on any state change and the view must survive that.
  // Filled on first use: ORBIT_HOME is declared further down this module.
  view: null,
};

const el = (...a) => H.el(...a);
/** The host's unsaved-work flag, so closing the tab on a character you spent
 *  ten minutes on asks first. Set by every edit to the TWEAK pane's genome;
 *  cleared by a save. A litter cell is not dirt -- it is one `roll` away from
 *  existing again, and its seed is printed on it. */
const setDirty = d => { S.dirty = !!d; if (H.onDirty) H.onDirty(d); };
/** THE ONE PLACE A GENE IS WRITTEN. Every edit site goes through it -- a slider,
 *  a colour picker, a checkbox, a style pick, the test hook -- so the dirty
 *  flag cannot be forgotten by a new control, which is the only way a flag like
 *  this ever goes wrong. */
function gset(path, v) { mg.setPath(S.genome, path, v); setDirty(true); }
const toast = (...a) => H.toast(...a);

// =============================================================================
// the software voxel painter
// =============================================================================

/**
 * ONE DENSE GRID over the whole figure in SHIPPED scene coords, holding
 * `art slot - 127` so 0 is empty. Parts overlap at the joints; later parts
 * win, exactly as the .vox dedup does. Cached on the body, which buildOf
 * already caches, so the orbit view's per-frame redraw never rebuilds it.
 *
 * It is a typed array and not the string-keyed Map this used to be because
 * the Map is what made the shipped lattice look expensive: eight times the
 * cells meant eight times the `x+','+y+','+z` allocations, four of them per
 * cell for the neighbour tests, plus a 24k-entry sort of arrays parsed back
 * out of those same strings. An integer index costs none of that, and the
 * painting ORDER falls out of the loop nesting, so the sort goes away too.
 * Measured on the default figure: 24 ms a body to 3 ms, i.e. cheaper than
 * the old Map was at HALF the resolution.
 *
 * The grid is sized from the part boxes, which is a superset of the occupied
 * cells and needs no pass over them; the TIGHT extent, which is what the
 * figure is scaled to fit, is tracked while filling.
 */
function gridOf(b) {
  if (b._grid !== undefined) return b._grid;
  let gx0 = 1e9, gx1 = -1e9, gy0 = 1e9, gy1 = -1e9, gz0 = 1e9, gz1 = -1e9;
  for (const p of b.parts) {
    gx0 = Math.min(gx0, p.mn[0]); gx1 = Math.max(gx1, p.mn[0] + p.size[0]);
    gy0 = Math.min(gy0, p.mn[1]); gy1 = Math.max(gy1, p.mn[1] + p.size[1]);
    gz0 = Math.min(gz0, p.mn[2]); gz1 = Math.max(gz1, p.mn[2] + p.size[2]);
  }
  let G = null;
  if (gx1 > gx0) {
    const dx = gx1 - gx0, dy = gy1 - gy0, dz = gz1 - gz0;
    const grid = new Uint8Array(dx * dy * dz);
    const at = (x, y, z) =>
      (x < gx0 || x >= gx1 || y < gy0 || y >= gy1 || z < gz0 || z >= gz1)
        ? 0 : grid[(x - gx0) + dx * ((y - gy0) + dy * (z - gz0))];
    let mnx = 1e9, mxx = -1e9, mnz = 1e9, mxz = -1e9, mny = 1e9, mxy = -1e9;
    let any = false;
    for (const p of b.parts) {
      for (const [x, y, z, c] of p.cells) {
        const ax = p.mn[0] + x, ay = p.mn[1] + y, az = p.mn[2] + z;
        // The art palette is 128..255 and 0 is empty here, so the slot is
        // stored biased by -127 and fits a byte with room to spare.
        grid[(ax - gx0) + dx * ((ay - gy0) + dy * (az - gz0))] = c - 127;
        if (ax < mnx) mnx = ax; if (ax > mxx) mxx = ax;
        if (ay < mny) mny = ay; if (ay > mxy) mxy = ay;
        if (az < mnz) mnz = az; if (az > mxz) mxz = az;
        any = true;
      }
    }
    if (any) G = { grid, gx0, gy0, gz0, dx, dy, dz, at,
                   mnx, mxx, mny, mxy, mnz, mxz };
  }
  // Non-enumerable, so a body handed to JSON.stringify or a sidecar diff never
  // grows a 100 KB grid.
  Object.defineProperty(b, '_grid', { value: G, configurable: true });
  return G;
}

/** Art slot -> [r,g,b], indexed by the SAME biased byte the grid holds,
 *  straight out of the genome so the preview and the file agree by
 *  construction (both read genome.colors through COLOR_SLOTS). Shade/light
 *  slots carry their base's colour: the painters' own face shading is the
 *  depth cue. */
function slotRGB(genome) {
  const out = [];
  for (const [k, slot] of Object.entries(mg.COLOR_SLOTS)) {
    const n = parseInt((genome.colors[k] || '#ff00ff').slice(1), 16);
    out[slot - 127] = [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  for (const [base, deps] of Object.entries(mg.COLOR_FAMILIES))
    for (const d of deps)
      out[mg.COLOR_SLOTS[d] - 127] = out[mg.COLOR_SLOTS[base] - 127];
  return out;
}

/** The camera the tweak pane's preview starts at, and returns to on a
 *  double-click: roughly the three-quarter view the fixed painter below draws
 *  (in front, a little to the figure's left, a little above). */
export const ORBIT_HOME = Object.freeze({ yaw: -0.42, pitch: 0.22, zoom: 1,
                                          panX: 0, panY: 0 });

// The six face directions: outward normal, the offset from the cell's own
// (x,y,z) to the face's min corner, and the two unit edges spanning the face.
const FACES = [
  { n: [ 1, 0, 0], o: [1, 0, 0], a: [0, 1, 0], b: [0, 0, 1] },
  { n: [-1, 0, 0], o: [0, 0, 0], a: [0, 1, 0], b: [0, 0, 1] },
  { n: [ 0, 1, 0], o: [0, 1, 0], a: [1, 0, 0], b: [0, 0, 1] },
  { n: [ 0,-1, 0], o: [0, 0, 0], a: [1, 0, 0], b: [0, 0, 1] },
  { n: [ 0, 0, 1], o: [0, 0, 1], a: [1, 0, 0], b: [0, 1, 0] },
  { n: [ 0, 0,-1], o: [0, 0, 0], a: [1, 0, 0], b: [0, 1, 0] },
];

/** Every exposed face of the figure as flat (x, y, z, dir, slot) quintuples.
 *  View-independent, so it is built once per body and a drag only projects. */
function exposedFaces(b) {
  if (b._faces !== undefined) return b._faces;
  const G = gridOf(b);
  const out = [];
  if (G) {
    const { grid, gx0, gy0, gz0, dx, dy, dz, at } = G;
    for (let z = 0; z < dz; z++)
      for (let y = 0; y < dy; y++)
        for (let x = 0; x < dx; x++) {
          const v = grid[x + dx * (y + dy * z)];
          if (!v) continue;
          const X = x + gx0, Y = y + gy0, Z = z + gz0;
          for (let f = 0; f < 6; f++) {
            const n = FACES[f].n;
            if (!at(X + n[0], Y + n[1], Z + n[2])) out.push(X, Y, Z, f, v);
          }
        }
  }
  const faces = new Int32Array(out);
  Object.defineProperty(b, '_faces', { value: faces, configurable: true });
  return faces;
}

/**
 * Draw a built character from ANY angle: the tweak pane's orbit preview.
 *
 * Orthographic, with a z-buffer, into ImageData. The fixed painter below gets
 * away with no depth buffer because its loop order IS back-to-front for its one
 * camera; a free camera has no such order. Each visible face is a
 * parallelogram whose two screen-space edges are the SAME for every face of
 * that direction (orthographic), so the inverse that turns a pixel into face
 * (u,v) is solved six times per frame, not once per face.
 *
 * Still a 2D canvas, deliberately: the page's headless check reads pixels back
 * with getImageData on a machine with no working GL.
 *
 * Light is fixed to the WORLD (above, in front, off the figure's left), so
 * turning the figure shows its form turning under the light rather than a lamp
 * that follows the camera.
 */
export function drawOrbit(canvas, b, view = ORBIT_HOME) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width, Hh = canvas.height;
  ctx.clearRect(0, 0, W, Hh);
  if (!b || !b.parts) return;
  const G = gridOf(b);
  if (!G) return;
  const faces = exposedFaces(b);

  // Camera basis. yaw turns about +z; 0 looks along +y at the face (which is
  // toward LOW y) with the figure's +x on screen right, as the fixed painter
  // has it. pitch > 0 raises the camera to look down.
  const cy = Math.cos(view.yaw), sy = Math.sin(view.yaw);
  const cp = Math.cos(view.pitch), sp = Math.sin(view.pitch);
  const R = [cy, sy, 0];
  const F = [-sy * cp, cy * cp, -sp];                 // into the scene
  const U = [-sy * sp, cy * sp, cp];
  const dot = (a, v) => a[0] * v[0] + a[1] * v[1] + a[2] * v[2];

  // A zoom-1 scale that fits the figure from EVERY yaw, so turning it does not
  // breathe in and out: the horizontal extent is the footprint's diagonal.
  const ex = G.mxx - G.mnx + 1, ey = G.mxy - G.mny + 1, ez = G.mxz - G.mnz + 1;
  const foot = Math.hypot(ex, ey), pad = 4;
  const s0 = Math.min((W - pad * 2) / foot,
                      (Hh - pad * 2) / (ez * cp + foot * Math.abs(sp)));
  const s = s0 * view.zoom;
  const c = [(G.mnx + G.mxx + 1) / 2, (G.mny + G.mxy + 1) / 2,
             (G.mnz + G.mxz + 1) / 2];
  const ox = W / 2 + view.panX - dot(c, R) * s;
  const oy = Hh / 2 + view.panY + dot(c, U) * s;

  const L = [0.35, -0.55, 0.76], Ln = Math.hypot(...L);
  const rgb = slotRGB(b.genome);

  // Per direction: null if it faces away, else its screen edges, their
  // determinant, the pixel box of one face relative to its min corner, and
  // its brightness.
  const dirs = FACES.map(fc => {
    if (dot(fc.n, F) >= -1e-6) return null;
    const ax = dot(fc.a, R) * s, ay = -dot(fc.a, U) * s;
    const bx = dot(fc.b, R) * s, by = -dot(fc.b, U) * s;
    return {
      fc, ax, ay, bx, by, det: ax * by - ay * bx,
      x0: Math.min(0, ax, bx, ax + bx), x1: Math.max(0, ax, bx, ax + bx),
      y0: Math.min(0, ay, by, ay + by), y1: Math.max(0, ay, by, ay + by),
      lit: 0.5 + 0.5 * Math.max(0, dot(fc.n, L) / Ln),
    };
  });

  const img = ctx.createImageData(W, Hh);
  const px = img.data;
  const zb = new Float32Array(W * Hh).fill(Infinity);
  const E = 0.02;                          // seam guard between faces, in (u,v)
  const plot = (k, depth, r, g, bl) => {
    if (depth >= zb[k]) return;
    zb[k] = depth;
    const o = k * 4;
    px[o] = r; px[o + 1] = g; px[o + 2] = bl; px[o + 3] = 255;
  };
  for (let i = 0; i < faces.length; i += 5) {
    const d = dirs[faces[i + 3]];
    if (!d) continue;
    const fc = d.fc;
    const X = faces[i] + fc.o[0], Y = faces[i + 1] + fc.o[1];
    const Z = faces[i + 2] + fc.o[2];
    const sx = ox + (X * R[0] + Y * R[1]) * s;
    const sy0 = oy - (X * U[0] + Y * U[1] + Z * U[2]) * s;
    // Depth of the CELL centre, not of the face: every face of one cell ties,
    // and between cells the nearer centre wins, which is exactly the
    // occlusion a grid of cubes has under an orthographic camera.
    const depth = (faces[i] + 0.5) * F[0] + (faces[i + 1] + 0.5) * F[1] +
                  (faces[i + 2] + 0.5) * F[2];
    const col = rgb[faces[i + 4]] || [136, 136, 136];
    const r = col[0] * d.lit, g = col[1] * d.lit, bl = col[2] * d.lit;
    const X0 = Math.max(0, Math.floor(sx + d.x0));
    const X1 = Math.min(W - 1, Math.ceil(sx + d.x1));
    const Y0 = Math.max(0, Math.floor(sy0 + d.y0));
    const Y1 = Math.min(Hh - 1, Math.ceil(sy0 + d.y1));
    let hit = false;
    for (let py = Y0; py <= Y1; py++) {
      const qy = py + 0.5 - sy0;
      for (let qx0 = X0; qx0 <= X1; qx0++) {
        const qx = qx0 + 0.5 - sx;
        const u = (qx * d.by - qy * d.bx) / d.det;
        const v = (d.ax * qy - d.ay * qx) / d.det;
        if (u < -E || u > 1 + E || v < -E || v > 1 + E) continue;
        hit = true;
        plot(py * W + qx0, depth, r, g, bl);
      }
    }
    // Zoomed out, a face can fall between pixel centres. It still owns the
    // pixel under its middle, or the figure would dissolve into holes.
    if (!hit) {
      const mx = Math.floor(sx + (d.ax + d.bx) / 2);
      const my = Math.floor(sy0 + (d.ay + d.by) / 2);
      if (mx >= 0 && mx < W && my >= 0 && my < Hh)
        plot(my * W + mx, depth, r, g, bl);
    }
  }
  ctx.putImageData(img, 0, 0);
}

/**
 * Draw a built character into a 2D canvas.
 *
 * ORTHOGRAPHIC THREE-QUARTER VIEW. The figure is authored Z-up with its face
 * toward LOW y (every builder mirrors in y at the end — see flipY), so the
 * camera sits in front, to the left and above: screen x takes +x and a slice of
 * +y, screen y takes -z and a slice of +y. Cells are painted back to front, so
 * no depth buffer is needed.
 *
 * Shading is by which face is exposed, not by a light: top brightest, front
 * next, side darkest. That matches the art's own region-based shading rule and
 * costs one neighbour lookup.
 *
 * THE LATTICE IS THE SHIPPED ONE, and that is not a detail. This painter used
 * to halve every coordinate back to the authored 4-micro lattice for speed,
 * which is a defensible trade right up until something exists only BETWEEN the
 * two — and the shoulder round does. It runs after the 2x upscale (it has to:
 * the authored lattice has no cross-section between "square" and "rounded
 * square"), it removes 160 shipped voxels from the default figure, and every
 * one of them had surviving siblings in its own authored cell. Measured: the
 * halved painter drew the rounded body and the unrounded body as the SAME
 * PICTURE, 3,102 cells either way, zero difference. A preview that cannot show
 * the sculpting is not a preview of the character that gets saved.
 *
 * The cost is real and was measured rather than feared: ~24 ms a body at the
 * shipped lattice against ~6 ms halved, next to ~22 ms to GENERATE the body in
 * the first place. That is why `thumbCache` below exists — the fix for eight
 * repaints is to stop repainting, not to draw the wrong thing quickly.
 */
export function drawBody(canvas, b, opts = {}) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width, Hh = canvas.height;
  ctx.clearRect(0, 0, W, Hh);
  if (!b) return;

  const G = gridOf(b);
  if (!G) return;
  const { grid, gx0, gy0, gz0, dx, dy, at, mnx, mxx, mny, mxy, mnz, mxz } = G;

  const SKEW_X = 0.45, SKEW_Y = 0.30;
  const spanX = (mxx - mnx + 1) + (mxy - mny + 1) * SKEW_X;
  const spanY = (mxz - mnz + 1) + (mxy - mny + 1) * SKEW_Y;
  const pad = 2;
  const s = Math.min((W - pad * 2) / spanX, (Hh - pad * 2) / spanY);
  const ox = (W - spanX * s) * 0.5 - mnx * s + (mxy - mny) * SKEW_X * s;
  const oy = Hh - pad + mnz * s;
  const px = (x, y) => ox + (x - (y - mny) * SKEW_X) * s;
  const py = (y, z) => oy - (z + (y - mny) * SKEW_Y) * s;

  // Art slot -> css colour, straight out of the genome so the preview and the
  // file agree by construction (both read genome.colors through COLOR_SLOTS).
  // Indexed by the SAME biased byte the grid holds, and pre-shaded three ways,
  // so the inner loop never parses a hex string or builds an rgb() — that was
  // one string allocation per drawn voxel.
  const face = [[], [], []];              // 0 = top, 1 = front, 2 = side
  slotRGB(b.genome).forEach((rgb, i) => {
    if (!rgb) return;
    const sh = f => {
      const c = v => Math.max(0, Math.min(255, Math.round(v * f)));
      return `rgb(${c(rgb[0])},${c(rgb[1])},${c(rgb[2])})`;
    };
    face[0][i] = sh(1.0);
    face[1][i] = sh(0.84);
    face[2][i] = sh(0.66);
  });

  // BACK TO FRONT, and the loop order IS the painter's order: high y is behind
  // (the face is at low y) and within a depth row a lower cell is behind a
  // higher one. No depth buffer and, now, no sort.
  //
  // A CELL IS THE GAP BETWEEN ITS OWN TWO EDGES, rounded — not a fixed square.
  // The old form was `ceil(s) + 1` on both axes, which at the shipped lattice's
  // sub-pixel scales (a pool thumbnail is 52 px for a 112-cell figure, so
  // s ~= 0.46) drew every cell four times its size: the figure came out as a
  // blob with the silhouette dilated by two pixels all round, which is exactly
  // the detail this painter was changed to show. Rounding both edges and taking
  // the difference is seam-free at every scale for free, because adjacent cells
  // round their shared edge to the same pixel by construction.
  // RUNS ALONG X, NOT CELLS. Screen x is monotonic in lattice x within a row,
  // so a stretch of cells with the same colour and the same exposed face is
  // ONE rectangle. This is not a micro-optimisation: `fillRect` is the unit of
  // work a software rasteriser charges for, and moving to the shipped lattice
  // took the default figure from ~900 rects to 7,414. The headless check went
  // from passing in minutes to not finishing, which is how the cost was found
  // -- an earlier benchmark had "measured" the painter at 3 ms a body against
  // a stubbed fillRect, i.e. it measured everything except the drawing.
  //
  // A torso row is thirty-odd cells of one slot, so the runs are long and the
  // rect count comes back under what the authored lattice cost while the
  // picture stays at the finer one.
  let fill = '';
  for (let y = mxy; y >= mny; y--) {
    const yBase = dx * (y - gy0);
    for (let z = mnz; z <= mxz; z++) {
      const row = yBase + dx * dy * (z - gz0) - gx0;
      // runF < 0 is "no run open". NOT runX < 0: scene x is centred on the
      // spine, so every run on the figure's right half starts at a negative x,
      // and testing runX dropped that whole half of the body from the picture.
      let runX = 0, runV = 0, runF = -1;
      const flush = xEnd => {
        if (runF < 0) return;
        const col = face[runF][runV] || '#888';
        if (col !== fill) { ctx.fillStyle = fill = col; }
        const x0 = Math.round(px(runX, y)), x1 = Math.round(px(xEnd, y));
        const y1 = Math.round(py(y, z)), y0 = Math.round(py(y, z + 1));
        ctx.fillRect(x0, y0, Math.max(1, x1 - x0), Math.max(1, y1 - y0));
        runF = -1;
      };
      for (let x = mnx; x <= mxx; x++) {
        const v = grid[row + x];
        // A face test is three lookups; skipping it for an interior cell whose
        // neighbours in z and y are both filled is most of the grid.
        const f = !v ? -1
                : !at(x, y, z + 1) ? 0
                : !at(x, y - 1, z) ? 1
                : (!at(x + 1, y, z) || !at(x - 1, y, z)) ? 2
                : -1;
        if (f < 0) { flush(x); continue; }         // empty or interior
        if (runF >= 0 && v === runV && f === runF) continue;
        flush(x);
        runX = x; runV = v; runF = f;
      }
      flush(mxx + 1);
    }
  }
  if (opts.label) {
    // Sized off the canvas, not pinned at 9 px: a litter cell's backing store
    // is twice its CSS box, so a fixed font would come out half-height on the
    // one surface that uses labels at all.
    const bar = Math.max(10, Math.round(Hh * 0.095));
    ctx.fillStyle = 'rgba(0,0,0,.55)';
    ctx.fillRect(0, Hh - bar, W, bar);
    ctx.fillStyle = '#cfc8b8';
    ctx.font = Math.round(bar * 0.72) + 'px monospace';
    ctx.fillText(opts.label, Math.round(bar * 0.3), Hh - Math.round(bar * 0.25));
  }
}

// =============================================================================
// build + validate, with a cache
//
// A litter cell is only rebuilt when its genome actually changes. Keyed on the
// genome's own JSON, which is cheap next to generating a body and is the only
// key that cannot go stale.
// =============================================================================

const buildCache = new Map();

function buildOf(genome, name) {
  const g = mg.normalizeGenome(genome);
  const k = JSON.stringify(g);
  const hit = buildCache.get(k);
  if (hit) return hit;
  let out;
  try {
    out = mg.generateMob(g, 0, {
      materials: H.materials(),
      player: (H.tuning && H.tuning() && H.tuning().player) || undefined,
      name: name || g.name,
    });
    out.complaints = mg.validateMob(out);
  } catch (e) {
    out = { error: e.message || String(e), genome: g, parts: [], complaints: [] };
  }
  out.key = k;                       // what thumbFor caches the PIXELS against
  if (buildCache.size > 240) buildCache.clear();        // bounded, like everything
  buildCache.set(k, out);
  return out;
}

// =============================================================================
// the thumbnail cache
//
// buildCache stops a body being GENERATED twice. This stops it being PAINTED
// twice, which is a separate and, since the painter moved to the shipped
// lattice, comparable cost: render() rebuilds the whole page for any state
// change at all -- ticking a lock box, dropping a parent, selecting a pool row
// -- and there are up to 48 litter cells plus the pool on it. The banner at the
// top of this file claimed this cache existed from the day it was written; it
// did not, and every render repainted every cell.
//
// It caches PIXELS, not canvases. Handing the same <canvas> element to two
// call sites looks like it works until two litter cells hold the same genome
// (sigma 0 is a clone litter, and it is the first thing anyone tries): a DOM
// node can only be in one place, so one of the two cells silently goes blank.
// An offscreen source blitted into a fresh canvas has no such rule.
// =============================================================================

const thumbCache = new Map();

/** Paint `genome` into `canvas`, reusing earlier pixels when nothing that
 *  affects them has changed. `view` is 'front' or 'side'. */
function thumbFor(canvas, genome, name, view, label) {
  const b = buildOf(genome, name);
  const k = view + '|' + (label || '') + '|' +
            canvas.width + 'x' + canvas.height + '|' + b.key;
  let off = thumbCache.get(k);
  if (!off) {
    off = document.createElement('canvas');
    off.width = canvas.width; off.height = canvas.height;
    if (view === 'side') drawSide(off, b); else drawBody(off, b, { label });
    if (thumbCache.size > 400) thumbCache.clear();
    thumbCache.set(k, off);
  }
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.drawImage(off, 0, 0);
  return b;
}

// =============================================================================
// the pool: what is already saved
// =============================================================================

async function loadPool() {
  S.pool = [];
  let list;
  try {
    const r = await fetch('/api/models');
    list = await r.json();
  } catch (e) {
    S.status = 'no tuner server: the pool and saving are unavailable ' +
               '(run `python scripts/tuner_server.py`)';
    return;
  }
  const mobs = (list.files || []).filter(f => f.dir === 'mobs' &&
                                              f.name.endsWith('.json'));
  for (const f of mobs) {
    const stem = f.name.slice(0, -5);
    let j = null;
    try {
      j = await (await fetch('/api/model?path=mobs/' + f.name)).json();
    } catch { continue; }
    // behaviors.json and attack_styles.json live here too and are not mobs.
    if (!j || (!j.limbs && !j.extends)) continue;
    S.pool.push({
      name: stem,
      genome: j.genome ? mg.normalizeGenome(j.genome) : null,
      extendsName: j.extends || '',
      hasPalette: !!j.palette,
      bytes: f.size,
    });
  }
  S.pool.sort((a, b) => (b.genome ? 1 : 0) - (a.genome ? 1 : 0) ||
                        a.name.localeCompare(b.name));
}

// =============================================================================
// saving
// =============================================================================

const NAME_OK = /^[a-z0-9_]{1,40}$/;

/** The base every character extends, resolved off the server. Fetched fresh on
 *  each save rather than cached for the session: the Models tab may have edited
 *  the human in another window, and a diff against a stale base writes
 *  overrides for numbers that are no longer different. */
async function resolvedBase() {
  const fetched = new Map();
  const get = async (dir, stem) => {
    const key = dir + '/' + stem;
    if (fetched.has(key)) return fetched.get(key);
    const r = await fetch('/api/model?path=' + encodeURIComponent(key + '.json'),
                          { cache: 'no-store' });
    fetched.set(key, r.ok ? JSON.parse(await r.text()) : null);
    return fetched.get(key);
  };
  const walk = async (stem, depth) => {
    if (depth > SC.MAX_EXTENDS) return;
    const j = await get('mobs', stem);
    if (!j) throw new Error(`assets/mobs/${stem}.json did not load — a ` +
                            'character is a diff against its base, so the ' +
                            'base has to be there to diff against');
    for (const nm of Array.isArray(j.effects) ? j.effects : [])
      if (typeof nm === 'string') await get('mobs/effects', nm);
    if (typeof j.extends === 'string' && j.extends)
      await walk(j.extends, depth + 1);
  };
  await walk(mg.BASE_MOB, 0);
  return SC.resolveSidecar(stem => fetched.get('mobs/' + stem), mg.BASE_MOB,
                           nm => fetched.get('mobs/effects/' + nm) ?? null);
}

async function postModel(rel, body, isJson) {
  const r = await fetch('/api/model?path=' + encodeURIComponent(rel), {
    method: 'POST',
    headers: { 'Content-Type': isJson ? 'application/json' : 'model/x-vox' },
    body,
  });
  const j = await r.json().catch(() => ({ ok: false, error: r.status }));
  if (!j.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}

async function saveCharacter() {
  const name = S.name.trim().toLowerCase();
  if (!NAME_OK.test(name))
    return toast('a mob name is lowercase letters, digits and underscores', true);
  const b = buildOf(S.genome, name);
  if (b.error) return toast('cannot build: ' + b.error, true);
  if (b.complaints.length)
    return toast('refused: ' + b.complaints[0], true);
  const existing = S.pool.find(p => p.name === name);
  if (existing && !confirm(`assets/mobs/${name}.{vox,json} already exists. ` +
                           'Overwrite?')) return;
  // THE ANATOMY IS BAKED HERE, not left to a command line. A body straight out
  // of the generator is solid `skin` at every depth: no flesh, no muscle, no
  // blood, no bone, no skull. It walks and it burns, but a sword through it
  // finds skin all the way down and the gore system's wound materials are
  // absent — so an un-baked body is not a finished character, and a page that
  // saved one would be a page that cannot finish the job it exists for.
  // anatomy.js runs in the browser (the Models tab's Apply recipe already does
  // this), and mobgen.bakeAnatomy is the same call scripts/gen_mobs.mjs makes.
  let baked;
  try {
    baked = mg.bakeAnatomy(b, H.materials());
  } catch (e) {
    return toast('anatomy bake refused: ' + e.message, true);
  }
  // THE SIDECAR THAT IS WRITTEN IS THE DIFFERENCE, not the body. `b.sidecar`
  // is the whole resolved creature; the file states only what this character
  // makes different from the base it extends, so the rig keeps tracking the
  // human instead of freezing a copy of it on the day the character was born
  // (mobgen.thinSidecar, and src/game/sidecar.h for what "different" means).
  // The base is fetched rather than assumed, because the diff is against what
  // is on disk right now.
  let doc;
  try {
    doc = mg.thinSidecar(b.sidecar, await resolvedBase(), name);
  } catch (e) {
    return toast('refused: ' + e.message, true);
  }
  try {
    await postModel('mobs/' + name + '.vox', b.vox, false);
    await postModel('mobs/' + name + '.json',
                    JSON.stringify(doc, null, 2) + '\n', true);
  } catch (e) {
    return toast('save failed: ' + e.message, true);
  }
  const census = Object.entries(baked.census).sort((x, y) => y[1] - x[1])
    .map(([m, n]) => `${m} ${n}`).join(', ');
  toast(`saved ${name}.vox + ${name}.json, anatomy baked (${census})`);
  S.status = `saved ${name}: ${b.vox.length} bytes of art, anatomy baked — ` +
             census;
  // The build cache holds the object we just mutated, so the next save would
  // bake an already-baked body (a no-op that still rewrites the file) and the
  // preview would be reading a body whose interior is no longer skin. Drop it,
  // and the pixels drawn from it with it.
  buildCache.clear();
  thumbCache.clear();
  setDirty(false);
  await loadPool();
  render();
}

async function saveVariant() {
  const name = S.name.trim().toLowerCase();
  if (!NAME_OK.test(name))
    return toast('a mob name is lowercase letters, digits and underscores', true);
  const base = S.baseFor;
  if (!base) return toast('pick a base def for the variant first', true);
  const v = S.variant || {};
  const pal = {};
  if (v.saturation !== undefined && v.saturation !== 1)
    pal.saturation = round3(v.saturation);
  if (v.brightness !== undefined && v.brightness !== 1)
    pal.brightness = round3(v.brightness);
  if (v.tintAmount) { pal.tint = v.tint || '#b9c3ae';
                      pal.tintAmount = round3(v.tintAmount); }
  if (!Object.keys(pal).length)
    return toast('a variant with no palette filter is just a rename', true);
  const doc = {
    '//': `A COLOUR VARIANT OF ${base}. No .vox: \`extends\` pulls in ` +
          `${base}.json's whole rig (limbs, chains, clips, states, sockets, ` +
          'anatomy) and `model` follows it to the same art, so this creature ' +
          'keeps tracking its base as the base is edited — including what is ' +
          'under its skin, which is BAKED into the .vox and would silently ' +
          "diverge in a copy. Written by the tuner's Characters page.",
    extends: base,
    '//palette': 'A FILTER over the base art palette, not a colour table. ' +
                 'GLOBAL by construction: it cannot say "red hair, unchanged ' +
                 'skin" — for that, save a character instead.',
    palette: pal,
  };
  try {
    await postModel('mobs/' + name + '.json',
                    JSON.stringify(doc, null, 2) + '\n', true);
  } catch (e) {
    return toast('save failed: ' + e.message, true);
  }
  toast(`saved ${name}.json — a ${Object.keys(pal).join('/')} variant of ` +
        `${base}, no art`);
  setDirty(false);
  await loadPool();
  render();
}

const round3 = v => Math.round(v * 1000) / 1000;

// =============================================================================
// the litter
// =============================================================================

function parentGenomes() {
  return S.parents.map(p => typeof p === 'string'
    ? (S.pool.find(x => x.name === p) || {}).genome
    : p).filter(Boolean);
}

function rollLitter() {
  const ps = parentGenomes();
  const locks = S.locks;
  S.litter = [];
  for (let i = 0; i < S.litterCount; i++) {
    const rng = mg.makeRng(S.seed * 4096 + i);
    let g, label;
    if (S.source === 'random') {
      g = mg.rollColors(mg.randomGenome(rng, locks), rng);
      // A locked gene must survive the colour roll too, or the lock only holds
      // for half the page.
      g = reapplyLocks(g, S.genome, locks);
      label = 'roll ' + (i + 1);
    } else if (S.source === 'cross' && ps.length >= 2) {
      g = mg.cross(ps, rng, { sigma: S.sigma * 0.5, locks });
      label = 'cross ' + (i + 1);
    } else {
      g = mg.mutate(ps[0] || S.genome, S.sigma, rng, locks);
      label = 'mutant ' + (i + 1);
    }
    S.litter.push({ genome: g, label, seed: S.seed * 4096 + i });
  }
}

/** Put every locked gene back to the value the TWEAK pane holds. `mutate` and
 *  `cross` honour locks themselves; `randomGenome` starts from the default, so
 *  a lock there has to mean "keep what I have", not "keep the default". */
function reapplyLocks(g, from, locks) {
  for (const path of locks) mg.setPath(g, path, mg.getPath(from, path));
  return mg.normalizeGenome(g);
}

// =============================================================================
// DOM
// =============================================================================

function css() {
  if (document.getElementById('breed-css')) return;
  const s = document.createElement('style');
  s.id = 'breed-css';
  s.textContent = `
#view-characters .bintro{font-size:11px;line-height:1.5;opacity:.8;
  margin:0 0 9px;max-width:90em}
#view-characters .bintro code{opacity:.75}
#view-characters .bwrap{display:grid;grid-template-columns:210px 1fr 1fr;gap:10px}
#view-characters h3{margin:0 0 6px;font-size:12px;letter-spacing:.08em;
  text-transform:uppercase;opacity:.75}
#view-characters .bcol{min-width:0}
#view-characters .pool{display:flex;flex-direction:column;gap:4px;
  max-height:70vh;overflow:auto}
#view-characters .prow{display:flex;gap:6px;align-items:center;padding:3px;
  border:1px solid #2a2a2a;background:#161616;cursor:pointer}
#view-characters .prow:hover{border-color:#5a5a4a}
#view-characters .prow.sel{border-color:#9a8a4a;background:#20201a}
#view-characters .prow canvas{background:#0e0e0e;image-rendering:pixelated}
#view-characters .prow .pn{font-size:11px;line-height:1.3;min-width:0;
  overflow:hidden;text-overflow:ellipsis}
#view-characters .prow .pk{font-size:9px;opacity:.55}
#view-characters .litter{display:grid;
  grid-template-columns:repeat(auto-fill,minmax(84px,1fr));gap:6px;
  max-height:62vh;overflow:auto}
#view-characters .lcell{border:1px solid #2a2a2a;background:#141414;
  cursor:pointer;position:relative}
#view-characters .lcell:hover{border-color:#5a5a4a}
#view-characters .lcell.bad{border-color:#7a3030}
#view-characters .lcell canvas{display:block;width:100%;height:auto;
  image-rendering:pixelated}
#view-characters .lbtns{display:flex;gap:2px;padding:2px}
#view-characters .lbtns button{flex:1;font-size:9px;padding:1px 0}
#view-characters .genes{max-height:56vh;overflow:auto;padding-right:4px}
#view-characters .ggroup{margin:0 0 8px}
#view-characters .genes .gh{font-size:10px;opacity:.75;color:#cbbf95;
  text-transform:uppercase;letter-spacing:.08em;margin:12px 0 2px;
  border-top:1px solid #2a2a2a;padding-top:7px}
#view-characters .genes .gh:first-child{margin-top:0;border-top:0;padding-top:0}
/* The group's one line of orientation, and each row's one line of it. Same
   voice, same size, different scope -- read the first once, read the second
   when a slider surprises you. */
#view-characters .gnote{font-size:10px;opacity:.55;line-height:1.45;
  margin:0 0 6px;max-width:46em}
#view-characters .gwrap{padding:2px 0}
#view-characters .ghint{font-size:10px;opacity:0;line-height:1.4;
  margin:0 0 0 20px;max-width:44em;height:0;overflow:hidden;
  transition:opacity .08s}
/* The hint costs no height until you are on the row. Forty sliders each with a
   permanent second line is a pane nobody scrolls to the bottom of; forty
   sliders that explain themselves under the pointer is the same pane that
   teaches. The :focus-within rule is the keyboard half of the same thing --
   tab to a slider and you get the sentence too.
   NOTE this whole block is a TEMPLATE LITERAL: no backticks in here. */
#view-characters .gwrap:hover .ghint,
#view-characters .gwrap:focus-within .ghint{opacity:.72;height:auto;
  margin-bottom:3px}
#view-characters .grow{display:grid;grid-template-columns:14px 104px 1fr 54px;
  gap:6px;align-items:center;font-size:11px;padding:1px 0}
#view-characters .grow input[type=range]{width:100%}
#view-characters .gwrap:hover .grow{background:#1a1a18}
#view-characters .grow .gl{overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
#view-characters .grow .gu{opacity:.45}
#view-characters .grow .gv{text-align:right;opacity:.7;font-variant:tabular-nums}
#view-characters .grow.locked{opacity:.55}
#view-characters .grow.locked .gl{text-decoration:underline dotted #9a8a4a}
/* A tone row is indented under the base it follows, so the five colours read
   as five and not as nine. */
#view-characters .grow.tone .gl{padding-left:9px;opacity:.85}
#view-characters .prev{display:flex;gap:8px;align-items:flex-start}
#view-characters .prev canvas{background:#0d0d0f;image-rendering:pixelated;
  border:1px solid #2a2a2a}
#view-characters .bad{color:#e08080}
#view-characters .note{font-size:10px;opacity:.6;line-height:1.4;margin:4px 0}
#view-characters .brow{display:flex;gap:6px;align-items:center;
  flex-wrap:wrap;margin:4px 0}
`;
  document.head.append(s);
}

/** A canvas `w`x`h` CSS pixels with a `ss`-times finer backing store. The
 *  figure is drawn at the shipped lattice now, which is ~112 cells tall against
 *  a 230 px preview -- a 1:1 store throws away half the detail the round exists
 *  to show, and doubling the store costs one blit. */
function canvasFor(w, h, ss = 1) {
  const c = document.createElement('canvas');
  c.width = w * ss; c.height = h * ss;
  c.style.width = w + 'px'; c.style.height = h + 'px';
  return c;
}

/** Ask before throwing away unsaved work on the tweak pane. Loading a pool
 *  row, promoting a litter cell and picking a preset all REPLACE the genome
 *  the sliders hold, and all three used to do it silently -- ten minutes of
 *  dragging gone to a stray click, with the dirty pill the only warning and
 *  nothing to undo it with. */
function mayDiscard(what) {
  if (!S.dirty) return true;
  return confirm(`"${S.name}" has unsaved edits.\n\n${what} and lose them?`);
}

function render() {
  if (!root) return;
  root.textContent = '';
  // The one line that says what the three columns are FOR. The page is a loop
  // -- roll, judge, breed, save -- and nothing on screen said so; you were
  // left to infer it from a pane called "Litter".
  root.append(el('div', { class: 'bintro' },
    el('b', {}, 'Make a person by picking one.'), ' ',
    'Roll a litter on the right, keep whichever child looks like somebody, ',
    'make it a parent and roll again. Use the middle column when you want to ',
    'move one thing by hand. Saving writes a finished character into ',
    el('code', {}, 'assets/mobs/'), ' that the game will spawn as it is.'));
  root.append(el('div', { class: 'bwrap' },
    poolPane(), tweakPane(), litterPane()));
  if (S.status)
    root.append(el('div', { class: 'note' }, S.status));
}

// ---- pool -------------------------------------------------------------------

function poolPane() {
  const rows = [];
  for (const p of S.pool) {
    const c = canvasFor(40, 60);
    if (p.genome) thumbFor(c, p.genome, p.name, 'front');
    else {
      const ctx = c.getContext('2d');
      ctx.fillStyle = '#222'; ctx.fillRect(0, 0, c.width, c.height);
      ctx.fillStyle = '#666'; ctx.font = '8px monospace';
      ctx.fillText('no', 12, 28); ctx.fillText('genome', 2, 38);
    }
    const kind = p.extendsName ? 'variant of ' + p.extendsName
               : p.genome ? 'generated' : 'hand-authored';
    const row = el('div', {
      class: 'prow' + (S.name === p.name ? ' sel' : ''),
      title: p.genome
        ? 'load ' + p.name + ' into the sliders'
        : p.name + ' was drawn by hand, not generated, so it carries no ' +
          'settings to load. It can still be the base for a recolour.',
      onclick: () => {
        if (p.genome) {
          if (!mayDiscard(`Load ${p.name}`)) return;
          S.genome = mg.normalizeGenome(p.genome);
          S.name = p.name;
          S.baseFor = p.name;
          setDirty(false);
          S.status = `loaded ${p.name} into the sliders — saving under this ` +
                     'name overwrites it, so rename first to make a relative.';
        } else {
          S.baseFor = p.name;
          S.status = `${p.name} has no genome block, so it cannot be loaded ` +
                     'into the sliders. Selected as the BASE for a colour ' +
                     'variant instead.';
        }
        render();
      },
    }, c, el('div', { class: 'pn' }, p.name,
             el('div', { class: 'pk' }, kind)));
    rows.push(row);
  }
  return el('div', { class: 'bcol' },
    el('h3', { title: 'every character already saved in assets/mobs' },
       'saved characters'),
    el('div', { class: 'brow' },
      el('button', { onclick: async () => { await loadPool(); render(); } },
         'refresh'),
      el('button', {
        title: 'use the character in the Tweak pane as a parent for the ' +
               'next litter',
        onclick: () => {
          const g = mg.normalizeGenome(S.genome);
          if (S.parents.length >= 4) S.parents.shift();
          S.parents.push(g);
          render();
        },
      }, '+ parent')),
    el('div', { class: 'note' },
      'Everything in assets/mobs. Click one to load it into the sliders. ',
      'The ones marked hand-authored were drawn in the model editor before ',
      'this page existed, so there are no settings to load — pick one of ',
      'those as the thing a recolour points at instead.'),
    el('div', { class: 'pool' }, rows.length ? rows
      : el('div', { class: 'note' }, 'nothing loaded')));
}

// ---- tweak ------------------------------------------------------------------

/** Drag / wheel / pan on the tweak preview. Writes S.view and repaints only
 *  this canvas, at most once a frame: a drag must not rebuild the page. */
function orbitControls(canvas) {
  let drag = null, queued = false;
  const redraw = () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      drawOrbit(canvas, buildOf(S.genome, S.name), S.view);
    });
  };
  // Pointer deltas arrive in CSS pixels; pan is in backing-store pixels.
  const ratio = () => canvas.width / canvas.getBoundingClientRect().width;
  canvas.addEventListener('pointerdown', e => {
    drag = { x: e.clientX, y: e.clientY,
             pan: e.button !== 0 || e.shiftKey };
    canvas.setPointerCapture(e.pointerId);
    canvas.style.cursor = drag.pan ? 'move' : 'grabbing';
    e.preventDefault();
  });
  canvas.addEventListener('pointermove', e => {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.x = e.clientX; drag.y = e.clientY;
    const v = S.view;
    if (drag.pan) {
      const r = ratio();
      v.panX += dx * r; v.panY += dy * r;
    } else {
      v.yaw -= dx * 0.012;
      v.pitch = Math.max(-1.45, Math.min(1.45, v.pitch + dy * 0.012));
    }
    redraw();
  });
  const end = () => { drag = null; canvas.style.cursor = 'grab'; };
  canvas.addEventListener('pointerup', end);
  canvas.addEventListener('pointercancel', end);
  canvas.addEventListener('contextmenu', e => e.preventDefault());
  // Zoom TOWARDS THE POINTER: the scene point under it stays under it, so
  // zooming onto a face needs no pan afterwards.
  canvas.addEventListener('wheel', e => {
    e.preventDefault();
    const v = S.view;
    const z = Math.max(0.5, Math.min(16, v.zoom * Math.exp(-e.deltaY * 0.0015)));
    const k = z / v.zoom;
    const rc = canvas.getBoundingClientRect(), r = canvas.width / rc.width;
    const mx = (e.clientX - rc.left) * r - canvas.width / 2;
    const my = (e.clientY - rc.top) * r - canvas.height / 2;
    v.panX = mx - (mx - v.panX) * k;
    v.panY = my - (my - v.panY) * k;
    v.zoom = z;
    redraw();
  }, { passive: false });
  canvas.addEventListener('dblclick', () => {
    S.view = { ...ORBIT_HOME };
    redraw();
  });
}

function tweakPane() {
  const big = canvasFor(150, 230, 2);
  big.title = 'drag to turn · wheel to zoom (towards the pointer) · ' +
              'right- or shift-drag to pan · double-click to reset';
  big.style.cursor = 'grab';
  big.style.touchAction = 'none';
  if (!S.view) S.view = { ...ORBIT_HOME };
  const b = buildOf(S.genome, S.name);
  drawOrbit(big, b, S.view);
  orbitControls(big);
  const side = canvasFor(90, 230, 2);
  side.title = 'side view — depth, the jaw, the skull set-back and the hair ' +
               'sweep, none of which read from the front at this resolution';
  thumbFor(side, S.genome, S.name, 'side');

  const geneRows = [];
  let group = null;
  for (const spec of mg.GENE_SPECS) {
    if (spec.group !== group) {
      group = spec.group;
      const g = mg.GENE_GROUPS.find(x => x.key === group);
      geneRows.push(el('div', { class: 'gh' }, (g && g.title) || group));
      if (g) geneRows.push(el('div', { class: 'gnote' }, g.note));
    }
    geneRows.push(geneRow(spec));
  }

  const presetSel = el('select', {
    title: 'load a worked example — a build the generator can make, to start ' +
           'from rather than to keep',
    onchange: e => {
      if (!e.target.value) return;
      if (!mayDiscard('Load the ' + e.target.value + ' preset')) {
        e.target.value = '';
        return;
      }
      S.genome = mg.presetGenome(e.target.value);
      S.name = e.target.value === 'human' ? 'newcomer' : e.target.value;
      setDirty(false);
      render();
    },
  }, el('option', { value: '' }, 'preset…'),
     mg.PRESET_ORDER.map(k => el('option', { value: k }, k)));

  const hairSel = el('select', {
    onchange: e => {
      mg.applyHairStyle(S.genome, e.target.value);
      setDirty(true);
      render();
    },
  }, mg.HAIR_STYLE_ORDER.map(k =>
       el('option', { value: k, selected: S.genome.hair.style === k }, k)));

  return el('div', { class: 'bcol' },
    el('h3', { title: 'the one character the sliders hold, and the thing a ' +
                      'save writes' }, 'this character'),
    el('div', { class: 'prev' }, big, side,
      el('div', {},
        el('div', { class: 'brow' },
          el('input', { value: S.name, size: 12, placeholder: 'name',
                        title: 'the filename this saves as: ' +
                               'assets/mobs/<name>.vox and .json. Lowercase ' +
                               'letters, digits and underscores.',
                        oninput: e => { S.name = e.target.value; } }),
          presetSel),
        el('div', { class: 'brow' },
          el('span', { title: 'a starting point for the four hair settings ' +
                              'further down; drag them afterwards and you ' +
                              'are off the preset, which is fine' }, 'hair'),
          hairSel,
          el('button', {
            title: 'picks a complexion, a hair colour and a cloth colour that ' +
                   'go together, then varies them a little — the three skin ' +
                   'tones stay related, which is the part that is easy to get ' +
                   'wrong by hand',
            onclick: () => {
              mg.rollColors(S.genome, mg.makeRng((Math.random() * 1e9) | 0));
              setDirty(true);
              render();
            },
          }, 'roll colours')),
        el('div', { class: 'bstats' }, statLines(b)),
        el('div', { class: 'brow' },
          el('button', { title: 'writes assets/mobs/<name>.vox and .json, ' +
                                'with the anatomy baked in — a finished ' +
                                'character the game will spawn as it is',
                         onclick: saveCharacter },
             'save character (.vox + .json)'),
          el('button', {
            title: 'pin every setting, so the next litter varies nothing. ' +
                   'Then un-pin the few you want to explore.',
            onclick: () => {
              S.locks = new Set(mg.GENE_SPECS.map(s => s.path));
              render();
            },
          }, 'pin all'),
          el('button', { title: 'un-pin everything, so a litter varies the ' +
                                'whole character',
                         onclick: () => { S.locks = new Set(); render(); } },
             'un-pin all')),
        variantRow())),
    el('div', { class: 'genes' }, geneRows));
}

/** The three lines under the preview: what this body IS, what the shoulder
 *  round did to it, and whether it passed its own structural checks. Factored
 *  out of tweakPane because a slider drag has to refresh them without
 *  rebuilding the page — they moved with the figure and the old page left them
 *  showing the height you started at until you let go of the mouse. */
function statLines(b) {
  const st = b.stats || {};
  const sc = b.sidecar;
  const complaints = b.error ? [b.error] : (b.complaints || []);
  return [
    el('div', { class: 'note' },
      `${st.worldH ?? '?'} voxels tall in the world · `,
      `${st.painted ?? '?'} voxels of art · `,
      `stands ${sc ? sc.gait.rideHeight : '?'} off the ground · `,
      `one walking stride ${sc ? sc.clips.walk.durationMs : '?'} ms`),
    // THE SHOULDER ROUND, said out loud. It is a rule that fires inside the
    // generator, it is the whole difference between a generated body and the
    // hand-sculpted shipped human, and until the painter moved to the shipped
    // lattice you could not see it here at all. A line naming the count is how
    // you tell the rule ran on THIS character rather than trusting that it ran
    // on some character once.
    el('div', { class: 'note',
                title: 'The shipped human had its shoulders rounded by hand ' +
                       'in the model editor: a one-voxel fin off the torso ' +
                       'where the silhouette flares toward the shoulder, and ' +
                       'the four sharp corners off the top of each upper arm. ' +
                       'The generator reproduces both as rules, so a ' +
                       'generated character needs no hand work.' },
      st.rounded
        ? `shoulders rounded: ${st.rounded} voxels off the torso and the ` +
          'upper arms'
        : 'shoulders: nothing to round on this build'),
    complaints.length
      ? el('div', { class: 'bad note' }, 'REFUSED: ' + complaints.join(' | '))
      : el('div', { class: 'note' },
          'checks pass: every joint sits inside the limb it moves, the eyes ',
          'are on the face, and no part is in two pieces'),
  ];
}

function geneRow(spec) {
  const locked = S.locks.has(spec.path);
  const v = mg.getPath(S.genome, spec.path);
  const lock = el('input', {
    type: 'checkbox', checked: locked,
    title: 'PIN this one. A pinned setting does not move when you roll a ' +
           'litter, so you can vary the face and hold the body still.',
    onchange: e => {
      if (e.target.checked) S.locks.add(spec.path); else S.locks.delete(spec.path);
      render();
    },
  });
  let input, readout = null;
  if (spec.kind === 'color') {
    input = el('input', { type: 'color', value: v,
      oninput: e => { gset(spec.path, e.target.value); renderPreviewOnly(); } });
    readout = el('span', { class: 'gv' }, v);
  } else if (spec.kind === 'bool') {
    input = el('input', { type: 'checkbox', checked: !!v,
      onchange: e => { gset(spec.path, e.target.checked); render(); } });
  } else if (spec.kind === 'enum') {
    input = el('select', {
      onchange: e => { gset(spec.path, e.target.value);
                       if (spec.path === 'hair.style')
                         mg.applyHairStyle(S.genome, e.target.value);
                       render(); },
    }, spec.choices.map(c => el('option', { value: c, selected: c === v }, c)));
  } else {
    // `uiMax` narrows the TRACK without narrowing the gene. hair.back and
    // hair.front run to 99 because that is how `bald` is spelled -- a hairline
    // above the crown -- so ninety of their ninety-seven units are the same
    // bald head and a slider spanning them is dead travel with every real
    // hairstyle crushed into the first tenth of it. The track stops at the
    // tallest head that exists and `bald` is reached by picking it, which is
    // where it belonged.
    const hi = spec.uiMax ?? spec.max;
    input = el('input', {
      type: 'range', min: spec.min, max: hi,
      step: spec.step ?? ((hi - spec.min) / 100),
      value: Math.min(v, hi),
      oninput: e => {
        gset(spec.path, Number(e.target.value));
        // A slider drag must not rebuild the whole page (the pool alone is 30
        // bodies), so the preview and this row's readout update in place.
        readout.textContent = fmt(Number(e.target.value), spec);
        renderPreviewOnly();
      },
    });
    readout = el('span', { class: 'gv' }, fmt(v, spec));
  }
  // The HINT is the row's tooltip and also the line under it when the pane is
  // wide enough for one (see .ghint in the CSS). Both, not either: the tooltip
  // is what you get when you are hunting and the line is what you get when you
  // are reading, and a dense stack of forty sliders needs both modes.
  const label = el('span', { class: 'gl' }, spec.label,
                   spec.unit ? el('span', { class: 'gu' }, ' ' + spec.unit)
                             : null,
                   spec.tone ? el('span', { class: 'gu' }, ' ↳') : null);
  // A TONE ROW carries the extra sentence, because "why did this move when I
  // rolled" is the question it would otherwise leave you with.
  const hint = spec.tone
    ? spec.hint + ' Rolls move it with the ' + spec.tone +
      ' above, keeping whatever relationship you leave it at — so the back ' +
      'of a figure is never a different colour from its front. Pin it to ' +
      'hold it still.'
    : spec.hint;
  const row = el('div', { class: 'grow' + (locked ? ' locked' : '') +
                                 (spec.tone ? ' tone' : ''),
                          title: hint ? hint + '\n\n(' + spec.path + ')'
                                      : spec.path },
                 lock, label, input, readout || el('span'));
  return hint
    ? el('div', { class: 'gwrap' }, row, el('div', { class: 'ghint' }, hint))
    : row;
}

/** A gene's value as it is worth reading. `uiMax` rows print the word rather
 *  than the number once the value has run off the top of the track -- "99" in
 *  the nape-line box is a puzzle, "bald" is the answer to it. */
const fmt = (v, spec) => {
  if (spec && spec.uiMax != null && v > spec.uiMax) return 'bald';
  return Number.isInteger(v) ? String(v) : String(Math.round(v * 100) / 100);
};

/** Redraw only the tweak pane's two canvases and the three lines under them.
 *  Called on every slider frame, so it must not touch the pool or the litter:
 *  a full render() is thirty bodies, and this is one. */
function renderPreviewOnly() {
  if (!root) return;
  const cs = root.querySelectorAll('.prev canvas');
  let b = null;
  if (cs[0]) drawOrbit(cs[0], b = buildOf(S.genome, S.name), S.view || ORBIT_HOME);
  if (cs[1]) b = thumbFor(cs[1], S.genome, S.name, 'side');
  const box = root.querySelector('.bstats');
  if (box && b) { box.textContent = ''; box.append(...statLines(b)); }
}

/** The same painter from the character's LEFT, which is the view that shows a
 *  profile — the jaw, the crown, the hair sweep and the chest depth, none of
 *  which read from the front at this resolution. */
function drawSide(canvas, b) {
  if (!b || !b.parts) return;
  // Reuse drawBody by handing it a body whose x and y are swapped: the painter
  // is axis-generic and this is cheaper (and far less code) than a second
  // projection. The swap turns "look along -y" into "look along -x".
  const swapped = {
    genome: b.genome,
    parts: b.parts.map(p => ({
      name: p.name,
      mn: [p.mn[1], p.mn[0], p.mn[2]],
      size: [p.size[1], p.size[0], p.size[2]],
      cells: p.cells.map(([x, y, z, c]) => [y, x, z, c]),
    })),
  };
  drawBody(canvas, swapped);
}

function variantRow() {
  const v = S.variant;
  const num = (label, key, min, max, step, tip) =>
    el('label', { style: 'font-size:10px', title: tip }, label + ' ',
      el('input', { type: 'number', value: v[key], min, max, step,
                    style: 'width:56px', title: tip,
                    oninput: e => { v[key] = Number(e.target.value); } }));
  const baseSel = el('select', {
    title: 'the character this variant re-colours. It keeps whatever shape, ' +
           'rig and anatomy that character has, for ever.',
    onchange: e => { S.baseFor = e.target.value; },
  }, el('option', { value: '' }, 'recolour…'),
     S.pool.map(p => el('option', { value: p.name,
                                    selected: p.name === S.baseFor }, p.name)));
  return el('div', {},
    el('div', { class: 'note' },
      'A RECOLOUR is a twenty-line file with no art of its own: it points at ',
      'another character and turns a dial over its colours, so it follows ',
      'that character for ever — including what is under the skin. The dial ',
      'is global, though: it can wash everything cooler or paler, but it ',
      'cannot say "red hair, same skin". For that, save a whole character.'),
    el('div', { class: 'brow' }, baseSel,
      num('colour', 'saturation', 0, 2, 0.05,
          '0 is grey, 1 is unchanged, 2 is lurid'),
      num('light', 'brightness', 0.2, 2, 0.05,
          'below 1 darkens the whole figure, above 1 washes it out'),
      el('input', { type: 'color', value: v.tint,
                    title: 'the colour to wash towards',
                    oninput: e => { v.tint = e.target.value; } }),
      num('wash', 'tintAmount', 0, 1, 0.05,
          'how far towards that colour. 0 leaves it alone; the shipped zombie ' +
          'is a small push towards a sick green.'),
      el('button', { title: 'writes assets/mobs/<name>.json only — no art',
                     onclick: saveVariant }, 'save recolour (.json only)')));
}

// ---- litter -----------------------------------------------------------------

function litterPane() {
  const ps = parentGenomes();
  const SRC = {
    random: ['from scratch',
             'Ignores the parents entirely and rolls a whole new person each ' +
             'time. The widest net, and the least likely to give you two ' +
             'children that look related.'],
    mutate: ['vary one parent',
             'Takes the first parent (or the character in the Tweak pane if ' +
             'there are none) and nudges it. This is the one you live in: ' +
             'keep the best child, make it the parent, roll again.'],
    cross:  ['blend the parents',
             'Mixes two or more parents setting by setting, then nudges the ' +
             'result. Every child lands between the parents, never outside ' +
             'them.'],
  };
  const srcSel = el('select', {
    title: Object.values(SRC).map(([a, b2]) => a.toUpperCase() + ' — ' + b2)
             .join('\n\n'),
    onchange: e => { S.source = e.target.value; rollLitter(); render(); },
  }, Object.entries(SRC).map(([k, [lab]]) =>
       el('option', { value: k, selected: S.source === k, title: SRC[k][1] },
          lab)));

  // Re-roll on change rather than waiting for the button. Setting the count to
  // 24 and getting 12 until you also press roll is the page disobeying you,
  // and it was the first thing anyone hit.
  const reroll = () => { rollLitter(); render(); };

  return el('div', { class: 'bcol' },
    el('h3', { title: 'a fresh batch of candidates, drawn from the parents' },
       'litter'),
    el('div', { class: 'brow' }, srcSel,
      el('label', { style: 'font-size:10px', title: 'how many children to ' +
                    'show at once' }, 'how many ',
        el('input', { type: 'number', value: S.litterCount, min: 1, max: 48,
                      style: 'width:46px',
                      onchange: e => {
                        S.litterCount = Math.max(1,
                          Math.min(48, e.target.value | 0));
                        reroll();
                      } })),
      el('label', { style: 'font-size:10px',
                    title: 'how far a child may drift from its parent. Left ' +
                           'is an exact copy; the middle is a recognisable ' +
                           'sibling; the right is a stranger.' },
        'how different ',
        el('input', { type: 'range', min: 0, max: 2, step: 0.05,
                      value: S.sigma, style: 'width:80px',
                      onchange: e => { S.sigma = Number(e.target.value);
                                       reroll(); } })),
      el('label', { style: 'font-size:10px',
                    title: 'the number the dice start from. The same seed, ' +
                           'the same parents and the same pins always give ' +
                           'the same litter, so a child you liked is always ' +
                           'one number away.' }, 'seed ',
        el('input', { type: 'number', value: S.seed, style: 'width:56px',
                      onchange: e => { S.seed = e.target.value | 0;
                                       reroll(); } })),
      el('button', { title: 'next seed, new children',
                     onclick: () => { S.seed++; rollLitter(); render(); } },
         'roll')),
    el('div', { class: 'brow' },
      `parents (${ps.length}):`,
      ps.length ? ps.map((g, i) => {
        const c = canvasFor(32, 50);
        thumbFor(c, g, 'parent', 'front');
        return el('span', { title: 'click to drop this parent',
                            style: 'cursor:pointer',
                            onclick: () => { S.parents.splice(i, 1); render(); } },
                  c);
      }) : el('span', { class: 'note' },
              'none yet — click one in the Pool, or press "parent" under a ' +
              'child below. With no parents, "vary one parent" varies the ' +
              'character in the Tweak pane instead.')),
    el('div', { class: 'note' },
      'Keep what looks like a person and breed from it — there is no score ',
      'here, you are the judge. A PINNED setting (the boxes in the Tweak ',
      'pane) does not move, which is what lets a litter vary hair and face ',
      'while holding the body still. A red border is a child that failed its ',
      'own structural checks — hover it to read why.'),
    el('div', { class: 'litter' },
      S.litter.length ? S.litter.map((k, i) => litterCell(k, i))
        : el('div', { class: 'note' }, 'press roll')));
}

function litterCell(k, i) {
  // The backing store is 2x the CSS box (the cell's canvas is stretched to the
  // column width by `width:100%`), so the shipped lattice's detail survives the
  // scale instead of being averaged away before you can judge it.
  const c = canvasFor(168, 256);
  c.style.width = ''; c.style.height = '';         // the grid's CSS sizes it
  const b = thumbFor(c, k.genome, 'litter', 'front', k.label);
  const bad = b.error || b.complaints.length;
  return el('div', { class: 'lcell' + (bad ? ' bad' : ''),
                     title: bad ? String(b.error || b.complaints[0])
                                : k.label + ' (seed ' + k.seed + ')' },
    c,
    el('div', { class: 'lbtns' },
      el('button', {
        title: 'load this one into the sliders',
        onclick: () => {
          if (!mayDiscard('Load ' + k.label)) return;
          S.genome = mg.normalizeGenome(k.genome);
          S.name = 'newcomer';
          // It IS unsaved work from this moment: the litter cell it came from
          // survives a re-roll only while the seed is untouched, and the name
          // is a placeholder that no file on disk answers to.
          setDirty(true);
          render();
        },
      }, 'tweak'),
      el('button', {
        title: 'add this one to the breeding stock',
        onclick: () => {
          if (S.parents.length >= 4) S.parents.shift();
          S.parents.push(mg.normalizeGenome(k.genome));
          if (S.parents.length >= 2) S.source = 'cross';
          render();
        },
      }, 'parent')));
}

// =============================================================================
// host interface — the same four-call shape the Models and Environment tabs use
// =============================================================================

export function attach(hooks) {
  H = hooks;
  root = hooks.section;
  css();
}

export async function activate() {
  if (!built) {
    built = true;
    if (!S.litter.length) rollLitter();
    await loadPool();
  }
  render();
}

/** Ctrl+S from the host. Returns the promise so a caller (the headless
 *  harness) can await the write rather than racing it. */
export function saveFromHost() { return saveCharacter(); }

/** For the headless browser check (scripts/check_characters.sh): drive the page
 *  without a mouse. Kept small and boring on purpose — a test hook that can do
 *  more than the UI is a second implementation. */
export const __test = {
  state: S,
  buildOf, drawBody, rollLitter, render, loadPool,
  set(path, v) { gset(path, v); render(); },
  preset(k) { S.genome = mg.presetGenome(k); render(); },
};
