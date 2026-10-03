/* robotkit.js — what the two ROBOT races (automaton.js, android.js) share.
 *
 * A robot, to the generator, is the sylvan's contract again: the human's rig
 * unchanged (fifteen limb boxes, anchors, sockets, chains, clips -- so every
 * helm, cuirass and blade fits), a SURFACE pass that may carve and paint
 * inside each part's own box but never grow it, and a few EXTRAS that stand
 * off the body as bloodless parts (mobgen hairMass's `snout` group rides the
 * head, `bough` the torso). This file is the toolbox those passes are written
 * in: the hash, a value noise, a mutable cell grid with the carving
 * primitives (a recessed disc, a groove, a proud cell), the front map of a
 * face, the one-piece filter, and the 6-connected line the extras are drawn
 * with.
 *
 * PURE AND NODE-RUNNABLE, like everything mobgen imports. Every random choice
 * is a counter hash of the genome's own numbers and the part's name, so a
 * body is a pure function of its genome.
 */

'use strict';

// ---- hashing (mobgen.js hashN's mix; duplicated so no import cycle) -------
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x >>> 0;
}
export function h32(...vals) {
  let h = 0x9e3779b9;
  for (let i = 0; i < vals.length; i++) {
    h = mix32((h ^ (vals[i] | 0)) >>> 0);
    h = (h + 0x9e3779b9) >>> 0;
  }
  return mix32(h);
}
export const rnd = (...v) => h32(...v) / 4294967296;
export const strHash = s => {
  let h = 7;
  for (const ch of String(s)) h = h32(h, ch.charCodeAt(0));
  return h;
};
export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/** Coarse value noise, 0..1, cell spacing `s`. */
export function vnoise(seed, x, y, z, s) {
  const fx = x / s, fy = y / s, fz = z / s;
  const x0 = Math.floor(fx), y0 = Math.floor(fy), z0 = Math.floor(fz);
  const tx = fx - x0, ty = fy - y0, tz = fz - z0;
  const sm = t => t * t * (3 - 2 * t);
  const a = sm(tx), b = sm(ty), c = sm(tz);
  const v = (i, j, k) => rnd(seed, x0 + i, y0 + j, z0 + k);
  const l = (p, q, t) => p + (q - p) * t;
  return l(l(l(v(0, 0, 0), v(1, 0, 0), a), l(v(0, 1, 0), v(1, 1, 0), a), b),
           l(l(v(0, 0, 1), v(1, 0, 1), a), l(v(0, 1, 1), v(1, 1, 1), a), b), c);
}

export const D6 = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1],
                   [0, 0, -1]];

/**
 * A part's cells as a mutable grid. Part-local, front is LOW y (mobgen's
 * flipY), z up. `put` refuses a cell outside the box or already taken -- the
 * box is what armour is fit to, so nothing here may grow it.
 */
export function grid(cells, size) {
  const [sx, sy, sz] = size;
  const idx = (x, y, z) => (z * sy + y) * sx + x;
  const inBox = (x, y, z) => x >= 0 && y >= 0 && z >= 0 &&
                             x < sx && y < sy && z < sz;
  const at = new Map();
  for (const [x, y, z, c] of cells) at.set(idx(x, y, z), [x, y, z, c]);
  const has = (x, y, z) => inBox(x, y, z) && at.has(idx(x, y, z));
  const get = (x, y, z) => (inBox(x, y, z) ? at.get(idx(x, y, z)) : undefined);
  const g = {
    sx, sy, sz, cx: sx / 2, cy: sy / 2, idx, inBox, has, get,
    paint(x, y, z, c) { const v = get(x, y, z); if (v) v[3] = c; return !!v; },
    del(x, y, z) { at.delete(idx(x, y, z)); },
    put(x, y, z, c) {
      if (!inBox(x, y, z) || has(x, y, z)) return false;
      at.set(idx(x, y, z), [x, y, z, c]);
      return true;
    },
    list: () => [...at.values()],
    // A cell with an open side face: the part's skin, not its ends.
    lateral: (x, y, z) => !has(x - 1, y, z) || !has(x + 1, y, z) ||
                          !has(x, y - 1, z) || !has(x, y + 1, z),
    exposed: (x, y, z) => D6.some(([dx, dy, dz]) => !has(x + dx, y + dy, z + dz)),
    // One cell in from (x, y, z) toward the part's long axis.
    inward(x, y) {
      const ox = x + 0.5 - sx / 2, oy = y + 0.5 - sy / 2;
      return Math.abs(ox) >= Math.abs(oy) ? [x - Math.sign(ox), y]
                                          : [x, y - Math.sign(oy)];
    },
    // Around the part: 0..1 from +x, counter-clockwise seen from above.
    turn: (x, y) => (Math.atan2(y + 0.5 - sy / 2, x + 0.5 - sx / 2) /
                     (2 * Math.PI) + 1) % 1,
  };
  /** A GROOVE: delete a surface cell whose inner neighbour stays (one cell
   *  deep, never a hole) and paint that floor. Returns whether it cut. */
  g.groove = (x, y, z, floor) => {
    if (!has(x, y, z)) return false;
    const [ix, iy] = g.inward(x, y);
    if (!has(ix, iy, z)) return false;
    g.del(x, y, z);
    if (floor !== undefined) g.paint(ix, iy, z, floor);
    return true;
  };
  return g;
}

