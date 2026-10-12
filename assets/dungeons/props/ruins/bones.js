/* ruins/bones — a skeleton laid out, a scatter, or a heap. Long axis Z. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'bones', kit: 'ruins', label: 'Bones', version: 1,
  SCHEMA: [
    {k: 'layout', n: 'layout', kind: 'enum', options: ['laid', 'scatter', 'heap'], def: 'scatter', d: 'laid = a whole skeleton on its back; scatter = disturbed; heap = an ossuary pile.'},
    {k: 'count', n: 'bones', min: 1, max: 30, step: 1, int: true, def: 8, d: 'Scatter / heap only.'},
    {k: 'skulls', n: 'skulls', min: 0, max: 6, step: 1, int: true, def: 1, d: ''}
  ],
  PRESETS: {skeleton: {params: {layout: 'laid'}}, scatter: {params: {}}, ossuary: {params: {layout: 'heap', count: 24, skulls: 4}}},
  draw(g, P, seed) {
    const skull = (x, y, z) => { g.box(x - 1, y, z - 1, x + 2, y + 3, z + 2, 'bone'); g.set(x - 1, y + 1, z + 1, '#carve'); g.set(x + 1, y + 1, z + 1, '#carve'); };
    if (P.layout === 'laid') {
      skull(0, 0, 7);
      g.box(0, 0, -1, 1, 1, 6, 'bone');                                      // spine
      for (let z = 1; z < 5; z++) { g.set(-2, 0, z, 'bone'); g.set(2, 0, z, 'bone'); g.set(-1, 1, z, 'bone'); g.set(1, 1, z, 'bone'); }
      g.box(-3, 0, -2, 4, 1, -1, 'bone');                                    // pelvis
      for (const x of [-2, 2]) { g.box(x, 0, -8, x + 1, 1, -2, 'bone'); g.box(x * 2, 0, 0, x * 2 + 1, 1, 5, 'bone'); }
      return;
    }
    const heap = P.layout === 'heap';
    const R = heap ? 3 + Math.round(P.count / 6) : 6;
    for (let i = 0; i < P.count; i++) {
      const x = Math.round((frand(seed, 'b', i, 'x') * 2 - 1) * R), z = Math.round((frand(seed, 'b', i, 'z') * 2 - 1) * R);
      const y = heap ? Math.max(0, Math.round((R - Math.hypot(x, z)) * 0.7)) : 0;
      const len = 2 + Math.round(frand(seed, 'b', i, 'l') * 4);
      if (frand(seed, 'b', i, 'ax') < 0.5) g.box(x, y, z, x + len, y + 1, z + 1, 'bone'); else g.box(x, y, z, x + 1, y + 1, z + len, 'bone');
      if (heap) for (let yy = 0; yy < y; yy++) g.set(x, yy, z, 'bone');
    }
    for (let k = 0; k < P.skulls; k++) {
      const x = Math.round((frand(seed, 's', k, 'x') * 2 - 1) * (R - 1)), z = Math.round((frand(seed, 's', k, 'z') * 2 - 1) * (R - 1));
      skull(x, heap ? Math.max(0, Math.round((R - Math.hypot(x, z)) * 0.7)) : 0, z);
    }
  }
});
