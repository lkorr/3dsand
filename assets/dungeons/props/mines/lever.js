/* mines/lever — a winding-house lever: a timber stand with an iron quadrant
 * and a long iron handle, the thing a `lever` ref (which is not drawn) is
 * dressed with. Front faces +Z; the handle leans toward the front when
 * `pulled`, toward the back when not. Every voxel touches the stand, and the
 * stand stands on the floor: nothing here is an island (an island falls as
 * debris and wakes its chunk). */
import { defineProp } from '../propkit.js';
export default defineProp({
  name: 'lever', kit: 'mines', label: 'Lever', version: 1,
  SCHEMA: [
    {k: 'height', n: 'handle (m)', min: 0.6, max: 1.8, step: 0.1, def: 1.2, d: 'Length of the handle above the stand.'},
    {k: 'pulled', n: 'pulled', kind: 'bool', def: false, d: 'Drawn thrown forward (match the ref\'s startOn).'},
    {k: 'post', n: 'signal post', kind: 'bool', def: true, d: 'A tall timber post behind the stand with an iron bell bracket: the lever is easy to spot from across a room.'}
  ],
  PRESETS: {lever: {params: {}}, thrown: {params: {pulled: true}}, plain: {params: {post: false}}},
  draw(g, P) {
    // The stand: two timber cheeks on a plank sole.
    g.box(-3, 0, -2, 4, 1, 3, 'plank');
    for (const x of [-2, 2]) g.box(x, 1, -1, x + 1, 6, 2, 'timber');
    // Iron quadrant (the notched arc the handle rides in), on the cheeks.
    for (let a = 0; a <= 6; a++) {
      const t = (a / 6) * Math.PI;
      const y = 4 + Math.round(2 * Math.sin(t)), z = Math.round(2 * Math.cos(t));
      g.set(-1, y, z, 'iron'); g.set(1, y, z, 'iron');
    }
    // The pivot and the handle.
    g.box(-2, 4, 0, 3, 5, 1, 'iron');
    const L = Math.round(P.height * 10), lean = P.pulled ? 1 : -1;
    const tip = [0, 4 + Math.round(L * 0.9), Math.round(lean * L * 0.42)];
    g.line([0, 5, 0], tip, 'iron');
    g.box(-1, tip[1], tip[2], 2, tip[1] + 1, tip[2] + 1, 'plank');
    if (P.post) {
      g.box(-1, 1, -3, 1, 18, -1, 'timber');
      g.box(-1, 16, -5, 1, 17, -3, 'iron');
      g.box(-1, 13, -6, 1, 16, -4, 'iron');
    }
  }
});
