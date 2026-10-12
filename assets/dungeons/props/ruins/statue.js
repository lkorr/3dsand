/* ruins/statue — a robed figure on a plinth, blocked out in palette statue stone. Faces +Z. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'statue', kit: 'ruins', label: 'Statue', version: 1,
  SCHEMA: [
    {k: 'height', n: 'height (m)', min: 1, max: 8, step: 0.1, def: 2.2, d: 'Figure height, plinth excluded.'},
    {k: 'pose', n: 'pose', kind: 'enum', options: ['standing', 'arms_raised', 'kneeling', 'headless'], def: 'standing', d: ''},
    {k: 'plinth', n: 'plinth (m)', min: 0, max: 1.5, step: 0.05, def: 0.4, d: ''},
    {k: 'weathering', n: 'weathering', min: 0, max: 1, step: 0.05, def: 0.3, d: 'Chips knocked off the surface.'}
  ],
  PRESETS: {guardian: {params: {}}, colossus: {params: {height: 6, plinth: 1}}, supplicant: {params: {pose: 'kneeling'}}, ruined: {params: {pose: 'headless', weathering: 0.8}}},
  draw(g, P, seed) {
    const H = Math.round(P.height * 10), pl = Math.round(P.plinth * 10);
    const s = H / 22;                          // proportions authored for a 2.2 m figure
    const S = (v) => Math.max(1, Math.round(v * s));
    if (pl > 0) g.box(-S(5), 0, -S(4), S(5) + 1, pl, S(4) + 1, '@trim');
    const y0 = pl;
    const kneel = P.pose === 'kneeling';
    const legH = kneel ? S(5) : S(10), torso = S(7), head = S(3);
    // robe/legs, torso, shoulders, head
    g.box(-S(3), y0, -S(2), S(3) + 1, y0 + legH, S(2) + 1, '@statue');
    if (kneel) g.box(-S(3), y0, S(2), S(3) + 1, y0 + S(2), S(5), '@statue');
    const ty = y0 + legH;
    g.box(-S(3), ty, -S(2), S(3) + 1, ty + torso, S(2), '@statue');
    g.box(-S(4), ty + torso - S(2), -S(2), S(4) + 1, ty + torso, S(2), '@statue');
    if (P.pose === 'arms_raised') for (const x of [-S(4), S(4)]) g.box(x, ty + torso, -1, x + 1, ty + torso + S(5), 1, '@statue');
    else for (const x of [-S(4), S(4)]) g.box(x, ty + S(1), -1, x + 1, ty + torso, 1, '@statue');
    if (P.pose !== 'headless') g.box(-S(1), ty + torso, -S(1), S(1) + 1, ty + torso + head, S(1) + 1, '@statue');
    else g.box(-S(1), ty + torso, -S(1), S(1) + 1, ty + torso + 1, S(1) + 1, '@statue');
    // Weathering: carve seeded chips off the front and sides.
    const n = Math.round(P.weathering * H);
    for (let i = 0; i < n; i++) {
      const x = Math.round((frand(seed, 'chip', i, 'x') * 2 - 1) * S(4)), y = y0 + Math.round(frand(seed, 'chip', i, 'y') * (legH + torso));
      const z = frand(seed, 'chip', i, 'z') < 0.5 ? S(2) : -S(2);
      if (g.get(x, y, z) === '@statue') g.set(x, y, z, '#carve');
    }
  }
});
