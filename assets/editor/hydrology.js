/* hydrology.js — water and erosion on a height GRID for the World map page
 * (docs/PLAN_worldbuilder.md K1 item 3): pure, DOM-free, typed arrays only,
 * so it runs in a Worker (terrain_worker.js), in Node (scripts/
 * test_hydrology.mjs) and on the page.
 *
 * A grid is a row-major array of heights h[j * w + i] (VOXELS, absolute Y),
 * cells `dx` voxels apart. Everything here is DETERMINISTIC: fixed loop
 * orders, a binary heap that breaks ties by cell index, no Math.random.
 *
 *   priorityFlood     Barnes et al. 2014 Priority-Flood (+epsilon variant):
 *                     fills every depression to its spill level, seeded from
 *                     the grid edge and every cell flagged as an outlet (the
 *                     sea). epsilon > 0 leaves a tiny gradient across flats
 *                     so D8 drains them.
 *   d8                steepest-descent receivers (8 neighbours, diagonal
 *                     distance dx*sqrt2); a cell with no lower neighbour is
 *                     its own receiver (a pit or an outlet).
 *   stackOrder        Braun & Willett 2013: the cells ordered so that every
 *                     receiver comes before its donors, O(n).
 *   accumulate        drainage area (in cells, or weighted) down the stack, O(n).
 *   mfd               multiple-flow-direction accumulation (Quinn / Freeman,
 *                     exponent p) and the topographic wetness index.
 *   lakes             depressions of (filled - h): each lake's cells, spill
 *                     level, area, max depth and a simplified outline.
 *   lakeAt            the basin under one cell: its spill level and outline.
 *   rivers            the channel network (accumulation >= threshold) as
 *                     polylines SOURCE -> MOUTH, main stems first, each
 *                     tributary naming the river it joins; Douglas-Peucker +
 *                     Chaikin; width w = k * sqrt(A).
 *   streamPower       the implicit Braun-Willett stream-power solver
 *                     (m = 0.5, n = 1) with an uplift field U and an
 *                     erodibility / hardness field K, plus a thermal pass
 *                     that caps slopes at the talus angle.
 *   thermal           talus slumping (Jacobi, 8 neighbours).
 *   downsample / upsample   the coarse PROXY these run on (25.6 / 51.2 m) and
 *                     the way back (bicubic Catmull-Rom), so a whole-map pass
 *                     costs O(map / 64) and the posts get the detail back. */

const SQ2 = Math.SQRT2;
// 8 neighbours: E, W, S, N, SE, SW, NE, NW (fixed order = deterministic ties)
const NX = [1, -1, 0, 0, 1, -1, 1, -1];
const NZ = [0, 0, 1, -1, 1, 1, -1, -1];
const ND = [1, 1, 1, 1, SQ2, SQ2, SQ2, SQ2];

/* ---- a binary min-heap on (key, index), ties broken by index ---------------------------- */
class Heap {
  constructor(cap) { this.k = new Float64Array(cap); this.i = new Int32Array(cap); this.n = 0; }
  less(a, b) { const ka = this.k[a], kb = this.k[b]; return ka < kb || (ka === kb && this.i[a] < this.i[b]); }
  push(key, idx) {
    if (this.n === this.k.length) {
      const k = new Float64Array(this.n * 2), i = new Int32Array(this.n * 2);
      k.set(this.k); i.set(this.i); this.k = k; this.i = i;
    }
    let c = this.n++;
    this.k[c] = key; this.i[c] = idx;
    while (c > 0) {
      const p = (c - 1) >> 1;
      if (!this.less(c, p)) break;
      this.swap(c, p); c = p;
    }
  }
  swap(a, b) {
    const tk = this.k[a]; this.k[a] = this.k[b]; this.k[b] = tk;
    const ti = this.i[a]; this.i[a] = this.i[b]; this.i[b] = ti;
  }
  pop() {   // returns the index; the key is this.top
    this.top = this.k[0];
    const r = this.i[0];
    this.n--;
    if (this.n > 0) {
      this.k[0] = this.k[this.n]; this.i[0] = this.i[this.n];
      let c = 0;
      for (;;) {
        const l = 2 * c + 1, rr = l + 1;
        let m = c;
        if (l < this.n && this.less(l, m)) m = l;
        if (rr < this.n && this.less(rr, m)) m = rr;
        if (m === c) break;
        this.swap(c, m); c = m;
      }
    }
    return r;
  }
}

/** Outlet mask: the grid edge plus every cell below `seaY` (if given) or
 *  flagged in `outlets` (Uint8Array). */
export function outletMask(h, w, hh, opts = {}) {
  const m = new Uint8Array(w * hh);
  for (let i = 0; i < w; i++) { m[i] = 1; m[(hh - 1) * w + i] = 1; }
  for (let j = 0; j < hh; j++) { m[j * w] = 1; m[j * w + w - 1] = 1; }
  if (opts.seaY != null) for (let n = 0; n < m.length; n++) if (h[n] < opts.seaY) m[n] = 1;
  if (opts.outlets) for (let n = 0; n < m.length; n++) if (opts.outlets[n]) m[n] = 1;
  if (opts.edges === false) for (let n = 0; n < m.length; n++) {
    const i = n % w, j = (n / w) | 0;
    if ((i === 0 || j === 0 || i === w - 1 || j === hh - 1) && !(opts.seaY != null && h[n] < opts.seaY) && !(opts.outlets && opts.outlets[n])) m[n] = 0;
  }
  return m;
}