/** The FRONT (lowest y) of every (x, z) column, measured once. */
export function frontMap(cells) {
  const m = new Map();
  for (const [x, y, z] of cells) {
    const k = x * 4096 + z;
    if (!m.has(k) || y < m.get(k)) m.set(k, y);
  }
  return (x, z) => m.get(x * 4096 + z);
}
/** The BACK (highest y) of every (x, z) column. */
export function backMap(cells) {
  const m = new Map();
  for (const [x, y, z] of cells) {
    const k = x * 4096 + z;
    if (!m.has(k) || y > m.get(k)) m.set(k, y);
  }
  return (x, z) => m.get(x * 4096 + z);
}

/**
 * A DISC on a face, recessed `depth` cells and painted by `paintOf(d, ang)`
 * (d = 0 at the centre .. 1 at the rim; the rim ring past 1 up to 1 + rimW/r
 * is painted by `rimOf`). `face` is a frontMap/backMap and `dir` -1 for the
 * front (cells go +y inward), +1 for the back. The disc is in the x-z plane.
 */
export function disc(G, face, dir, px, pz, rx, rz, depth, paintOf, rimOf, rimW = 0) {
  const r = Math.max(rx, rz);
  for (let z = Math.floor(pz - rz - rimW - 1); z <= pz + rz + rimW + 1; z++)
    for (let x = Math.floor(px - rx - rimW - 1); x <= px + rx + rimW + 1; x++) {
      const f = face(x, z);
      if (f === undefined) continue;
      const ex = (x + 0.5 - px) / rx, ez = (z + 0.5 - pz) / rz;
      const d = Math.sqrt(ex * ex + ez * ez);
      const ang = Math.atan2(ez, ex);
      const step = -dir;                         // inward along y
      if (d <= 1) {
        // Never through: the floor cell must exist.
        let k = 0;
        while (k < depth && G.has(x, f + step * (k + 1), z)) {
          G.del(x, f + step * k, z);
          k++;
        }
        const floor = f + step * k;
        const c = paintOf(d, ang, x, z);
        if (c !== undefined && c !== null) G.paint(x, floor, z, c);
      } else if (rimOf && d <= 1 + rimW / r) {
        const c = rimOf(d, ang, x, z);
        if (c !== undefined && c !== null) G.paint(x, f, z, c);
      }
    }
}

/** Keep the largest 6-connected piece (the engine splits a disconnected limb
 *  on its first carve, so a carve that orphans a cell must drop it). */
export function keepLargest(cells, size) {
  const [sx, sy, sz] = size;
  const idx = (x, y, z) => (z * sy + y) * sx + x;
  const at = new Map(cells.map((c, i) => [idx(c[0], c[1], c[2]), i]));
  const comp = new Int32Array(cells.length).fill(-1);
  let best = -1, bestN = 0, nComp = 0;
  for (let i = 0; i < cells.length; i++) {
    if (comp[i] >= 0) continue;
    const q = [i]; comp[i] = nComp;
    for (let qi = 0; qi < q.length; qi++) {
      const [x, y, z] = cells[q[qi]];
      for (const [dx, dy, dz] of D6) {
        const X = x + dx, Y = y + dy, Z = z + dz;
        if (X < 0 || Y < 0 || Z < 0 || X >= sx || Y >= sy || Z >= sz) continue;
        const j = at.get(idx(X, Y, Z));
        if (j === undefined || comp[j] >= 0) continue;
        comp[j] = nComp; q.push(j);
      }
    }
    if (q.length > bestN) { bestN = q.length; best = nComp; }
    nComp++;
  }
  return nComp <= 1 ? cells : cells.filter((_, i) => comp[i] === best);
}

