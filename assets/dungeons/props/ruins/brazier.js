/* ruins/brazier — a bowl on a tripod (iron) or a stone pedestal, flame in the bowl. */
import { defineProp, frand, LIT_ROW } from '../propkit.js';
export default defineProp({
  name: 'brazier', kit: 'ruins', label: 'Brazier', version: 1,
  SCHEMA: [LIT_ROW,
    {k: 'style', n: 'style', kind: 'enum', options: ['iron', 'stone'], def: 'stone', d: 'iron = a tripod; stone = a pedestal in palette trim.'},
    {k: 'height', n: 'height (m)', min: 0.4, max: 2, step: 0.05, def: 1, d: ''},
    {k: 'bowl', n: 'bowl radius (m)', min: 0.2, max: 0.8, step: 0.05, def: 0.35, d: ''}],
  PRESETS: {pedestal: {params: {}}, tripod: {params: {style: 'iron', height: 0.9}}, great: {params: {height: 1.6, bowl: 0.7}}, cold: {params: {lit: 'none'}}},
  draw(g, P, seed) {
    const H = Math.round(P.height * 10), r = P.bowl * 10, mat = P.style === 'iron' ? 'iron' : '@trim';
    if (P.style === 'iron') for (const [x, z] of [[-2, -2], [2, -2], [0, 2]]) g.line([x * 2, 0, z * 2], [x, H - 1, z], 'iron');
    else { g.box(-3, 0, -3, 4, 2, 4, mat); g.cylY(0, 0, 2, H - 1, 1.6, mat); }
    g.cylY(0, 0, H - 1, H, r, mat);
    g.cylY(0, 0, H, H + 2, r, mat);
    g.cylY(0, 0, H, H + 2, r - 1, '#carve');
    g.cylY(0, 0, H, H + 1, r - 1, 'cinder');
    if (P.lit !== 'none') {
      const n = Math.max(2, Math.round(r));
      for (let i = 0; i < n; i++) {
        const x = Math.round((frand(seed, 'fl', i, 'x') * 2 - 1) * (r - 2)), z = Math.round((frand(seed, 'fl', i, 'z') * 2 - 1) * (r - 2));
        g.box(x, H + 1, z, x + 1, H + 2 + Math.round(frand(seed, 'fl', i, 'h') * 2), z + 1, '@' + P.lit);
      }
    }
  }
});
