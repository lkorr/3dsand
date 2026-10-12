/* ruins/candle_cluster — tallow candles of seeded heights, melted together. */
import { defineProp, frand, LIT_ROW, MOUNT_ROW } from '../propkit.js';
export default defineProp({
  name: 'candle_cluster', kit: 'ruins', label: 'Candle cluster', version: 1,
  SCHEMA: [LIT_ROW, MOUNT_ROW,
    {k: 'count', n: 'candles', min: 1, max: 20, step: 1, int: true, def: 6, d: ''},
    {k: 'spread', n: 'spread (m)', min: 0.1, max: 1, step: 0.05, def: 0.3, d: ''},
    {k: 'tallest', n: 'tallest (m)', min: 0.1, max: 0.6, step: 0.05, def: 0.35, d: ''},
    {k: 'flame', n: 'flame size', min: 0, max: 1, step: 0.05, def: 0, d: '0 = one-voxel flames; 1 = taller flames, and the melted mass glows where the wicks crowd (a vigil that lights its alcove).'}],
  PRESETS: {cluster: {params: {}}, vigil: {params: {count: 16, spread: 0.7}}, stubs: {params: {count: 4, tallest: 0.12}}, wall_sconce: {params: {mount: 'wall', count: 3}}},
  draw(g, P, seed) {
    const sp = Math.round(P.spread * 10), top = Math.max(1, Math.round(P.tallest * 10));
    const z0 = P.mount === 'wall' ? 1 : 0, y0 = P.mount === 'ceil' ? -2 : 0;
    if (P.mount === 'wall') { g.box(-sp - 1, y0 - 1, -1, sp + 2, y0, z0 + sp + 1, '@trim'); }
    if (P.mount === 'ceil') { g.box(0, -1, 0, 1, 1, 1, 'iron'); g.box(-sp - 1, y0 - 1, -sp - 1, sp + 2, y0, sp + 2, 'iron'); }
    g.box(-1, y0, z0 - 1, 2, y0 + 1, z0 + 2, 'tallow');                      // the melted puddle
    for (let i = 0; i < P.count; i++) {
      const x = Math.round((frand(seed, 'c', i, 'x') * 2 - 1) * sp), z = z0 + Math.round((frand(seed, 'c', i, 'z') * 2 - 1) * sp);
      const h = 1 + Math.round(frand(seed, 'c', i, 'h') * (top - 1));
      g.box(x, y0, z, x + 1, y0 + h, z + 1, 'tallow');
      if (P.lit !== 'none') {
        const fh = 1 + Math.round(P.flame * 2);
        g.box(x, y0 + h, z, x + 1, y0 + h + fh, z + 1, '@' + P.lit);
      }
    }
  }
});