/** Priority-Flood (Barnes, Lehman & Mulla 2014), the "improved" variant with a
 *  plain FIFO for cells inside a depression. Returns a NEW Float64Array: every
 *  cell raised to the lowest level from which water can reach an outlet.
 *  opts = {epsilon (0 = flat lakes; > 0 = the +epsilon variant, each step into
 *  a depression climbs by epsilon so D8 drains it), seaY, outlets, edges}. */
export function priorityFlood(h, w, hh, opts = {}) {
  const N = w * hh, eps = +opts.epsilon || 0;
  const z = Float64Array.from(h);
  const seen = new Uint8Array(N);
  const out = opts.mask || outletMask(h, w, hh, opts);
  const heap = new Heap(Math.max(16, 4 * (w + hh)));
  const pit = new Int32Array(N);
  let ph = 0, pt = 0;
  for (let n = 0; n < N; n++) if (out[n]) { seen[n] = 1; heap.push(z[n], n); }
  while (heap.n > 0 || ph < pt) {
    let c;
    if (ph < pt) c = pit[ph++];
    else c = heap.pop();
    const zc = z[c], ci = c % w, cj = (c / w) | 0;
    for (let k = 0; k < 8; k++) {
      const ii = ci + NX[k], jj = cj + NZ[k];
      if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
      const nb = jj * w + ii;
      if (seen[nb]) continue;
      seen[nb] = 1;
      const floor = eps > 0 ? zc + eps : zc;
      if (z[nb] <= floor) {
        // inside a depression (or on a flat): raise and walk it breadth-first
        z[nb] = eps > 0 ? (z[nb] > floor ? z[nb] : floor) : zc;
        pit[pt++] = nb;
      } else heap.push(z[nb], nb);
    }
    if (ph === pt) { ph = 0; pt = 0; }
  }
  return z;
}

