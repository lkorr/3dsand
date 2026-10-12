/* make_dungeon_sample.mjs — rebuild assets/dungeons/_d2_sample/dungeon.json
 * through the same API the tuner's stitcher uses (newPiece / attachPiece), then
 * print its validation panel. Recompile the shell afterwards:
 *   node scripts/make_dungeon_sample.mjs && node scripts/compile_dungeon.mjs _d2_sample
 * The sample: a ruin entrance (the anchor) -> corridor -> hall, whose left door
 * leads through a mine drift and incline up to a mine adit, whose right door
 * leads through a cave tunnel and cavern out of a hillside cave mouth, and
 * whose far door opens on a sanctum: every kit, every entrance type, tapers
 * where the kits meet.
 */
import * as G from '../assets/editor/dungeon/dungeongen.js';
import { writeFileSync } from 'fs';
let d = G.newDungeon({name: 'D2 sample (all three kits)', style: 'ruins', seed: 4242});
d.pieces.push(G.newPiece(d, {kit: 'ruins', type: 'ruin_entrance', preset: 'colonnade'}));
d.entrance = {piece: 'p0', socket: 'surface'};
const at = (ref, spec) => { d = G.attachPiece(d, ref, Object.assign({fit: true}, spec)).dungeon; return d.pieces[d.pieces.length - 1].id; };
const p1 = at({piece: 'p0', socket: 'in'}, {kit: 'ruins', type: 'corridor', preset: 'processional'});
const p2 = at({piece: p1, socket: 'out'}, {kit: 'ruins', type: 'hall', preset: 'nave', params: {exits: 'all'}});
const p3 = at({piece: p2, socket: 'left'}, {kit: 'mines', type: 'drift', preset: 'standard'});
const p4 = at({piece: p3, socket: 'out'}, {kit: 'mines', type: 'incline', preset: 'haulage_up'});
at({piece: p4, socket: 'out'}, {kit: 'mines', type: 'adit_entrance', preset: 'working'});
const p6 = at({piece: p2, socket: 'right'}, {kit: 'caves', type: 'tunnel', preset: 'passage'});
const p7 = at({piece: p6, socket: 'out'}, {kit: 'caves', type: 'cavern', preset: 'chamber', params: {exits: 1}});
at({piece: p7, socket: 'out'}, {kit: 'caves', type: 'mouth', preset: 'hillside'});
at({piece: p2, socket: 'out'}, {kit: 'ruins', type: 'sanctum', preset: 'shrine'});
const c = G.placePiece(d, d.pieces.find(p => p.id === p1));
const s = c.sockets.find(q => q.name === 'in');
d.props.push({id: 'x0', prop: 'ruins/statue', params: {height: 2.6, pose: 'arms_raised'}, seed: 7,
              pos: [s.pos[0] - s.dir[0] * 30 + 8, s.pos[1], s.pos[2] - s.dir[2] * 30], yaw: 270, piece: p1});
writeFileSync(new URL('../assets/dungeons/_d2_sample/dungeon.json', import.meta.url), G.dungeonJsonText(d));
for (const v of G.validate(d)) console.log(v.severity, v.code, v.piece, v.msg);
