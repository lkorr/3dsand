/* mines/ladder — a timber ladder standing at the origin, rising +Y, against a wall at z = -1. */
import { defineProp } from '../propkit.js';
export default defineProp({
  name: 'ladder', kit: 'mines', label: 'Ladder', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 1, max: 25, step: 0.1, def: 4, d: 'One template is at most 256 cells: longer shafts stack several (Builder.ladder).'},
    {k: 'width', n: 'width (m)', min: 0.3, max: 1, step: 0.05, def: 0.5, d: ''},
    {k: 'rungGap', n: 'rung gap (vox)', min: 2, max: 5, step: 1, int: true, def: 3, d: ''}
  ],
  PRESETS: {ladder: {params: {}}, long: {params: {length: 20}}},
  draw(g, P) {
    const L = Math.round(P.length * 10), hw = Math.round(P.width * 5);
    g.box(-hw, 0, 0, -hw + 1, L, 1, 'timber');
    g.box(hw, 0, 0, hw + 1, L, 1, 'timber');
    for (let y = 2; y < L; y += P.rungGap) g.box(-hw + 1, y, 0, hw, y + 1, 1, 'plank');
  }
});
