/* caves/crystal_cluster — prisms leaning out of a common root (crystal is emissive). */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'crystal_cluster', kit: 'caves', label: 'Crystal cluster', version: 1,
  SCHEMA: [
    {k: 'count', n: 'prisms', min: 1, max: 16, step: 1, int: true, def: 6, d: ''},
    {k: 'size', n: 'size', min: 0.3, max: 3, step: 0.05, def: 1, d: 'Scale of the longest prism (1 = 1.2 m).'},
    {k: 'lean', n: 'lean', min: 0, max: 1, step: 0.05, def: 0.5, d: 'How far prisms lean off vertical.'},
    {k: 'mat', n: 'material', kind: 'enum', options: ['crystal', 'crystal_spire', 'gem_arcane', 'rime_crystal', '@accent'], def: 'crystal', d: ''},
    {k: 'lit', n: 'flame', kind: 'enum', options: ['light', 'lightCool', 'none'], def: 'lightCool', d: 'Ignored (the crystal glows by its material); kept for light placement.'},
    {k: 'mount', n: 'mount', kind: 'enum', options: ['floor', 'ceil', 'wall'], def: 'floor', d: 'ceil = hangs down.'}
  ],
  PRESETS: {cluster: {params: {}}, geode_spray: {params: {count: 14, size: 0.6, lean: 0.9}}, monolith: {params: {count: 2, size: 2.6, lean: 0.15, mat: 'crystal_spire'}}, arcane: {params: {mat: 'gem_arcane', size: 0.5}}},
  draw(g, P, seed) {
    const sgn = P.mount === 'ceil' ? -1 : 1;
    g.ball(0, 0, 0, 2, 1, 2, 'quartzite');
    for (let i = 0; i < P.count; i++) {
      const L = Math.max(3, Math.round(12 * P.size * (0.4 + 0.6 * frand(seed, 'p', i, 'l'))));
      const a = frand(seed, 'p', i, 'a') * Math.PI * 2, lean = P.lean * frand(seed, 'p', i, 'lean');
      const dx = Math.cos(a) * lean, dz = Math.sin(a) * lean;
      const r = Math.max(0.6, L / 7);
      for (let s = 0; s < L; s++) {
        const rr = s > L - 3 ? Math.max(0, r * (L - s) / 3) : r;
        g.cylY(Math.round(dx * s), Math.round(dz * s), sgn * s, sgn * s + 1, rr, P.mat);
      }
    }
  }
});
