/* ruins/banner — cloth hung from a rod on a wall (wall at z = -1), hanging DOWN from y = 0. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'banner', kit: 'ruins', label: 'Banner', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 0.6, max: 6, step: 0.1, def: 2.4, d: ''},
    {k: 'width', n: 'width (m)', min: 0.4, max: 2.5, step: 0.1, def: 1, d: ''},
    {k: 'emblem', n: 'emblem', kind: 'enum', options: ['stripe', 'chevron', 'disc', 'none'], def: 'chevron', d: 'In the palette accent.'},
    {k: 'tattered', n: 'tattered', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Rotted holes and a ragged hem.'}
  ],
  PRESETS: {banner: {params: {}}, long_pennant: {params: {length: 5, width: 0.7, emblem: 'stripe'}}, rag: {params: {tattered: 0.85, emblem: 'none'}}},
  draw(g, P, seed) {
    const L = Math.round(P.length * 10), hw = Math.round(P.width * 5);
    g.box(-hw - 1, -1, 0, hw + 2, 0, 1, 'timber');
    g.box(-hw - 1, -1, -1, -hw, 0, 0, 'iron'); g.box(hw + 1, -1, -1, hw + 2, 0, 0, 'iron');
    for (let y = -L; y < -1; y++) for (let x = -hw; x <= hw; x++) {
      const fromBot = y + L;
      const hem = Math.round(frand(seed, 'hem', x) * P.tattered * 6);
      if (fromBot < hem) continue;
      if (frand(seed, 'hole', x, y) < P.tattered * 0.12) continue;
      let mat = '@cloth';
      const u = Math.abs(x) / Math.max(1, hw), v = (-y) / L;
      if (P.emblem === 'stripe' && Math.abs(x) <= Math.max(1, Math.round(hw / 4))) mat = '@accent';
      else if (P.emblem === 'chevron' && Math.abs(v - 0.55 - u * 0.2) < 0.05) mat = '@accent';
      else if (P.emblem === 'disc' && Math.hypot(x, (y + L * 0.45) ) < hw * 0.55) mat = '@accent';
      g.set(x, y, 0, mat);
    }
  }
});
