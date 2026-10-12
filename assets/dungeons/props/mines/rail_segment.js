/* mines/rail_segment — a length of track: iron rails on timber sleepers, along +Z. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'rail_segment', kit: 'mines', label: 'Rail segment', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 0.5, max: 12, step: 0.1, def: 3, d: 'Along +Z from the origin.'},
    {k: 'gauge', n: 'gauge (m)', min: 0.4, max: 1.2, step: 0.05, def: 0.6, d: ''},
    {k: 'sleeperSpacing', n: 'sleeper spacing (m)', min: 0.3, max: 1.5, step: 0.05, def: 0.6, d: ''},
    {k: 'broken', n: 'broken', min: 0, max: 1, step: 0.05, def: 0, d: 'Missing sleepers and a sprung rail end.'}
  ],
  PRESETS: {straight: {params: {}}, long: {params: {length: 10}}, wrecked: {params: {broken: 0.7}}},
  draw(g, P, seed) {
    const L = Math.round(P.length * 10), hg = Math.round(P.gauge * 5), sp = Math.max(3, Math.round(P.sleeperSpacing * 10));
    for (let z = 1, i = 0; z + 2 <= L; z += sp, i++) if (frand(seed, 'sl', i) >= P.broken * 0.6) g.box(-hg - 2, 0, z, hg + 3, 1, z + 2, 'timber');
    const sprung = P.broken > 0.3 && frand(seed, 'sprung') < P.broken;
    g.box(-hg - 1, 1, 0, -hg, 2, L, 'iron');
    g.box(hg, 1, 0, hg + 1, 2, sprung ? L - 4 : L, 'iron');
    if (sprung) g.line([hg, 2, L - 4], [hg + 2, 4, L], 'iron');
  }
});
