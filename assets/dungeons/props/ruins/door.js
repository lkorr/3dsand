/* ruins/door — a door LEAF for a doorway: planks with iron straps and studs.
 * The voxels a `door` ref names (refs_doors.h: closed, a door is nothing but
 * its leaf). FRAME = the door ref's: the leaf runs from the origin cell along
 * +X for `width` cells (the HINGE is the x = 0 edge: ref hinge "left", ref
 * yaw = this prop's yaw), up `height` cells, and its FRONT layer is z = 0 with
 * the thickness going BEHIND (-Z): place the ref at the cell the prop's origin
 * lands on and the leaf box is exactly the ref's box. The arch above a
 * rectangular leaf can be closed with stone (`tympanum`, outside the leaf box,
 * so it does not swing).
 */
import { defineProp, frand } from '../propkit.js';
export default defineProp({
  name: 'door', kit: 'ruins', label: 'Door leaf', version: 1,
  SCHEMA: [
    {k: 'width', n: 'width (cells)', min: 4, max: 60, step: 1, int: true, def: 12, d: 'Along the leaf, hinge to latch.'},
    {k: 'height', n: 'height (cells)', min: 8, max: 80, step: 1, int: true, def: 22, d: ''},
    {k: 'thickness', n: 'thickness (cells)', min: 1, max: 4, step: 1, int: true, def: 2, d: 'Behind the front face.'},
    {k: 'style', n: 'style', kind: 'enum', options: ['iron_bound', 'planked', 'iron'], def: 'iron_bound', d: 'iron = a solid iron leaf with ribs.'},
    {k: 'straps', n: 'straps', min: 0, max: 6, step: 1, int: true, def: 3, d: 'Iron straps across the front.'},
    {k: 'tympanum', n: 'tympanum (cells)', min: 0, max: 20, step: 1, int: true, def: 0, d: 'Stone (palette trim) over the leaf to close an arched head.'}
  ],
  PRESETS: {iron_bound: {params: {}}, great_door: {params: {width: 30, height: 30, thickness: 3, straps: 4}}, plain: {params: {style: 'planked', straps: 0}}, iron_door: {params: {style: 'iron', straps: 2}}},
  draw(g, P, seed) {
    const W = P.width, H = P.height, T = P.thickness;
    const wood = P.style === 'iron' ? 'iron' : 'door_wood';
    g.box(0, 0, 1 - T, W, H, 1, wood);
    // planks: a darker seam every few cells (charred_wood reads as a groove)
    if (P.style !== 'iron') for (let x = 3; x < W - 1; x += 3 + Math.floor(frand(seed, 'plank', x) * 2)) for (let y = 0; y < H; y++) g.set(x, y, 0, 'charred_wood');
    for (let i = 0; i < P.straps; i++) {
      const y = Math.round(2 + (H - 5) * (P.straps === 1 ? 0.5 : i / (P.straps - 1)));
      g.box(0, y, 0, W - 1, y + 2, 1, 'iron');
      for (let x = 2; x < W - 2; x += 4) g.set(x, y + 1, 1, 'iron');       // studs proud of the face
    }
    // the ring pull near the latch edge
    g.set(W - 3, Math.round(H * 0.45), 1, 'iron');
    g.set(W - 3, Math.round(H * 0.45) - 1, 1, 'iron');
    if (P.tympanum > 0) g.box(-1, H, 1 - T, W + 1, H + P.tympanum, 1, '@trim');
  }
});
