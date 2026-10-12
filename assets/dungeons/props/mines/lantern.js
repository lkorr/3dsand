/* mines/lantern — iron-framed lantern; hangs, stands, or sits on a wall bracket. */
import { defineProp, LIT_ROW, MOUNT_ROW } from '../propkit.js';
export default defineProp({
  name: 'lantern', kit: 'mines', label: 'Lantern', version: 1,
  SCHEMA: [LIT_ROW, Object.assign({}, MOUNT_ROW, {def: 'ceil'}),
    {k: 'chain', n: 'chain (m)', min: 0, max: 1.5, step: 0.05, def: 0.2, d: 'Hanging chain length (ceil mount).'},
    {k: 'glow', n: 'glow', min: 0, max: 1, step: 0.05, def: 0, d: 'Size of the flame: 0 = a 1 x 2 voxel wick; 1 = a 4 x 5 x 4 flame in a bigger cage. Light carries as far as the glowing surface is big (the irradiance grid works in 0.4 m blocks).'}],
  PRESETS: {hanging: {params: {}}, wall: {params: {mount: 'wall'}}, standing: {params: {mount: 'floor'}}, cold: {params: {lit: 'lightCool'}}, great_lamp: {params: {glow: 1, chain: 0.4}}},
  draw(g, P) {
    const flame = P.lit === 'none' ? 'glass' : '@' + P.lit;
    // c = the flame's width, fh its height; glow 0 is the original 1 x 2 wick.
    const c = 1 + Math.round(P.glow * 3), fh = 2 + Math.round(P.glow * 3), H = fh + 2;
    const o = -Math.floor((c - 1) / 2);
    const body = (y0, z0) => {
      g.frame(o - 1, y0, z0 + o - 1, o + c + 1, y0 + H, z0 + o + c + 1, 'iron');
      g.box(o, y0 + 1, z0 + o, o + c, y0 + 1 + fh, z0 + o + c, flame);
      g.box(0, y0 + H, z0, 1, y0 + H + 1, z0 + 1, 'iron');
    };
    if (P.mount === 'ceil') {
      const ch = Math.round(P.chain * 10);
      g.box(0, -ch, 0, 1, 1, 1, 'iron');
      body(-ch - H - 1, 0);
    } else if (P.mount === 'wall') {
      const zb = 2 - o;                                   // the cage clears the wall at z = -1
      g.box(0, H, -1, 1, H + 1, zb, 'iron');
      g.box(0, 0, -1, 1, H, 0, 'iron');
      body(-1, zb);
    } else body(0, 0);
  }
});
