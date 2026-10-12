/* ruins/urn — a fired-clay urn, turned profile. */
import { defineProp } from '../propkit.js';
export default defineProp({
  name: 'urn', kit: 'ruins', label: 'Urn', version: 1,
  SCHEMA: [
    {k: 'height', n: 'height (m)', min: 0.3, max: 1.6, step: 0.05, def: 0.7, d: ''},
    {k: 'belly', n: 'belly radius (m)', min: 0.1, max: 0.6, step: 0.05, def: 0.25, d: ''},
    {k: 'lidded', n: 'lidded', kind: 'bool', def: true, d: ''},
    {k: 'broken', n: 'broken', kind: 'bool', def: false, d: 'Top smashed off, sherds round it.'},
    {k: 'clay', n: 'clay', kind: 'enum', options: ['roof_tile', '@trim', 'gold'], def: 'roof_tile', d: 'Fired clay, palette stone, or gold.'}
  ],
  PRESETS: {urn: {params: {}}, amphora: {params: {height: 1.2, belly: 0.3, lidded: false}}, golden: {params: {clay: 'gold', height: 0.5}}, smashed: {params: {broken: true, lidded: false}}},
  draw(g, P) {
    const H = Math.round(P.height * 10), b = P.belly * 10;
    const top = P.broken ? Math.round(H * 0.55) : H;
    for (let y = 0; y < top; y++) {
      const t = (y + 0.5) / H;
      const r = Math.max(1, b * (0.55 + 0.6 * Math.sin(Math.PI * Math.min(1, t * 1.25))) * (t > 0.85 ? 0.6 : 1));
      g.cylY(0, 0, y, y + 1, r, P.clay);
    }
    if (P.lidded && !P.broken) g.cylY(0, 0, H, H + 1, Math.max(1, b * 0.45), P.clay);
    if (P.broken) for (const [x, z] of [[Math.round(b) + 2, 1], [-Math.round(b) - 1, 3], [2, -Math.round(b) - 2]]) g.set(x, 0, z, P.clay);
  }
});
