/* mines/bunk — a timber bunk, one to three tiers, straw mattresses. Long axis X. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'bunk', kit: 'mines', label: 'Bunk', version: 1,
  SCHEMA: [
    {k: 'tiers', n: 'tiers', min: 1, max: 3, step: 1, int: true, def: 2, d: ''},
    {k: 'length', n: 'length (m)', min: 1.6, max: 2.4, step: 0.1, def: 2, d: ''},
    {k: 'blanket', n: 'blankets', min: 0, max: 1, step: 0.05, def: 0.6, d: 'Chance each mattress has a blanket.'}
  ],
  PRESETS: {double: {params: {}}, single: {params: {tiers: 1}}, triple: {params: {tiers: 3, blanket: 0.2}}},
  draw(g, P, seed) {
    const hl = Math.round(P.length * 5), w = 4, gap = 9;
    const top = 3 + (P.tiers - 1) * gap + 6;
    for (const x of [-hl, hl - 1]) for (const z of [-w, w]) g.box(x, 0, z, x + 1, top, z + 1, 'timber');
    for (let t = 0; t < P.tiers; t++) {
      const y = 3 + t * gap;
      g.box(-hl, y, -w, hl, y + 1, w + 1, 'plank');
      g.box(-hl + 1, y + 1, -w + 1, hl - 1, y + 2, w, 'straw_bed');
      if (frand(seed, 'blanket', t) < P.blanket) g.box(-Math.round(hl / 3), y + 2, -w + 1, hl - 1, y + 3, w, 'cloth');
    }
  }
});
