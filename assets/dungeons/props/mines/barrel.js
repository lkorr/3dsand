/* mines/barrel — staved barrel with iron hoops; an open one shows its liquid. */
import { defineProp } from '../propkit.js';
export default defineProp({
  name: 'barrel', kit: 'mines', label: 'Barrel', version: 1,
  SCHEMA: [
    {k: 'height', n: 'height (m)', min: 0.5, max: 1.4, step: 0.05, def: 0.9, d: ''},
    {k: 'radius', n: 'radius (m)', min: 0.2, max: 0.6, step: 0.05, def: 0.3, d: ''},
    {k: 'contents', n: 'contents', kind: 'enum', options: ['sealed', 'water', 'oil', 'empty'], def: 'sealed', d: 'An open barrel shows its liquid (oil burns).'}
  ],
  PRESETS: {barrel: {params: {}}, water_butt: {params: {contents: 'water', height: 1.1, radius: 0.4}}, oil: {params: {contents: 'oil'}}},
  draw(g, P) {
    const H = Math.round(P.height * 10), r = P.radius * 10;
    for (let y = 0; y < H; y++) {
      const bulge = r + 0.8 * Math.sin((Math.PI * (y + 0.5)) / H);
      g.cylY(0, 0, y, y + 1, bulge, (y === 1 || y === H - 2 || y === Math.floor(H / 2)) ? 'iron' : 'plank');
    }
    if (P.contents !== 'sealed') {
      g.cylY(0, 0, 1, H, r - 0.6, '#carve');
      if (P.contents !== 'empty') g.cylY(0, 0, 1, H - 2, r - 0.6, P.contents);
    }
  }
});