/** A seeded hash of (cell, k) to [0, 1). */
export function hashU(n, k, seed) {
  let h = Math.imul(n | 0, 0x27d4eb2d) ^ Math.imul((k + 1) | 0, 0x165667b1) ^ Math.imul(seed | 0, 0x9e3779b1);
  h ^= h >>> 15; h = Math.imul(h, 0x85ebca6b); h ^= h >>> 13; h = Math.imul(h, 0xc2b2ae35);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
/** D8 steepest-descent receivers on `z`. Returns {rcv: Int32Array (self = a
 *  pit or outlet), dist: Float32Array (voxels to the receiver)}. Outlets
 *  (opts.mask) are always their own receivers. */
export function d8(z, w, hh, dx, mask = null, jitter = 0, seed = 0, out = null) {
  const N = w * hh;
  const rcv = out && out.rcv && out.rcv.length === N ? out.rcv : new Int32Array(N);
  const dist = out && out.dist && out.dist.length === N ? out.dist : new Float32Array(N);
  // STOCHASTIC routing (jitter > 0): a fixed, seeded per-(cell, direction)
  // factor on each downhill slope, so a planar flank does not drain in
  // straight grid-aligned lines and flow paths converge. Cached per call site
  // through out.jf (N * 8 factors), it is the same every step of a run.
  let jf = null;
  if (jitter > 0) {
    jf = out && out.jf && out.jf.length === N * 8 && out.jfKey === jitter + ':' + seed ? out.jf : jitterFactors(N, jitter, seed);
    if (out) { out.jf = jf; out.jfKey = jitter + ':' + seed; }
  }
  const OFF = [1, -1, w, -w, w + 1, w - 1, -w + 1, -w - 1];
  for (let j = 0; j < hh; j++) {
    const edgeRow = j === 0 || j === hh - 1;
    for (let i = 0; i < w; i++) {
      const n = j * w + i;
      rcv[n] = n; dist[n] = dx;
      if (mask && mask[n]) continue;
      const zn = z[n];
      let best = 0, bk = -1;
      if (!edgeRow && i > 0 && i < w - 1) {
        // interior: no bounds checks
        for (let k = 0; k < 8; k++) {
          let s = (zn - z[n + OFF[k]]) * (k < 4 ? 1 : 0.7071067811865476);
          if (s > 0 && jf) s *= jf[n * 8 + k];
          if (s > best) { best = s; bk = k; }
        }
      } else {
        for (let k = 0; k < 8; k++) {
          const ii = i + NX[k], jj = j + NZ[k];
          if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
          let s = (zn - z[jj * w + ii]) / ND[k];
          if (s > 0 && jf) s *= jf[n * 8 + k];
          if (s > best) { best = s; bk = k; }
        }
      }
      if (bk >= 0) { rcv[n] = n + OFF[bk]; dist[n] = dx * ND[bk]; }
    }
  }
  return {rcv, dist};
}
/** The jitter multipliers of d8: 1 + jitter * (u * 2 - 1), u = hashU(n, k, seed). */
export function jitterFactors(N, jitter, seed) {
  const jf = new Float32Array(N * 8);
  for (let n = 0; n < N; n++) for (let k = 0; k < 8; k++) jf[n * 8 + k] = 1 + jitter * (hashU(n, k, seed) * 2 - 1);
  return jf;
}

/** Braun & Willett 2013 stack: every receiver before its donors. O(n). */
export function stackOrder(rcv) {
  const N = rcv.length, ndon = new Int32Array(N + 1);
  for (let n = 0; n < N; n++) if (rcv[n] !== n) ndon[rcv[n] + 1]++;
  for (let n = 0; n < N; n++) ndon[n + 1] += ndon[n];      // prefix: donors of n at [ndon[n], ndon[n+1])
  const don = new Int32Array(Math.max(1, ndon[N])), fillp = ndon.slice(0, N);
  for (let n = 0; n < N; n++) if (rcv[n] !== n) don[fillp[rcv[n]]++] = n;
  const stack = new Int32Array(N), st = new Int32Array(N);
  let k = 0;
  for (let b = 0; b < N; b++) {
    if (rcv[b] !== b) continue;
    let top = 0;
    st[top++] = b;
    while (top) {
      const c = st[--top];
      stack[k++] = c;
      for (let d = ndon[c + 1] - 1; d >= ndon[c]; d--) st[top++] = don[d];
    }
  }
  return stack;
}

/** Drainage area down the stack: each cell's `weight` (default 1 = cells)
 *  plus everything upstream. */
export function accumulate(rcv, stack, weight = null) {
  const N = rcv.length, A = new Float64Array(N);
  if (weight) for (let n = 0; n < N; n++) A[n] = weight[n]; else A.fill(1);
  for (let k = N - 1; k >= 0; k--) { const n = stack[k], r = rcv[n]; if (r !== n) A[r] += A[n]; }
  return A;
}

/** Everything a water overlay or a river trace needs, from raw heights:
 *  {filled (epsilon), mask, rcv, dist, stack, A}. */
export function flow(h, w, hh, dx, opts = {}) {
  const mask = outletMask(h, w, hh, opts);
  const filled = priorityFlood(h, w, hh, {...opts, mask, epsilon: opts.epsilon == null ? 1e-3 : opts.epsilon});
  const {rcv, dist} = d8(filled, w, hh, dx, mask);
  const stack = stackOrder(rcv);
  const A = accumulate(rcv, stack, opts.weight || null);
  return {filled, mask, rcv, dist, stack, A};
}

/** Multiple-flow-direction accumulation (Quinn 1991 / Freeman 1991): each
 *  cell passes its area to every lower neighbour in proportion to
 *  (drop / distance)^p. Run on the epsilon-filled surface so flats drain.
 *  Returns {A (cells), twi: ln(A * dx / tan(slope)) (wetness index)}. */
export function mfd(h, w, hh, dx, opts = {}) {
  const p = opts.p == null ? 1.1 : +opts.p, N = w * hh;
  const z = opts.filled || priorityFlood(h, w, hh, {...opts, epsilon: 1e-3});
  const order = new Int32Array(N);
  for (let n = 0; n < N; n++) order[n] = n;
  const idx = Array.from(order).sort((a, b) => z[b] - z[a] || a - b);
  const A = new Float64Array(N).fill(1), twi = new Float32Array(N);
  const wt = new Float64Array(8);
  for (const n of idx) {
    const i = n % w, j = (n / w) | 0;
    let sum = 0, smax = 0;
    for (let k = 0; k < 8; k++) {
      const ii = i + NX[k], jj = j + NZ[k];
      wt[k] = 0;
      if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
      const s = (z[n] - z[jj * w + ii]) / (ND[k] * dx);
      if (s > 0) { wt[k] = Math.pow(s, p) * (k < 4 ? 0.5 : 0.354); sum += wt[k]; if (s > smax) smax = s; }
    }
    twi[n] = Math.log(A[n] * dx / Math.max(1e-3, smax));
    if (sum <= 0) continue;
    for (let k = 0; k < 8; k++) if (wt[k] > 0) A[(j + NZ[k]) * w + i + NX[k]] += A[n] * wt[k] / sum;
  }
  return {A, twi};
}

/* ---- lakes --------------------------------------------------------------------------- */
/** Douglas-Peucker on [[x, z], ...] (open polyline; a closed ring passes its
 *  first point again at the end). */
export function simplify(pts, tol) {
  if (pts.length < 3) return pts.slice();
  const keep = new Uint8Array(pts.length);
  keep[0] = keep[pts.length - 1] = 1;
  const st = [[0, pts.length - 1]];
  while (st.length) {
    const [a, b] = st.pop();
    const ax = pts[a][0], az = pts[a][1], ex = pts[b][0] - ax, ez = pts[b][1] - az, L = Math.hypot(ex, ez);
    let worst = -1, wi = -1;
    for (let k = a + 1; k < b; k++) {
      const d = L > 0 ? Math.abs(ex * (pts[k][1] - az) - ez * (pts[k][0] - ax)) / L : Math.hypot(pts[k][0] - ax, pts[k][1] - az);
      if (d > worst) { worst = d; wi = k; }
    }
    if (worst > tol && wi > 0) { keep[wi] = 1; st.push([a, wi], [wi, b]); }
  }
  return pts.filter((_, k) => keep[k]);
}
/** Chaikin corner cutting, `iter` times; the ends of an open line stay put. */
export function chaikin(pts, iter = 1, closed = false) {
  let p = pts;
  for (let it = 0; it < iter; it++) {
    if (p.length < 3) return p;
    const q = closed ? [] : [p[0]];
    const n = p.length, last = closed ? n : n - 1;
    for (let k = 0; k < last; k++) {
      const a = p[k], b = p[(k + 1) % n];
      q.push([0.75 * a[0] + 0.25 * b[0], 0.75 * a[1] + 0.25 * b[1]], [0.25 * a[0] + 0.75 * b[0], 0.25 * a[1] + 0.75 * b[1]]);
    }
    if (!closed) { q.splice(1, 1); q.splice(q.length - 1, 1); q.push(p[n - 1]); }
    p = q;
  }
  return p;
}
/** The outer boundary of a cell set (Uint8Array mask over w x hh) as a ring of
 *  cell-corner points [[x, z], ...] in GRID units (corner (i, j) is the top
 *  left of cell (i, j)), walked clockwise from the topmost-leftmost cell; holes
 *  (islands) are not traced. */
export function outline(mask, w, hh) {
  let start = -1;
  for (let n = 0; n < mask.length; n++) if (mask[n]) { start = n; break; }
  if (start < 0) return [];
  const inside = (i, j) => i >= 0 && j >= 0 && i < w && j < hh && mask[j * w + i] === 1;
  // directed boundary edges, the inside on the RIGHT (x east, z south): an
  // edge leaving corner c in direction d (0 E, 1 S, 2 W, 3 N)
  const W1 = w + 1, out = new Map();
  const add = (x, z, d) => { const k = z * W1 + x; const l = out.get(k); if (l) l.push(d); else out.set(k, [d]); };
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      if (!inside(i, j)) continue;
      if (!inside(i, j - 1)) add(i, j, 0);
      if (!inside(i + 1, j)) add(i + 1, j, 1);
      if (!inside(i, j + 1)) add(i + 1, j + 1, 2);
      if (!inside(i - 1, j)) add(i, j + 1, 3);
    }
  const DX = [1, 0, -1, 0], DZ = [0, 1, 0, -1];
  const si = start % w, sj = (start / w) | 0;
  let x = si, z = sj, d = 0;
  const ring = [];
  for (let guard = 0; guard < 4 * mask.length + 8; guard++) {
    const k = z * W1 + x, l = out.get(k);
    if (!l || !l.length) break;
    // at a pinch corner prefer the right turn, then straight, then left
    let pick = -1;
    for (const want of [(d + 1) & 3, d, (d + 3) & 3]) { const q = l.indexOf(want); if (q >= 0) { pick = q; break; } }
    if (pick < 0) pick = 0;
    const nd = l[pick];
    l.splice(pick, 1);
    if (!ring.length || nd !== d) ring.push([x, z]);
    d = nd;
    x += DX[d]; z += DZ[d];
    if (x === si && z === sj) break;
  }
  return ring;
}
/** Every depression deeper than `minDepth` (voxels) and larger than `minCells`:
 *  [{cells: Int32Array, level (the spill level, voxels), area (cells),
 *    maxDepth, bbox [i0, j0, i1, j1], outline (grid units, simplified)}]. */
