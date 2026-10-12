/* mines/table — plank table on timber legs, benches either side. Long axis X. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'table', kit: 'mines', label: 'Table', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 0.8, max: 3, step: 0.1, def: 1.8, d: ''},
    {k: 'width', n: 'width (m)', min: 0.5, max: 1.2, step: 0.1, def: 0.8, d: ''},
    {k: 'benches', n: 'benches', kind: 'bool', def: true, d: ''},
    {k: 'clutter', n: 'clutter', min: 0, max: 1, step: 0.05, def: 0.4, d: 'Tallow stubs and tin mugs on top.'}
  ],
  PRESETS: {mess_table: {params: {}}, small: {params: {length: 1, benches: false}}, long: {params: {length: 3, clutter: 0.8}}},
  draw(g, P, seed) {
    const hl = Math.round(P.length * 5), hw = Math.round(P.width * 5);
    g.box(-hl, 7, -hw, hl, 8, hw, 'plank');
    for (const x of [-hl + 1, hl - 2]) for (const z of [-hw + 1, hw - 2]) g.box(x, 0, z, x + 1, 7, z + 1, 'timber');
    if (P.benches) for (const s of [-1, 1]) {
      const z = s * (hw + 3);
      g.box(-hl + 1, 4, z - 1, hl - 1, 5, z + 1, 'plank');
      for (const x of [-hl + 2, hl - 3]) g.box(x, 0, z, x + 1, 4, z + 1, 'timber');
    }
    for (let x = -hl + 2; x < hl - 2; x += 3) {
      const r = frand(seed, 'clutter', x);
      if (r < P.clutter * 0.5) g.set(x, 8, 0, 'tallow');
      else if (r < P.clutter) g.set(x, 8, -2, 'iron');
    }
  }
});
