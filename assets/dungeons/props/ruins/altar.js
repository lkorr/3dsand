/* ruins/altar — a dressed stone altar (palette trim), cloth runner, candles. Long axis X. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'altar', kit: 'ruins', label: 'Altar', version: 1,
  SCHEMA: [
    {k: 'width', n: 'width (m)', min: 0.8, max: 3, step: 0.1, def: 1.8, d: ''},
    {k: 'height', n: 'height (m)', min: 0.6, max: 1.4, step: 0.05, def: 1, d: ''},
    {k: 'cloth', n: 'altar cloth', kind: 'bool', def: true, d: 'A runner over the top (burns).'},
    {k: 'candles', n: 'candles', min: 0, max: 8, step: 1, int: true, def: 2, d: ''},
    {k: 'cracked', n: 'cracked', kind: 'bool', def: false, d: 'Split across the middle, a corner fallen.'}
  ],
  PRESETS: {altar: {params: {}}, plain_block: {params: {cloth: false, candles: 0, width: 1.2}}, desecrated: {params: {cracked: true, cloth: false, candles: 1}}},
  draw(g, P, seed) {
    const hw = Math.round(P.width * 5), H = Math.round(P.height * 10), d = 4;
    g.box(-hw - 1, 0, -d - 1, hw + 1, 2, d + 1, '@trim');
    g.box(-hw + 1, 2, -d + 1, hw - 1, H - 1, d - 1, '@trim');
    g.box(-hw, H - 1, -d, hw, H, d, '@trim');
    g.box(-hw + 2, 3, d - 1, hw - 2, H - 3, d, '@accent');      // inlaid front panel
    if (P.cracked) {
      g.box(-1, 2, -d - 1, 1, H + 1, d + 1, '#carve');
      g.box(hw - 3, H - 3, d - 3, hw + 1, H + 1, d + 1, '#carve');
      g.box(hw + 1, 0, d - 1, hw + 4, 2, d + 2, '@trim');
    } else if (P.cloth) g.box(-Math.round(hw / 3), H, -d, Math.round(hw / 3), H + 1, d + 1, '@cloth');
    for (let i = 0; i < P.candles; i++) {
      const x = -hw + 2 + Math.round(((2 * hw - 4) * (i + 0.5)) / P.candles), z = -2 + Math.round(frand(seed, 'cz', i) * 3);
      const h = 1 + Math.round(frand(seed, 'ch', i) * 2);
      g.box(x, H, z, x + 1, H + h, z + 1, 'tallow');
      g.set(x, H + h, z, '@light');
    }
  }
});
