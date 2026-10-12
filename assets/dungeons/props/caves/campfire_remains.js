/* caves/campfire_remains — a cold hearth: a ring of stones, ash, half-burnt logs, maybe a pot. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'campfire_remains', kit: 'caves', label: 'Campfire remains', version: 1,
  SCHEMA: [
    {k: 'radius', n: 'ring radius (m)', min: 0.3, max: 1.2, step: 0.05, def: 0.5, d: ''},
    {k: 'logs', n: 'charred logs', min: 0, max: 6, step: 1, int: true, def: 3, d: ''},
    {k: 'pot', n: 'cook pot', kind: 'bool', def: false, d: 'An iron pot on a tripod.'},
    {k: 'bedrolls', n: 'bedrolls', min: 0, max: 4, step: 1, int: true, def: 1, d: 'Cloth rolls round the hearth.'}
  ],
  PRESETS: {cold_hearth: {params: {}}, camp: {params: {pot: true, bedrolls: 3, logs: 4}}, ashes: {params: {logs: 0, bedrolls: 0}}},
  draw(g, P, seed) {
    const R = P.radius * 10, Ri = Math.ceil(R) + 1;
    for (let z = -Ri; z <= Ri; z++) for (let x = -Ri; x <= Ri; x++) {
      const d = Math.hypot(x, z);
      if (Math.abs(d - R) < 0.8 && frand(seed, 'ring', x, z) < 0.8) g.box(x, 0, z, x + 1, 1 + (frand(seed, 'rh', x, z) < 0.3 ? 1 : 0), z + 1, 'cobble');
      else if (d < R - 0.8) g.set(x, 0, z, 'ash');
    }
    for (let i = 0; i < P.logs; i++) {
      const a = (i / Math.max(1, P.logs)) * Math.PI + frand(seed, 'log', i) * 0.8, L = Math.round(R * 1.2);
      g.line([Math.round(Math.cos(a) * -L / 2), 1, Math.round(Math.sin(a) * -L / 2)], [Math.round(Math.cos(a) * L / 2), 1, Math.round(Math.sin(a) * L / 2)], 'charred_wood');
    }
    if (P.pot) {
      for (const [x, z] of [[-Ri, -Ri], [Ri, -Ri], [0, Ri]]) g.line([x, 0, z], [0, 9, 0], 'iron');
      g.ball(0, 6, 0, 2, 2, 2, 'iron'); g.box(-1, 7, -1, 2, 9, 2, '#carve');
    }
    for (let k = 0; k < P.bedrolls; k++) {
      const a = frand(seed, 'bed', k) * Math.PI * 2, d = Ri + 6;
      const x = Math.round(Math.cos(a) * d), z = Math.round(Math.sin(a) * d);
      g.cylH(Math.abs(Math.cos(a)) > 0.7 ? 'z' : 'x', (Math.abs(Math.cos(a)) > 0.7 ? z : x) - 8, (Math.abs(Math.cos(a)) > 0.7 ? z : x) + 8, 1, Math.abs(Math.cos(a)) > 0.7 ? x : z, 1.3, 'cloth');
    }
  }
});
