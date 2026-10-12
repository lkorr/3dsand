/* ruins/sarcophagus — a stone coffin; long axis Z. Broken = lid slid off, bones inside. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'sarcophagus', kit: 'ruins', label: 'Sarcophagus', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 1.6, max: 3, step: 0.1, def: 2.2, d: ''},
    {k: 'width', n: 'width (m)', min: 0.6, max: 1.4, step: 0.1, def: 0.9, d: ''},
    {k: 'broken', n: 'opened', kind: 'bool', def: false, d: 'Lid pushed half off and cracked; bones inside.'},
    {k: 'effigy', n: 'effigy lid', kind: 'bool', def: true, d: 'A carved figure lying on the lid.'}
  ],
  PRESETS: {sealed: {params: {}}, opened: {params: {broken: true}}, plain: {params: {effigy: false}}, great: {params: {length: 3, width: 1.4}}},
  draw(g, P, seed) {
    const hl = Math.round(P.length * 5), hw = Math.round(P.width * 5), H = 7;
    g.box(-hw - 1, 0, -hl - 1, hw + 1, 1, hl + 1, '@trim');
    g.box(-hw, 1, -hl, hw, H, hl, '@trim');
    g.box(-hw + 1, 2, -hl + 1, hw - 1, H, hl - 1, '#carve');
    if (P.broken) {
      for (let z = -hl + 2; z < hl - 2; z += 2) if (frand(seed, 'bone', z) < 0.6) g.set(Math.round((frand(seed, 'bx', z) * 2 - 1) * (hw - 2)), 2, z, 'bone');
      g.box(-1, 2, hl - 4, 2, 4, hl - 1, 'bone');
      const off = hw + 1;
      g.box(-hw + off, H, -hl, hw + off, H + 2, Math.round(hl * 0.2), '@trim');
      g.box(-hw + off, 0, Math.round(hl * 0.3), hw + off, 2, hl, '@trim');
    } else {
      g.box(-hw - 1, H, -hl - 1, hw + 1, H + 2, hl + 1, '@trim');
      if (P.effigy) {
        g.box(-2, H + 2, -hl + 3, 3, H + 4, hl - 5, '@statue');
        g.box(-1, H + 2, hl - 5, 2, H + 5, hl - 2, '@statue');
      }
    }
  }
});
