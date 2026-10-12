/* caves/glow_mushroom_patch — glowcaps on pale stems, a few big ones among small (glowcap is emissive: a light). */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'glow_mushroom_patch', kit: 'caves', label: 'Glow mushroom patch', version: 1,
  SCHEMA: [
    {k: 'count', n: 'mushrooms', min: 1, max: 30, step: 1, int: true, def: 9, d: ''},
    {k: 'spread', n: 'spread (m)', min: 0.2, max: 3, step: 0.1, def: 0.8, d: ''},
    {k: 'giant', n: 'giants', min: 0, max: 3, step: 1, int: true, def: 1, d: 'Big caps (glowcap_giant) among the small.'},
    {k: 'lit', n: 'flame', kind: 'enum', options: ['light', 'lightCool', 'none'], def: 'light', d: 'Ignored: the caps glow by their own material. Kept so the core can place it as a light.'},
    {k: 'mount', n: 'mount', kind: 'enum', options: ['floor', 'ceil', 'wall'], def: 'floor', d: 'Always grows up from the floor.'}
  ],
  PRESETS: {patch: {params: {}}, ring: {params: {count: 20, spread: 1.5, giant: 0}}, grove: {params: {count: 6, giant: 3, spread: 2}}},
  draw(g, P, seed) {
    const sp = P.spread * 10;
    g.box(-1, 0, -1, 2, 1, 2, 'wet_moss');
    for (let i = 0; i < P.count; i++) {
      const a = frand(seed, 'm', i, 'a') * Math.PI * 2, d = Math.sqrt(frand(seed, 'm', i, 'd')) * sp;
      const x = Math.round(Math.cos(a) * d), z = Math.round(Math.sin(a) * d);
      const giant = i < P.giant;
      const h = giant ? 6 + Math.round(frand(seed, 'm', i, 'h') * 8) : 1 + Math.round(frand(seed, 'm', i, 'h') * 3);
      g.box(x, 0, z, x + 1, h, z + 1, 'mushroom_stem');
      const cr = giant ? 2 + Math.round(h / 5) : (h > 2 ? 1 : 0);
      g.cylY(x, z, h, h + 1, cr, giant ? 'glowcap_giant' : 'glowcap');
      if (giant) g.cylY(x, z, h - 1, h, Math.max(1, cr - 1), 'glowcap_gill');
    }
  }
});
