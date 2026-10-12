/* caves/driftwood — what the sea leaves in a sea cave: a bleached log or two
 * lying on the beach, a snag of root at one end, kelp tangled over them. Lies
 * flat on the floor (y 0), a little sunk (`sink`) so it reads as bedded in the
 * sand rather than balanced on it. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'driftwood', kit: 'caves', label: 'Driftwood', version: 1,
  SCHEMA: [
    {k: 'length', n: 'log length (m)', min: 0.6, max: 6, step: 0.1, def: 2.4, d: ''},
    {k: 'girth', n: 'log radius (m)', min: 0.1, max: 0.5, step: 0.05, def: 0.2, d: ''},
    {k: 'logs', n: 'logs', min: 1, max: 4, step: 1, int: true, def: 2, d: 'Logs in the heap (each shorter, crossed over the first).'},
    {k: 'root', n: 'root snag', kind: 'bool', def: true, d: 'A splay of root at the butt of the first log.'},
    {k: 'kelp', n: 'kelp', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Kelp draped over the logs.'},
    {k: 'sink', n: 'bedded (vox)', min: 0, max: 3, step: 1, int: true, def: 1, d: 'How deep the logs sit in the floor.'}
  ],
  PRESETS: {log: {params: {logs: 1, kelp: 0.2}}, heap: {params: {}}, tangle: {params: {logs: 4, length: 3.5, kelp: 0.8}}},
  draw(g, P, seed) {
    const r0 = Math.max(1, P.girth * 10);
    for (let k = 0; k < P.logs; k++) {
      const L = Math.max(4, Math.round(P.length * 10 * (1 - 0.18 * k)));
      const a = k === 0 ? 0 : (frand(seed, 'ang', k) * 2 - 1) * 1.1;
      const r = Math.max(1, r0 * (1 - 0.15 * k));
      const cy = Math.round(r) - P.sink + (k > 0 ? Math.round(r0 * 1.4) : 0);
      const ox = k === 0 ? 0 : Math.round((frand(seed, 'ox', k) * 2 - 1) * L * 0.25);
      const ex = Math.cos(a) * L / 2, ez = Math.sin(a) * L / 2;
      const n = Math.ceil(L);
      for (let i = 0; i <= n; i++) {
        const t = i / n - 0.5;
        const rr = r * (1 - 0.25 * Math.abs(t));
        const x = ox + t * 2 * ex, z = t * 2 * ez;
        const R = Math.ceil(rr);
        for (let dy = -R; dy <= R; dy++) for (let d2 = -R; d2 <= R; d2++) {
          if (dy * dy + d2 * d2 > rr * rr + 0.25) continue;
          const y = cy + dy;
          if (y < -P.sink) continue;
          g.set(x - Math.sin(a) * d2, y, z + Math.cos(a) * d2, frand(seed, 'w', k, i, dy, d2) < 0.08 ? 'timber' : 'wood');
        }
      }
      if (k === 0 && P.root) {
        for (let s = 0; s < 6; s++) {
          const b = (s / 6) * Math.PI * 2 + frand(seed, 'root', s);
          const len = r0 * (1.5 + frand(seed, 'rl', s) * 2);
          g.line([Math.round(-ex), Math.round(cy), Math.round(-ez)], [Math.round(-ex - Math.cos(a) * len * 0.6), Math.max(0, Math.round(cy + Math.sin(b) * len)), Math.round(-ez + Math.cos(b) * len)], 'wood');
        }
      }
    }
    // Kelp: strands lying over whatever is highest in each column.
    if (P.kelp > 0) {
      const cols = new Map();
      for (const c of g.cells()) { const k = c.x + ',' + c.z; if (!cols.has(k) || cols.get(k).y < c.y) cols.set(k, c); }
      for (const c of cols.values()) if (frand(seed, 'kelp', c.x, c.z) < P.kelp * 0.5) g.set(c.x, c.y + 1, c.z, 'kelp');
    }
  }
});
