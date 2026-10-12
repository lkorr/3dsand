/* mines/crate — a plank box with timber edges; stackable. */
import { defineProp } from '../propkit.js';
export default defineProp({
  name: 'crate', kit: 'mines', label: 'Crate', version: 1,
  SCHEMA: [
    {k: 'size', n: 'size (m)', min: 0.4, max: 1.4, step: 0.1, def: 0.8, d: ''},
    {k: 'open', n: 'open', kind: 'bool', def: false, d: 'Lid off (hollow).'},
    {k: 'stack', n: 'stack', min: 1, max: 3, step: 1, int: true, def: 1, d: 'Crates stacked (each a little smaller).'}
  ],
  PRESETS: {crate: {params: {}}, open: {params: {open: true}}, stack: {params: {stack: 3, size: 0.9}}},
  draw(g, P) {
    let y = 0;
    for (let k = 0; k < P.stack; k++) {
      const s = Math.max(3, Math.round(P.size * 10) - k * 2), h = Math.floor(s / 2);
      g.box(-h, y, -h, s - h, y + s, s - h, 'plank');
      g.frame(-h, y, -h, s - h, y + s, s - h, 'timber');
      if (P.open && k === P.stack - 1) g.box(-h + 1, y + 1, -h + 1, s - h - 1, y + s, s - h - 1, '#carve');
      y += s;
    }
  }
});