export function lakes(h, w, hh, opts = {}) {
  const filled = opts.filled || priorityFlood(h, w, hh, {...opts, epsilon: 0});
  const minDepth = opts.minDepth == null ? 0.5 : opts.minDepth, minCells = opts.minCells == null ? 4 : opts.minCells;
  const N = w * hh, lab = new Int32Array(N).fill(-1), out = [];
  const q = new Int32Array(N);
  for (let s = 0; s < N; s++) {
    if (lab[s] >= 0 || !(filled[s] - h[s] > minDepth)) continue;
    let qh = 0, qt = 0, maxD = 0, level = -Infinity, i0 = w, j0 = hh, i1 = 0, j1 = 0;
    q[qt++] = s; lab[s] = out.length;
    while (qh < qt) {
      const c = q[qh++], ci = c % w, cj = (c / w) | 0;
      maxD = Math.max(maxD, filled[c] - h[c]); level = Math.max(level, filled[c]);
      i0 = Math.min(i0, ci); j0 = Math.min(j0, cj); i1 = Math.max(i1, ci); j1 = Math.max(j1, cj);
      for (let k = 0; k < 4; k++) {
        const ii = ci + NX[k], jj = cj + NZ[k];
        if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
        const nb = jj * w + ii;
        if (lab[nb] >= 0 || !(filled[nb] - h[nb] > 1e-6)) continue;
        lab[nb] = out.length; q[qt++] = nb;
      }
    }
    if (qt < minCells) { for (let k = 0; k < qt; k++) lab[q[k]] = -2; continue; }
    out.push({cells: q.slice(0, qt), level, area: qt, maxDepth: maxD, bbox: [i0, j0, i1, j1]});
  }
  if (opts.outlines !== false)
    for (const L of out) {
      const [i0, j0, i1, j1] = L.bbox, bw = i1 - i0 + 1, bh = j1 - j0 + 1, m = new Uint8Array(bw * bh);
      for (const c of L.cells) m[(((c / w) | 0) - j0) * bw + (c % w) - i0] = 1;
      const ring = outline(m, bw, bh).map(([x, z]) => [x + i0, z + j0]);
      L.outline = simplify([...ring, ring[0]], opts.tolerance == null ? 0.75 : opts.tolerance).slice(0, -1);
    }
  return {lakes: out, filled, label: lab};
}
/** The basin holding cell `seed`: its spill level and its cells / outline
 *  (the water a lake there would hold), or null when the cell drains. */
