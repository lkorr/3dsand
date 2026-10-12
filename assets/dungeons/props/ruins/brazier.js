/* ruins/brazier — a bowl on a tripod (iron) or a stone pedestal, flame in the bowl. */
import { defineProp, frand, LIT_ROW } from '../propkit.js';
export default defineProp({
  name: 'brazier', kit: 'ruins', label: 'Brazier', version: 1,
  SCHEMA: [LIT_ROW,
    {k: 'style', n: 'style', kind: 'enum', options: ['iron', 'stone'], def: 'stone', d: 'iron = a tripod; stone = a pedestal in palette trim.'},
    {k: 'height', n: 'height (m)', min: 0.4, max: 2, step: 0.05, def: 1, d: ''},
    {k: 'bowl', n: 'bowl radius (m)', min: 0.2, max: 0.8, step: 0.05, def: 0.35, d: ''},
    {k: 'fire', n: 'fire', min: 0, max: 1, step: 0.05, def: 0, d: 'How full the bowl burns: 0 = a few flame wisps; 1 = the whole bowl a bed of flame with tall tongues (a glowing surface the size of the bowl lights a room).'}],
  PRESETS: {pedestal: {params: {}}, tripod: {params: {style: 'iron', height: 0.9}}, great: {params: {height: 1.6, bowl: 0.7}}, cold: {params: {lit: 'none'}}, blazing: {params: {height: 1.2, bowl: 0.6, fire: 1}}},
  draw(g, P, seed) {
    const H = Math.round(P.height * 10), r = P.bowl * 10, mat = P.style === 'iron' ? 'iron' : '@trim';
    if (P.style === 'iron') for (const [x, z] of [[-2, -2], [2, -2], [0, 2]]) g.line([x * 2, 0, z * 2], [x, H - 1, z], 'iron');
    else { g.box(-3, 0, -3, 4, 2, 4, mat); g.cylY(0, 0, 2, H - 1, 1.6, mat); }
    g.cylY(0, 0, H - 1, H, r, mat);
    g.cylY(0, 0, H, H + 2, r, mat);
    g.cylY(0, 0, H, H + 2, r - 1, '#carve');
    g.cylY(0, 0, H, H + 1, r - 1, 'cinder');
    if (P.lit !== 'none' && P.fire > 0) {
      // the bed of flame fills the bowl, then taller tongues
      g.cylY(0, 0, H, H + 1 + Math.round(P.fire), r - 1, '@' + P.lit);
      const nt = Math.round(r * (1 + P.fire * 2));
      for (let i = 0; i < nt; i++) {
        const a = frand(seed, 'tg', i, 'a') * Math.PI * 2, d = Math.sqrt(frand(seed, 'tg', i, 'd')) * (r - 2);
        const x = Math.round(Math.cos(a) * d), z = Math.round(Math.sin(a) * d);
        g.box(x, H + 1, z, x + 1, H + 3 + Math.round(frand(seed, 'tg', i, 'h') * (2 + P.fire * 5)), z + 1, '@' + P.lit);
      }
    } else if (P.lit !== 'none') {
      const n = Math.max(2, Math.round(r));
      for (let i = 0; i < n; i++) {
        const x = Math.round((frand(seed, 'fl', i, 'x') * 2 - 1) * (r - 2)), z = Math.round((frand(seed, 'fl', i, 'z') * 2 - 1) * (r - 2));
        g.box(x, H + 1, z, x + 1, H + 2 + Math.round(frand(seed, 'fl', i, 'h') * 2), z + 1, '@' + P.lit);
      }
    }
  }
});
