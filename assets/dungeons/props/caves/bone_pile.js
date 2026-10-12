/* caves/bone_pile — a lair's leavings: gnawed bones heaped in a hollow, a skull or two. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'bone_pile', kit: 'caves', label: 'Bone pile', version: 1,
  SCHEMA: [
    {k: 'radius', n: 'radius (m)', min: 0.3, max: 2.5, step: 0.1, def: 0.9, d: ''},
    {k: 'height', n: 'height (m)', min: 0.1, max: 1.2, step: 0.05, def: 0.4, d: ''},
    {k: 'skulls', n: 'skulls', min: 0, max: 5, step: 1, int: true, def: 2, d: ''},
    {k: 'gore', n: 'stains', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Dirt mixed in.'}
  ],
  PRESETS: {pile: {params: {}}, lair_heap: {params: {radius: 2, height: 0.8, skulls: 5}}, few: {params: {radius: 0.5, height: 0.2, skulls: 0}}},
  draw(g, P, seed) {
    const R = P.radius * 10, H = P.height * 10, Ri = Math.ceil(R);
    for (let z = -Ri; z <= Ri; z++) for (let x = -Ri; x <= Ri; x++) {
      const d = Math.hypot(x, z) / R;
      if (d > 1) continue;
      const h = Math.round(H * (1 - d * d) + frand(seed, 'h', x, z) * 1.2 - 0.6);
      for (let y = 0; y < h; y++) {
        const r = frand(seed, 'c', x, y, z);
        if (r < 0.55) g.set(x, y, z, 'bone');
        else if (r < 0.55 + P.gore * 0.4) g.set(x, y, z, 'dirt');
      }
    }
    for (let k = 0; k < P.skulls; k++) {
      const x = Math.round((frand(seed, 's', k, 'x') * 2 - 1) * R * 0.6), z = Math.round((frand(seed, 's', k, 'z') * 2 - 1) * R * 0.6);
      const y = Math.max(0, Math.round(H * (1 - (Math.hypot(x, z) / R) ** 2)) - 1);
      g.box(x - 1, y, z - 1, x + 2, y + 3, z + 2, 'bone');
      g.set(x - 1, y + 1, z + 1, '#carve'); g.set(x + 1, y + 1, z + 1, '#carve');
    }
  }
});
