/* mines/pickaxe_rack — a timber rack against the wall (z = -1) with picks and shovels. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'pickaxe_rack', kit: 'mines', label: 'Pickaxe rack', version: 1,
  SCHEMA: [
    {k: 'tools', n: 'tools', min: 0, max: 6, step: 1, int: true, def: 3, d: ''},
    {k: 'width', n: 'width (m)', min: 0.6, max: 2.5, step: 0.1, def: 1.2, d: ''}
  ],
  PRESETS: {rack: {params: {}}, full: {params: {tools: 6, width: 2}}, bare: {params: {tools: 0}}},
  draw(g, P, seed) {
    const hw = Math.round(P.width * 5);
    for (const x of [-hw, hw - 1]) g.box(x, 0, -1, x + 1, 14, 0, 'timber');
    g.box(-hw, 12, -1, hw, 13, 0, 'timber');
    g.box(-hw, 4, -1, hw, 5, 0, 'timber');
    for (let i = 0; i < P.tools; i++) {
      const x = -hw + 2 + Math.round(((2 * hw - 4) * (i + 0.5)) / P.tools);
      g.box(x, 1, 0, x + 1, 12, 1, 'timber');
      if (frand(seed, 'tool', i) < 0.3) g.box(x - 1, 1, 0, x + 2, 4, 1, 'iron');          // shovel
      else { g.box(x - 2, 11, 0, x + 3, 12, 1, 'iron'); g.set(x - 3, 10, 0, 'iron'); g.set(x + 3, 10, 0, 'iron'); }
    }
  }
});
