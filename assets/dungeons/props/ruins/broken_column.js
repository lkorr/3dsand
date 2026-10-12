/* ruins/broken_column — a column stump on its base, its fallen drums beside it. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'broken_column', kit: 'ruins', label: 'Broken column', version: 1,
  SCHEMA: [
    {k: 'radius', n: 'radius (m)', min: 0.2, max: 1.2, step: 0.05, def: 0.4, d: ''},
    {k: 'stump', n: 'stump height (m)', min: 0.2, max: 4, step: 0.1, def: 1.2, d: ''},
    {k: 'drums', n: 'fallen drums', min: 0, max: 4, step: 1, int: true, def: 2, d: ''},
    {k: 'fluted', n: 'fluted', kind: 'bool', def: true, d: ''}
  ],
  PRESETS: {stump: {params: {}}, tall_snap: {params: {stump: 3, drums: 1}}, rubble_only: {params: {stump: 0.3, drums: 4}}},
  draw(g, P, seed) {
    const r = P.radius * 10, R = Math.ceil(r), h = Math.round(P.stump * 10);
    g.box(-R - 2, 0, -R - 2, R + 3, 2, R + 3, '@trim');
    g.cylY(0, 0, 2, h, r, '@support');
    for (let y = h; y < h + 3; y++) for (let x = -R; x <= R; x++) for (let z = -R; z <= R; z++)
      if (x * x + z * z <= r * r && frand(seed, 'snap', x, z) < 0.5 - (y - h) * 0.2) g.set(x, y, z, '@support');
    if (P.fluted && R >= 3) for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) g.box(dx * R, 3, dz * R, dx * R + 1, h - 1, dz * R + 1, '#carve');
    for (let i = 0; i < P.drums; i++) {
      const len = Math.round(r * (1.5 + frand(seed, 'drum', i, 'l'))), ax = frand(seed, 'drum', i, 'ax') < 0.5 ? 'x' : 'z';
      const off = Math.round(R * 2 + 3 + i * (R * 2 + 2)), side = frand(seed, 'drum', i, 's') < 0.5 ? -1 : 1;
      if (ax === 'x') g.cylH('x', off * side - len, off * side + len, R, Math.round((frand(seed, 'drum', i, 'o') * 2 - 1) * R), r, '@support');
      else g.cylH('z', off * side - len, off * side + len, R, Math.round((frand(seed, 'drum', i, 'o') * 2 - 1) * R), r, '@support');
    }
  }
});
