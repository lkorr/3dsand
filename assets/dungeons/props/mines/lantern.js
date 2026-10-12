/* mines/lantern — iron-framed lantern; hangs, stands, or sits on a wall bracket. */
import { defineProp, LIT_ROW, MOUNT_ROW } from '../propkit.js';
export default defineProp({
  name: 'lantern', kit: 'mines', label: 'Lantern', version: 1,
  SCHEMA: [LIT_ROW, Object.assign({}, MOUNT_ROW, {def: 'ceil'}),
    {k: 'chain', n: 'chain (m)', min: 0, max: 1.5, step: 0.05, def: 0.2, d: 'Hanging chain length (ceil mount).'}],
  PRESETS: {hanging: {params: {}}, wall: {params: {mount: 'wall'}}, standing: {params: {mount: 'floor'}}, cold: {params: {lit: 'lightCool'}}},
  draw(g, P) {
    const flame = P.lit === 'none' ? 'glass' : '@' + P.lit;
    const body = (y0, z0) => {
      g.frame(-1, y0, z0 - 1, 2, y0 + 4, z0 + 2, 'iron');
      g.box(0, y0 + 1, z0, 1, y0 + 3, z0 + 1, flame);
      g.box(0, y0 + 4, z0, 1, y0 + 5, z0 + 1, 'iron');
    };
    if (P.mount === 'ceil') {
      const c = Math.round(P.chain * 10);
      g.box(0, -c, 0, 1, 1, 1, 'iron');
      body(-c - 5, 0);
    } else if (P.mount === 'wall') {
      g.box(0, 4, -1, 1, 5, 2, 'iron');
      g.box(0, 0, -1, 1, 4, 0, 'iron');
      body(-1, 2);
    } else body(0, 0);
  }
});
