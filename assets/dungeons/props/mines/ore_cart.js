/* mines/ore_cart — a plank tub on iron wheels, loaded with ore (or a hoist cage). */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'ore_cart', kit: 'mines', label: 'Ore cart', version: 1,
  SCHEMA: [
    {k: 'style', n: 'style', kind: 'enum', options: ['cart', 'cage'], def: 'cart', d: 'cart = a wheeled tub; cage = an iron hoist cage.'},
    {k: 'load', n: 'load', kind: 'enum', options: ['ore', 'rubble', 'iron', 'gold', 'crystal', 'brimstone', 'none'], def: 'ore', d: 'What is heaped in it (ore / rubble = palette slots).'},
    {k: 'tipped', n: 'tipped over', kind: 'bool', def: false, d: 'Lying on its side, load spilled.'}
  ],
  PRESETS: {loaded: {params: {}}, empty: {params: {load: 'none'}}, cage: {params: {style: 'cage'}}, wreck: {params: {tipped: true, load: 'rubble'}}},
  draw(g, P, seed) {
    if (P.style === 'cage') {
      g.frame(-7, 0, -7, 7, 22, 7, 'iron');
      for (let x = -6; x < 7; x += 3) { g.box(x, 0, -7, x + 1, 22, -6, 'iron'); g.box(x, 0, 6, x + 1, 22, 7, 'iron'); }
      g.box(-7, 0, -7, 7, 1, 7, 'plank');
      g.box(-1, 22, -1, 1, 26, 1, 'iron');
      return;
    }
    const load = P.load === 'ore' ? '@ore' : P.load === 'rubble' ? '@rubble' : P.load;
    if (!P.tipped) {
      // tub 9 wide (x), 13 long (z), 6 tall, on wheels 3 high
      g.box(-4, 3, -6, 5, 9, 7, 'plank');
      g.box(-3, 4, -5, 4, 9, 6, '#carve');
      g.frame(-4, 3, -6, 5, 9, 7, 'iron');
      for (const z of [-4, 4]) for (const x of [-5, 5]) g.cylH('x', x, x + 1, 2, z, 2, 'iron');
      if (P.load !== 'none') for (let z = -5; z < 6; z++) for (let x = -3; x < 4; x++) {
        const h = 7 + Math.round(2 * (1 - (Math.abs(x) / 4 + Math.abs(z) / 6) / 1.4) + frand(seed, 'load', x, z));
        g.box(x, 4, z, x + 1, Math.min(10, h), z + 1, load);
      }
      return;
    }
    // On its side, the open top facing +X, the load spilled out across the floor.
    g.box(0, 0, -6, 6, 9, 7, 'plank');
    g.box(1, 1, -5, 6, 8, 6, '#carve');
    g.frame(0, 0, -6, 6, 9, 7, 'iron');
    for (const z of [-4, 4]) g.cylH('z', z, z + 1, 1, -2, 2, 'iron');
    if (P.load !== 'none') for (let z = -5; z < 9; z++) for (let x = 6; x < 13; x++)
      if (frand(seed, 'spill', x, z) < 0.6 - (x - 6) * 0.07) g.set(x, 0, z, load);
  }
});