export function lakeAt(h, w, hh, seed, opts = {}) {
  const filled = opts.filled || priorityFlood(h, w, hh, {...opts, epsilon: 0});
  const level = filled[seed];
  if (!(level - h[seed] > (opts.minDepth == null ? 1e-6 : opts.minDepth))) return null;
  const N = w * hh, m = new Uint8Array(N), q = new Int32Array(N);
  let qh = 0, qt = 0, i0 = w, j0 = hh, i1 = 0, j1 = 0, maxD = 0;
  q[qt++] = seed; m[seed] = 1;
  while (qh < qt) {
    const c = q[qh++], ci = c % w, cj = (c / w) | 0;
    i0 = Math.min(i0, ci); j0 = Math.min(j0, cj); i1 = Math.max(i1, ci); j1 = Math.max(j1, cj);
    maxD = Math.max(maxD, level - h[c]);
    for (let k = 0; k < 4; k++) {
      const ii = ci + NX[k], jj = cj + NZ[k];
      if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
      const nb = jj * w + ii;
      if (m[nb] || !(h[nb] < level)) continue;
      m[nb] = 1; q[qt++] = nb;
    }
  }
  const bw = i1 - i0 + 1, bh = j1 - j0 + 1, mm = new Uint8Array(bw * bh);
  for (let k = 0; k < qt; k++) { const c = q[k]; mm[(((c / w) | 0) - j0) * bw + (c % w) - i0] = 1; }
  const ring = outline(mm, bw, bh).map(([x, z]) => [x + i0, z + j0]);
  return {level, cells: q.slice(0, qt), area: qt, maxDepth: maxD, bbox: [i0, j0, i1, j1],
          outline: simplify([...ring, ring[0]], opts.tolerance == null ? 0.75 : opts.tolerance).slice(0, -1)};
}

/* ---- rivers ---------------------------------------------------------------------------- */
/** The channel network: cells draining at least `threshold` cells. Returns
 *  [{id, points: [[i, j, A], ...] SOURCE -> MOUTH (grid units, cell centres
 *  at i + 0.5), joins: id | null, mouth: 'outlet' | 'pit', A (at the mouth),
 *  width (at the mouth, voxels)}], main stems first. A tributary's last point
 *  is the junction cell ON its parent. widthK: w = widthK * sqrt(A * dx^2 / 1e6)
 *  voxels (A in km^2: 20 = 2 m wide per sqrt(km^2)), clamped 8..1024. */
export function rivers(h, w, hh, dx, opts = {}) {
  const F = opts.flow || flow(h, w, hh, dx, opts);
  const {rcv, A, stack} = F, N = w * hh;
  const thr = Math.max(2, opts.threshold || 200);
  const minLen = opts.minLength == null ? 4 : opts.minLength;
  const widthK = opts.widthK == null ? 20 : opts.widthK;
  // channel donors of each channel cell, largest first
  const chan = new Uint8Array(N);
  for (let n = 0; n < N; n++) if (A[n] >= thr) chan[n] = 1;
  const dons = new Map();
  for (const n of stack) {
    const r = rcv[n];
    if (r === n || !chan[n] || !chan[r]) continue;
    let l = dons.get(r);
    if (!l) { l = []; dons.set(r, l); }
    l.push(n);
  }
  for (const l of dons.values()) l.sort((a, b) => A[b] - A[a] || a - b);
  const out = [];
  // outlets with a channel draining into them, biggest first
  const mouths = [];
  for (let n = 0; n < N; n++) if (chan[n] && rcv[n] === n) mouths.push(n);
  for (let n = 0; n < N; n++) if (chan[n] && rcv[n] !== n && !chan[rcv[n]]) mouths.push(n);
  mouths.sort((a, b) => A[b] - A[a] || a - b);
  const todo = mouths.map(m => ({cell: m, joins: null, at: null}));
  const wid = a => Math.max(8, Math.min(1024, widthK * Math.sqrt(a * dx * dx / 1e6)));
  while (todo.length) {
    const t = todo.shift();
    // walk upstream along the largest donor; every other donor is a tributary
    const path = [t.cell], branches = [];
    let c = t.cell;
    for (;;) {
      const l = dons.get(c);
      if (!l || !l.length) break;
      for (let k = 1; k < l.length; k++) branches.push(l[k]);
      c = l[0];
      path.push(c);
    }
    if (path.length < minLen) continue;
    const id = (opts.prefix || 'river_') + out.length;
    const pts = path.reverse().map(n => [n % w + 0.5, ((n / w) | 0) + 0.5, A[n]]);
    if (t.at != null) pts.push([t.at % w + 0.5, ((t.at / w) | 0) + 0.5, A[t.at]]);
    const mouthCell = t.cell;
    out.push({id, points: pts, joins: t.joins, mouth: rcv[mouthCell] === mouthCell ? (F.mask && F.mask[mouthCell] ? 'outlet' : 'pit') : 'outlet',
              A: A[mouthCell], width: wid(A[mouthCell])});
    for (const b of branches) todo.push({cell: b, joins: id, at: rcv[b]});
    todo.sort((a, b) => A[b.cell] - A[a.cell] || a.cell - b.cell);
  }
  // simplify + smooth the polylines (in grid units), carrying A as the third value
  const tol = opts.tolerance == null ? 0.6 : opts.tolerance, ch = opts.chaikin == null ? 2 : opts.chaikin;
  for (const R of out) {
    const pl = simplify(R.points, tol);
    const sm = chaikin(pl.map(p => [p[0], p[1]]), ch, false);
    // re-attach A by nearest original point (monotone along the path)
    let k = 0;
    R.line = sm.map(p => {
      while (k + 1 < R.points.length && Math.hypot(R.points[k + 1][0] - p[0], R.points[k + 1][1] - p[1]) <= Math.hypot(R.points[k][0] - p[0], R.points[k][1] - p[1])) k++;
      return [p[0], p[1], R.points[k][2]];
    });
    R.widths = R.line.map(p => wid(p[2]));
  }
  return out;
}