/**
 * The head's face, as the human head painted it: where the eyes are (the EYE
 * cells near the eye row), the eye row, the mouth row, the chin. Every face
 * pass starts here and then WIPES the human paint (pupils, a dark mouth,
 * brows) back to plate, so a robot never wears a human's painted face.
 */
export function readFace(G, A, info, plateOf) {
  const cx = G.cx;
  const eyeRow = info.eyeRow ?? G.sz * 0.6;
  const eyeCells = G.list().filter(c => c[3] === A.EYE &&
                                       Math.abs(c[2] + 0.5 - eyeRow) < 4);
  const groups = [eyeCells.filter(c => c[0] + 0.5 < cx),
                  eyeCells.filter(c => c[0] + 0.5 >= cx)];
  const eyes = [];
  for (const grp of groups) {
    if (!grp.length) continue;
    eyes.push({ x: grp.reduce((a, c) => a + c[0] + 0.5, 0) / grp.length,
                z: grp.reduce((a, c) => a + c[2] + 0.5, 0) / grp.length });
  }
  // Fallback: a head whose eyes the human pass did not paint (an extreme
  // eye-row gene) still gets two, a third of the head apart.
  if (eyes.length < 2) {
    eyes.length = 0;
    eyes.push({ x: cx - G.sx * 0.18, z: eyeRow + 0.5 },
              { x: cx + G.sx * 0.18, z: eyeRow + 0.5 });
  }
  const FACEPAINT = new Set([A.EYE, A.HAIR, A.HAIR_SHADE, A.SKIN_LIGHT, A.NAIL]);
  for (const v of G.list()) if (FACEPAINT.has(v[3])) v[3] = plateOf(v);
  return {
    eyes,
    eyeRow,
    mouthRow: info.mouthRow ?? eyeRow - 5,
    mouthDx: info.mouthDx || 0,
    neckTop: info.neckTop || 0,
    front: frontMap(G.list()),
  };
}

/** A 6-connected line between two cells: a diagonal step is broken into
 *  axis steps, so a thin strand is ONE piece the way the engine counts
 *  pieces. `emit(x, y, z)` is called for every cell after the first. */
export function line6(a, b, emit) {
  let [x, y, z] = a.map(Math.round);
  const t = b.map(Math.round);
  let guard = 0;
  while ((x !== t[0] || y !== t[1] || z !== t[2]) && guard++ < 512) {
    const dx = t[0] - x, dy = t[1] - y, dz = t[2] - z;
    const ax = Math.abs(dx), ay = Math.abs(dy), az = Math.abs(dz);
    if (ax >= ay && ax >= az) x += Math.sign(dx);
    else if (ay >= az) y += Math.sign(dy);
    else z += Math.sign(dz);
    emit(x, y, z);
  }
}

/** Every cell of a solid upright cylinder (axis +z) in scene coordinates. */
export function cylinder(cx, cy, z0, z1, r, emit) {
  const R = Math.ceil(r + 0.5);
  for (let z = z0; z <= z1; z++)
    for (let y = Math.floor(cy) - R; y <= Math.floor(cy) + R; y++)
      for (let x = Math.floor(cx) - R; x <= Math.floor(cx) + R; x++) {
        const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
        if (d <= r + 0.15) emit(x, y, z, d / Math.max(r, 0.5));
      }
}

/** Head-local / torso-local cells of a part, in scene coordinates. */
export const sceneCells = p =>
  p.cells.map(([x, y, z, c]) => [p.mn[0] + x, p.mn[1] + y, p.mn[2] + z, c]);

/** The extent of a part in scene coordinates: {x0,x1,y0,y1,z0,z1,cx,cy}. */
export function extent(p) {
  const s = sceneCells(p);
  const e = { x0: 1e9, x1: -1e9, y0: 1e9, y1: -1e9, z0: 1e9, z1: -1e9 };
  for (const [x, y, z] of s) {
    e.x0 = Math.min(e.x0, x); e.x1 = Math.max(e.x1, x);
    e.y0 = Math.min(e.y0, y); e.y1 = Math.max(e.y1, y);
    e.z0 = Math.min(e.z0, z); e.z1 = Math.max(e.z1, z);
  }
  e.cx = (e.x0 + e.x1 + 1) / 2;
  e.cy = (e.y0 + e.y1 + 1) / 2;
  return e;
}
