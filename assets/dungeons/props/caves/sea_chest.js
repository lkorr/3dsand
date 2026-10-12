/* caves/sea_chest — a smuggler's sea chest: a plank box with a domed lid,
 * iron bands, a brass lock plate and rope beckets; stood on a FOOTING of the
 * floor material so a chest set on an uneven cave floor neither floats over
 * a dip nor hides in a bump (the floor's noise is several voxels; the footing
 * fills down and the body clears up). The chest's CELL for a container ref is
 * the prop origin (0, 0, 0), the middle of its front-bottom edge. */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'sea_chest', kit: 'caves', label: 'Sea chest', version: 1,
  SCHEMA: [
    {k: 'length', n: 'length (m)', min: 0.6, max: 1.6, step: 0.1, def: 1, d: 'Along its front.'},
    {k: 'depth', n: 'depth (m)', min: 0.4, max: 1, step: 0.1, def: 0.6, d: ''},
    {k: 'height', n: 'height (m)', min: 0.4, max: 1, step: 0.1, def: 0.6, d: 'To the top of the lid.'},
    {k: 'bands', n: 'iron bands', min: 0, max: 4, step: 1, int: true, def: 2, d: ''},
    {k: 'domed', n: 'domed lid', kind: 'bool', def: true, d: ''},
    {k: 'rot', n: 'rot', min: 0, max: 1, step: 0.05, def: 0.2, d: 'Missing planks (the sea air).'},
    {k: 'footing', n: 'footing (vox)', min: 0, max: 8, step: 1, int: true, def: 4, d: 'Floor material laid under the chest, down this many voxels: it meets a floor that dips.'},
    {k: 'clear', n: 'clear above (vox)', min: 0, max: 6, step: 1, int: true, def: 2, d: 'Air carved round the chest\'s body and this far over it: a floor bump does not bury it.'}
  ],
  PRESETS: {chest: {params: {}}, strongbox: {params: {length: 0.8, depth: 0.5, height: 0.5, bands: 3, domed: false, rot: 0}},
            rotten: {params: {rot: 0.6, bands: 1, length: 1.2}}},
  draw(g, P, seed) {
    const L = Math.max(5, Math.round(P.length * 10)), D = Math.max(4, Math.round(P.depth * 10)), H = Math.max(4, Math.round(P.height * 10));
    const x0 = -Math.floor(L / 2), x1 = x0 + L, z0 = 0, z1 = D;
    const lidH = P.domed ? Math.max(2, Math.round(H * 0.35)) : 1, bodyH = H - lidH;
    // The air round it first (later sets overwrite these cells).
    if (P.clear > 0) g.box(x0 - 1, 0, z0 - 1, x1 + 1, H + P.clear, z1 + 1, '#carve');
    if (P.footing > 0) g.box(x0 - 1, -P.footing, z0 - 1, x1 + 1, 0, z1 + 1, '@floor');
    // Body: plank shell with a timber rim.
    g.box(x0, 0, z0, x1, bodyH, z1, 'plank');
    g.frame(x0, 0, z0, x1, bodyH, z1, 'timber');
    // Lid: flat, or a barrel dome along X.
    if (P.domed) {
      const r = (D - 1) / 2, cz = (D - 1) / 2;
      for (let y = 0; y < lidH; y++) for (let z = z0; z < z1; z++) {
        const dz = (z - cz) / Math.max(1, r), dy = (y + 0.5) / lidH;
        if (dz * dz + dy * dy <= 1.05) for (let x = x0; x < x1; x++) g.set(x, bodyH + y, z, 'plank');
      }
    } else g.box(x0, bodyH, z0, x1, bodyH + lidH, z1, 'plank');
    // Rot: missing planks (never the bottom row or the rim).
    for (let x = x0 + 1; x < x1 - 1; x++) for (let y = 1; y < bodyH - 1; y++)
      if (frand(seed, 'rot', x, y) < P.rot * 0.35) g.set(x, y, z0, '#carve');
    // Iron bands round body and lid.
    for (let k = 0; k < P.bands; k++) {
      const x = x0 + Math.round(((k + 1) * L) / (P.bands + 1));
      for (let y = 0; y < H; y++) for (const z of [z0, z1 - 1]) if (g.get(x, y, z)) g.set(x, y, z, 'iron');
      for (let z = z0; z < z1; z++) { let top = -1; for (let y = 0; y < H; y++) if (g.get(x, y, z) && g.get(x, y, z) !== '#carve') top = y; if (top >= 0) g.set(x, top, z, 'iron'); }
    }
    // The lock plate and the rope beckets at the ends.
    g.box(-1, bodyH - 2, z0 - 1, 1, bodyH, z0, 'brass');
    for (const x of [x0 - 1, x1]) g.box(x, bodyH - 2, Math.floor(D / 2) - 1, x + 1, bodyH - 1, Math.floor(D / 2) + 1, 'leather');
  }
});