/* ---- erosion ----------------------------------------------------------------------------- */
/** Talus slumping: every cell steeper than `talus` (rise per unit run) toward a
 *  lower 8-neighbour sheds a part of the excess, Jacobi (one snapshot per
 *  pass). `fixed` cells neither give nor take. Mass-conserving. */
export function thermal(h, w, hh, dx, talusDeg, passes = 1, opts = {}) {
  const T = Math.tan(Math.max(1, Math.min(89, talusDeg)) * Math.PI / 180) * dx;
  const k = opts.rate == null ? 0.5 : opts.rate, fixed = opts.fixed || null, sink = opts.fixedSink === true;
  const N = w * hh, d = opts.scratch && opts.scratch.length === N ? opts.scratch : new Float64Array(N);
  const ex = new Float64Array(8), TD = ND.map(v => v * T);
  for (let p = 0; p < passes; p++) {
    d.fill(0);
    for (let j = 0; j < hh; j++)
      for (let i = 0; i < w; i++) {
        const n = j * w + i;
        if (fixed && fixed[n]) continue;
        const hn = h[n];
        let sum = 0, mx = 0;
        for (let q = 0; q < 8; q++) {
          ex[q] = 0;
          const ii = i + NX[q], jj = j + NZ[q];
          if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
          const nb = jj * w + ii;
          if (fixed && fixed[nb] && !sink) continue;
          const e = hn - h[nb] - TD[q];
          if (e > 0) { ex[q] = e; sum += e; if (e > mx) mx = e; }
        }
        if (sum <= 0) continue;
        const move = k * mx * 0.5;
        d[n] -= move;
        const f = move / sum;
        for (let q = 0; q < 8; q++) if (ex[q] > 0) d[(j + NZ[q]) * w + i + NX[q]] += ex[q] * f;
      }
    for (let n = 0; n < N; n++) h[n] += d[n];
  }
  return h;
}

/** The implicit stream-power solver (Braun & Willett 2013, m = 0.5, n = 1):
 *  each iteration uplifts every free cell by U * dt, routes D8 on the
 *  (periodically pit-filled) surface, accumulates area, then walks the stack
 *  from the outlets up:  h[i] = (h[i] + f * h[r]) / (1 + f),
 *  f = K[i] * dt * A[i]^m / dist[i]  (A in cells, dist in cells). A cell never
 *  drops below its receiver, so the update is unconditionally stable. Then a
 *  thermal pass caps slopes at `talusDeg`.
 *  h: Float32Array/Float64Array, modified IN PLACE (and returned).
 *  opts = {iters, dt, K (number | array), U (number | array), m, fixed
 *  (Uint8Array: base-level cells that never move, e.g. outside a range's
 *  footprint), seaY (cells below are outlets), talusDeg (0 = off),
 *  thermalPasses, fillEvery, progress(fraction)}. */
