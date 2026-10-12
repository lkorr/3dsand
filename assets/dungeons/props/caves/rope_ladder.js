/* caves/rope_ladder — rope rails with plank rungs, from its foot (origin) up +Y; the wall it hangs against is at z = -1. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'rope_ladder', kit: 'caves', label: 'Rope ladder', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 1, max: 25, step: 0.1, def: 6, d: 'One template is at most 256 cells: longer drops stack several (Builder.ladder).'},
    {k: 'width', n: 'width (m)', min: 0.3, max: 1, step: 0.05, def: 0.5, d: ''},
    {k: 'rungGap', n: 'rung gap (vox)', min: 2, max: 6, step: 1, int: true, def: 3, d: ''},
    {k: 'rotten', n: 'rotten', min: 0, max: 1, step: 0.05, def: 0, d: 'Missing rungs (a gap over 6 voxels is hard to climb).'},
    {k: 'anchor', n: 'top anchor', kind: 'bool', def: true, d: 'An iron peg and a coil at the top.'}
  ],
  PRESETS: {ladder: {params: {}}, long_drop: {params: {length: 24}}, rotten: {params: {rotten: 0.5}}},
  draw(g, P, seed) {
    const L = Math.round(P.length * 10), hw = Math.round(P.width * 5);
    g.box(-hw, 0, 0, -hw + 1, L, 1, 'cloth');
    g.box(hw, 0, 0, hw + 1, L, 1, 'cloth');
    for (let y = 2, i = 0; y < L; y += P.rungGap, i++) if (frand(seed, 'rung', i) >= P.rotten * 0.7) g.box(-hw + 1, y, 0, hw, y + 1, 1, 'plank');
    if (P.anchor) { g.box(-hw, L, -1, hw + 1, L + 1, 1, 'iron'); g.ball(hw + 3, L, 2, 2, 1, 2, 'cloth'); }
  }
});