export function streamPower(h, w, hh, dx, opts = {}) {
  const N = w * hh;
  const iters = Math.max(0, opts.iters | 0), dt = opts.dt == null ? 1 : +opts.dt, m = opts.m == null ? 0.5 : +opts.m;
  const Kc = typeof opts.K === 'number' || opts.K == null ? (opts.K == null ? 1e-3 : opts.K) : null;
  const Uc = typeof opts.U === 'number' || opts.U == null ? (opts.U || 0) : null;
  const fixed = opts.fixed || null;
  const mask = outletMask(h, w, hh, {seaY: opts.seaY, outlets: fixed, edges: opts.edges});
  const fillEvery = Math.max(1, opts.fillEvery || 8), thermalEvery = Math.max(1, opts.thermalEvery || 3);
  const scratch = {}, tscr = new Float64Array(N);
  const talus = opts.talusDeg || 0, tp = opts.thermalPasses == null ? 2 : opts.thermalPasses;
  const z = h;
  let rcv = null, dist = null, stack = null, A = null;
  const pow = m === 0.5 ? Math.sqrt : (a => Math.pow(a, m));
  for (let it = 0; it < iters; it++) {
    if (Uc !== 0) for (let n = 0; n < N; n++) if (!mask[n]) z[n] += (Uc != null ? Uc : opts.U[n]) * dt;
    // a CEILING (the designed envelope of a range): the land rises into it and
    // no further, so the crest heights are the design and erosion only cuts down
    if (opts.ceiling) for (let n = 0; n < N; n++) if (z[n] > opts.ceiling[n]) z[n] = opts.ceiling[n];
    if (it % fillEvery === 0) {
      const f = priorityFlood(z, w, hh, {mask, epsilon: 1e-3});
      for (let n = 0; n < N; n++) if (f[n] > z[n]) z[n] = f[n];
    }
    ({rcv, dist} = d8(z, w, hh, 1, mask, opts.jitter || 0, opts.seed || 0, scratch));
    stack = stackOrder(rcv);
    A = accumulate(rcv, stack, opts.weight || null);
    for (let k = 0; k < N; k++) {
      const n = stack[k], r = rcv[n];
      if (r === n || mask[n]) continue;
      const K = Kc != null ? Kc : opts.K[n];
      const f = K * dt * pow(A[n]) / dist[n];
      const hn = (z[n] + f * z[r]) / (1 + f);
      if (hn < z[n]) z[n] = hn;
    }
    // hillslope diffusion (opts.diffuse, fading to 0 by opts.diffuseUntil of the run)
    if (opts.diffuse > 0) {
      const fr = it / iters, until = opts.diffuseUntil == null ? 1 : opts.diffuseUntil;
      if (fr < until) diffuse(z, w, hh, mask, opts.diffuse * (1 - fr / until), opts.diffusePasses || 1);
    }
    if (talus > 0 && tp > 0 && (it % thermalEvery === thermalEvery - 1 || it === iters - 1)) thermal(z, w, hh, dx, talus, tp, {fixed: mask, rate: 0.5, scratch: tscr});
    if (opts.progress && (it % 4 === 3 || it === iters - 1)) opts.progress((it + 1) / iters);
  }
  return {h: z, A, rcv, stack};
}

/** Laplacian smoothing (Jacobi, 4 neighbours) of the free cells, `passes` times. */
export function diffuse(z, w, hh, mask, k, passes = 1) {
  const N = w * hh, t = new Float64Array(N);
  for (let p = 0; p < passes; p++) {
    for (let j = 0; j < hh; j++)
      for (let i = 0; i < w; i++) {
        const n = j * w + i;
        if (mask[n] || i === 0 || j === 0 || i === w - 1 || j === hh - 1) { t[n] = z[n]; continue; }
        t[n] = z[n] + k * 0.25 * (z[n - 1] + z[n + 1] + z[n - w] + z[n + w] - 4 * z[n]);
      }
    for (let n = 0; n < N; n++) z[n] = t[n];
  }
  return z;
}
/** The STEADY STATE of uplift vs stream power (Cordonnier et al. 2016's
 *  fixed point): with dh/dt = U - K A^m S = 0 every cell sits exactly
 *  U dist / (K A^m) above its receiver, so given a drainage network the whole
 *  surface follows in one pass up the stack. The network depends on the
 *  surface, so iterate: route D8 on the (epsilon-filled) surface, rebuild,
 *  relax toward it; a few tens of rounds and the divides settle into a
 *  dendritic net whose valleys reach the crest. Base-level cells (`fixed`,
 *  the grid edge, cells below seaY) keep their height.
 *  h is modified IN PLACE. opts = {U (array), K (number | array), m, fixed,
 *  seaY, iters, relax (0..1, the step toward the new surface), talusDeg (cap
 *  the slope to a receiver at this angle; 0 = none), progress}. */
export function steadyState(h, w, hh, dx, opts = {}) {
  const N = w * hh, m = opts.m == null ? 0.5 : +opts.m;
  const iters = Math.max(1, opts.iters | 0 || 20), relax = opts.relax == null ? 0.5 : +opts.relax;
  const fixed = opts.fixed || null;
  const mask = outletMask(h, w, hh, {seaY: opts.seaY, outlets: fixed, edges: opts.edges});
  const Kc = typeof opts.K === 'number' || opts.K == null ? (opts.K == null ? 1 : opts.K) : null;
  const cap = opts.talusDeg ? Math.tan(opts.talusDeg * Math.PI / 180) : Infinity;
  const pow = m === 0.5 ? Math.sqrt : (a => Math.pow(a, m));
  const hillA = opts.hillA || 0, hillS = Math.tan((opts.hillDeg || 33) * Math.PI / 180), hillU = opts.hillU || 1e9;
  const z = h, base = Float64Array.from(h), nz = new Float64Array(N);
  let A = null, rcv = null, stack = null, U = Float64Array.from(opts.U);
  for (let it = 0; it < iters; it++) {
    const f = priorityFlood(z, w, hh, {mask, epsilon: 1e-3});
    let r;
    ({rcv, dist: r} = d8(f, w, hh, dx, mask, opts.jitter || 0, opts.seed || 0));
    stack = stackOrder(rcv);
    A = accumulate(rcv, stack, opts.weight || null);
    for (let k = 0; k < N; k++) {
      const n = stack[k], q = rcv[n];
      if (q === n || mask[n]) { nz[n] = base[n]; continue; }
      const K = Kc != null ? Kc : opts.K[n];
      let rise = U[n] * r[n] / (K * pow(A[n]));
      // THRESHOLD HILLSLOPES: above the channel heads (A < hillA) the ground is
      // a planar slope at hillSlope (times the local uplift share), not a channel
      if (hillA > 0 && A[n] < hillA) rise = Math.min(rise, hillS * r[n] * Math.min(1, U[n] * hillU));
      if (rise > cap * r[n]) rise = cap * r[n];
      nz[n] = nz[q] + rise;
    }
    // CALIBRATION (opts.targetMax): rescale the uplift so the highest relief
    // is the target -- the solution is linear in U while no slope rule binds,
    // so the hillslope / talus angles below are real angles, not arbitrary units
    if (opts.targetMax > 0) {
      let mx = 0;
      for (let n = 0; n < N; n++) if (!mask[n] && nz[n] - base[n] > mx) mx = nz[n] - base[n];
      if (mx > 1e-6) {
        const sc = opts.targetMax / mx;
        for (let n = 0; n < N; n++) if (!mask[n]) nz[n] = base[n] + (nz[n] - base[n]) * sc;
        U = U.map ? U.map(v => v * sc) : U;
      }
    }
    // hillslope DIFFUSION (fading out over the run): it smooths away the
    // smallest valleys, which sets the drainage spacing and makes the flow
    // gather into fewer, bigger, branching valleys
    const kd = (opts.diffuse || 0) * (1 - it / iters);
    if (kd > 0) diffuse(nz, w, hh, mask, kd, opts.diffusePasses || 2);
    const a = it === iters - 1 ? 1 : relax;
    for (let n = 0; n < N; n++) z[n] = z[n] + (nz[n] - z[n]) * a;
    if (opts.progress) opts.progress((it + 1) / iters);
  }
  return {h: z, A, rcv, stack};
}

/* ---- the coarse proxy --------------------------------------------------------------------- */
/** Box-average by an integer factor f: ceil(w/f) x ceil(hh/f). */
export function downsample(h, w, hh, f) {
  const cw = Math.ceil(w / f), ch = Math.ceil(hh / f), out = new Float32Array(cw * ch);
  for (let cj = 0; cj < ch; cj++)
    for (let ci = 0; ci < cw; ci++) {
      let s = 0, c = 0;
      for (let j = cj * f; j < Math.min(hh, cj * f + f); j++)
        for (let i = ci * f; i < Math.min(w, ci * f + f); i++) { s += h[j * w + i]; c++; }
      out[cj * cw + ci] = s / c;
    }
  return {h: out, w: cw, hh: ch};
}
/** Sample a coarse grid (cell k covers [k*f, (k+1)*f) of the fine one, its
 *  value at the cell centre) at fine (fi, fj) with Catmull-Rom bicubic. */
export function bicubicAt(c, cw, ch, x, z) {
  const i = Math.floor(x), j = Math.floor(z), u = x - i, v = z - j;
  const cr = (p0, p1, p2, p3, t) => p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
  const g = (ii, jj) => c[Math.max(0, Math.min(ch - 1, jj)) * cw + Math.max(0, Math.min(cw - 1, ii))];
  const r = [];
  for (let q = -1; q <= 2; q++) r.push(cr(g(i - 1, j + q), g(i, j + q), g(i + 1, j + q), g(i + 2, j + q), u));
  return cr(r[0], r[1], r[2], r[3], v);
}
/** The coarse grid back at fine resolution w x hh (bicubic). */
export function upsample(c, cw, ch, f, w, hh) {
  const out = new Float32Array(w * hh);
  for (let j = 0; j < hh; j++) {
    const z = (j + 0.5) / f - 0.5;
    for (let i = 0; i < w; i++) out[j * w + i] = bicubicAt(c, cw, ch, (i + 0.5) / f - 0.5, z);
  }
  return out;
}
/** Run `fn(coarse, cw, ch)` on a proxy f times coarser and bring the CHANGE
 *  back: fine + upsample(after - before). The fine detail the proxy cannot
 *  hold is kept, the proxy's valleys and ridges are added. */
export function viaProxy(h, w, hh, f, fn) {
  if (f <= 1) { const c = Float32Array.from(h); fn(c, w, hh); return c; }
  const {h: c0, w: cw, hh: ch} = downsample(h, w, hh, f);
  const c1 = Float32Array.from(c0);
  fn(c1, cw, ch);
  const d = new Float32Array(c0.length);
  for (let n = 0; n < d.length; n++) d[n] = c1[n] - c0[n];
  const up = upsample(d, cw, ch, f, w, hh), out = new Float32Array(w * hh);
  for (let n = 0; n < out.length; n++) out[n] = h[n] + up[n];
  return out;
}
