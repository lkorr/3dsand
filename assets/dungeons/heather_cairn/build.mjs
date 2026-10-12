/* build.mjs — THE TARN CAIRN (dungeon `heather_cairn`), package D7 of
 * docs/PLAN_dungeons.md and the D8 variety pass: the example RUIN dungeon,
 * authored THROUGH THE TOOLS -- the same dungeongen.js API the tuner's
 * Dungeons tab calls (newPiece / attachPiece / validate / compile) and the
 * same refs.json writer (ui_entities.js writeRefs), so opening
 * assets/dungeons/heather_cairn/ in the tab shows exactly this.
 *
 *   node assets/dungeons/heather_cairn/build.mjs          write + validate (map-free)
 *   node assets/dungeons/heather_cairn/build.mjs --dry    validate only
 *
 * MAP-FREE. Its home placement (the default map: the head of Heatherhigh's
 * Tarn Way cutting, where the road cuts 14.7 m into the hill) is made and
 * ground-checked by scripts/place_dungeon.mjs ("Portability" in
 * docs/PLAN_dungeons.md):
 *   node scripts/place_dungeon.mjs --map default --dungeon heather_cairn --at -56045,-33555 --yaw 180 --yOffset -147 --ground-y 5053 --write
 * (--ground-y: the road's cutting lowers the terrain column the heightmap
 * reports; placement is relative to the BARE ground there.) The sliders tuned
 * to that ground -- the ones a new map adjusts -- are the entrance's
 * `apronHeight` (how high the paving is cleared: low in a sunken way) and its
 * cairn (`cairnLift`, `cairnZ`: the cairn stands on the brow above the stair),
 * plus the narthex oculus's `keyHeight`:
 *   node scripts/place_dungeon.mjs ... --set p0.apronHeight=6 --set p0.cairnLift=0 --as heather_cairn_hill
 *
 * THE STORY. A hollow way climbs the moor and ends, blind, at the head of its
 * cutting. There a broken colonnade stands round a stairwell, a cairn heaped
 * by later hands on the brow above it. The stair goes down ten metres to a
 * narthex under a low dome, a ring of daylight falling through its oculus.
 * East, into the ridge: the long columned hall, its floor heaved, a wall
 * fallen in, its far half caved. South of the hall the crypt galleries (bone
 * niches, smashed and robbed, the walking dead) and the noble tomb where the
 * bone key lies on a sarcophagus. West of the narthex, a robbed side chapel.
 * Past the hall's ruined end a stair goes down to the second level: the
 * drowned cistern of leaning columns, the pit room whose bridge gives way
 * under a pressure plate, and the sanctum behind the bone-key door, watched
 * by its warden.
 *
 * Every number is a slider a person could have set in the tab; the layout is
 * explicit (no grow), seeds are the derived ones unless a reroll was kept.
 */
import { G, MATS, writeSchedules, writeDungeon, report, adoptWaynodes } from '../../../scripts/dungeon_build_lib.mjs';

const ID = 'heather_cairn';
const args = process.argv.slice(2);
const DRY = args.includes('--dry');

let d = G.newDungeon({name: 'The Tarn Cairn', style: 'ruins', seed: 51336});
// The temple palette, moorland-dressed: heather, not ivy, on the paving; a
// dark basalt inlay for the floor bands; old moss on the damp walls.
d.palette = {variant: 'temple', veg: 'heather', tile2: 'basalt', moss: 'old_moss'};

const at = (ref, spec) => { d = G.attachPiece(d, ref, Object.assign({fit: true, checkOverlap: true}, spec)).dungeon; return d.pieces[d.pieces.length - 1].id; };
// Lights that carry: a brazier with a full bowl, candles that make a vigil's glow.
const LIT = {lightPower: 0.7};

// ---- level 0: the surface ruin -------------------------------------------------
d.pieces.push(G.newPiece(d, {kit: 'ruins', type: 'ruin_entrance', params: {
  width: 3.2, height: 3.2, drop: 12, pitch: 0.55, stepHeight: 0.22, foot: 2.5,
  platform: 2.5, apronHeight: 2.2, columns: 3, columnHeight: 5.2, columnStyle: 'fluted', lintels: true, toppled: 2,
  parapet: 0.7, overgrowth: 0.7, decay: 0.6, moisture: 0.4, lightCount: 2, lightType: 'brazier', asym: 0.5, jitter: 0.7, lightPower: 0.6,
  // the cairn stands on the brow of the saddle, above the stair's head: seen from the Tarn Way
  cairn: 3.3, cairnRadius: 2.9, cairnX: -1.5, cairnZ: 18, cairnLift: 11.4, cairnStone: 'mixed'}}));
d.entrance = {piece: 'p0', socket: 'surface'};

// ---- level 1 -------------------------------------------------------------------
const narthex = at({piece: 'p0', socket: 'in'}, {kit: 'ruins', type: 'hall', params: Object.assign({
  width: 9, length: 8, height: 4.2, ceiling: 3.2, vault: 'dome', columnRows: '0', dais: false, banners: 0,
  tilePattern: 'runner', settle: 0.45, jitter: 0.5,
  keyLight: 'shaft', keySize: 0.6, keyHeight: 20, keyAt: 0.55,
  exits: 'sides', decay: 0.35, moisture: 0.3, lightCount: 2, lightType: 'candles', doorWidth: 3.2, doorHeight: 3.2}, LIT)});

const hall = at({piece: narthex, socket: 'left'}, {kit: 'ruins', type: 'hall', params: Object.assign({
  width: 13, length: 34, height: 6.5, ceiling: 3.5, vault: 'groin', columnRows: '2', columnCount: 6, columnRadius: 0.55,
  columnStyle: 'fluted', aisleWidth: 6.5, dais: false, banners: 3, niches: true, tilePattern: 'border',
  collapse: 0.6, collapseFocus: 0.6, collapseAt: 0.8, fallCavity: 2.5, exits: 'through+left',
  settle: 0.65, breach: 0.55, jitter: 0.75, bays: 1, bayDepth: 2.4,
  keyLight: 'fire', keySize: 0.45, keyAt: 0.08, keySide: 0,
  decay: 0.65, moisture: 0.35, lightCount: 7, lightDensity: 0.2, lightType: 'brazier', asym: 0.4}, LIT)});

// crypt wing off the hall's side: a bent, settled passage, a flight lower
const cryptCorr = at({piece: hall, socket: 'left'}, {kit: 'ruins', type: 'corridor', params: {
  width: 2.2, length: 9, height: 2.6, profile: 'pointed', archSpacing: 1.8, ribDepth: 2, alcoves: 0.7, pairAlcoves: false,
  wander: 0.8, wanderWave: 10, jitter: 0.6, settle: 0.5, slope: -0.5, slopeSteps: 0,
  tilePattern: 'plain', decay: 0.55, moisture: 0.45, lightCount: 0, lightType: 'none'}});
// the crypts lie a flight lower than the hall: a steep, narrow, worn stair
const cryptStair = at({piece: cryptCorr, socket: 'out'}, {kit: 'ruins', type: 'stair', params: {
  kind: 'straight', direction: 'down', width: 2, height: 2.6, rise: 1.9, pitch: 0.8, stepHeight: 0.3, landing: 0.8, decay: 0.7, moisture: 0.5, lightCount: 0, lightType: 'none'}});
const gallery = at({piece: cryptStair, socket: 'out'}, {kit: 'ruins', type: 'crypt', preset: 'bone_gallery', params: {
  width: 3.2, length: 22, height: 3.4, ceiling: 1.2, tiers: 4, nicheCols: 9, nicheWidth: 1.1, nicheHeight: 0.5,
  filled: 0.75, ossuary: 0.7, guardians: false, exits: 'through', jitter: 0.8, settle: 0.45, breach: 0.3,
  lightCount: 0, lightType: 'none', decay: 0.55, moisture: 0.5}});
const tomb = at({piece: gallery, socket: 'out'}, {kit: 'ruins', type: 'crypt', preset: 'noble_tomb', params: Object.assign({
  width: 8, length: 9, height: 3.4, ceiling: 2.6, tiers: 1, nicheCols: 3, nicheWidth: 2, sarcophagi: 3, ossuary: 0.1,
  keyNiche: true, exits: 'dead end', settle: 0.3, bays: 1, bayDepth: 1.8, jitter: 0.4,
  lightCount: 1, lightType: 'candles', decay: 0.3}, LIT)});

// west of the narthex: a robbed side chapel, through a passage that has sagged
const westCorr = at({piece: narthex, socket: 'right'}, {kit: 'ruins', type: 'collapsed_passage', params: {
  width: 2.6, length: 8, height: 2.8, falls: 1, blockage: 0.35, blocks: 3, cracks: 0.7, cavity: 1.2, widthVar: 0.3, heightVar: 0.3}});
const chapel = at({piece: westCorr, socket: 'out'}, {kit: 'ruins', type: 'crypt', preset: 'plundered', params: {
  width: 5, length: 6, height: 3, ceiling: 1.8, tiers: 2, nicheCols: 3, sarcophagi: 1, exits: 'dead end',
  settle: 0.75, breach: 0.5, jitter: 0.8, lightCount: 0, lightType: 'none'}});

// ---- level 2: down from the hall's fallen end -----------------------------------
const fall = at({piece: hall, socket: 'out'}, {kit: 'ruins', type: 'collapsed_passage', params: {
  width: 2.8, length: 10, height: 3, falls: 2, blockage: 0.4, blocks: 5, cracks: 0.6, cavity: 2, widthVar: 0.25, heightVar: 0.35}});
const down = at({piece: fall, socket: 'out'}, {kit: 'ruins', type: 'stair', params: Object.assign({
  kind: 'straight', direction: 'down', width: 2.6, height: 3, rise: 9, pitch: 0.6, landing: 1.5, stepHeight: 0.24, lightCount: 1, lightType: 'candles', decay: 0.5}, LIT)});
const cistern = at({piece: down, socket: 'out'}, {kit: 'ruins', type: 'flooded_hall', preset: 'cistern', params: Object.assign({
  width: 14, length: 20, height: 5, ceiling: 2.5, columnCount: 4, sunk: 2.2, waterLevel: 0.9, columnStyle: 'square',
  jitter: 0.75, breach: 0.35,
  exits: 'left', lightCount: 5, lightType: 'crystal', warmth: 0.3, decay: 0.5, moisture: 0.9}, LIT)});
const pit = at({piece: cistern, socket: 'left'}, {kit: 'ruins', type: 'pit_room', params: Object.assign({
  width: 11, length: 13, height: 6, ceiling: 2, pitWidth: 0.6, pitLength: 0.62, pitDepth: 7, ledge: 1.4,
  bridge: 'stone', bridgeWidth: 1.6, bays: 1, bayDepth: 1.5, lightCount: 4, lightType: 'brazier'}, LIT)});
const approach = at({piece: pit, socket: 'out'}, {kit: 'ruins', type: 'corridor', preset: 'wide_gallery', params: Object.assign({
  width: 3.2, length: 10, height: 3.6, ceiling: 1.6, archSpacing: 3.2, alcoves: 0.6, pairAlcoves: true,
  wander: 1.0, wanderWave: 12, jitter: 0.5, settle: 0.3,
  lightCount: 4, lightType: 'brazier', tilePattern: 'runner'}, LIT)});
const sanctum = at({piece: approach, socket: 'out'}, {kit: 'ruins', type: 'sanctum', preset: 'great_sanctum', params: Object.assign({
  width: 14, length: 14, height: 7, ceiling: 6, pillarRing: 8, statues: 4, braziers: 4, tilePattern: 'border', jitter: 0.5, settle: 0.25,
  lightCount: 6, decay: 0.3}, LIT)});

// ---- hand-placed props (the Props palette) -------------------------------------
// Everything below is in the DUNGEON frame = the anchor frame (the entrance
// socket is 0,0,0): what the tab's inspector shows and refs.json stores.
const asm = G.assemble(d);
const placedOf = (id) => asm.placed.find(p => p.id === id);
const sockOf = (id, name) => placedOf(id).sockets.find(s => s.name === name);
// where a prop's local cell (x, z) lands for a prop yawed q quarters (3.4.1 Rot)
const rotCell = (q, x, z) => [[x, z], [z, -1 - x], [-1 - x, -1 - z], [-1 - z, x]][q & 3];
const headingYaw = (dir) => ({'0,0,1': 0, '1,0,0': 90, '0,0,-1': 180, '-1,0,0': 270})[dir.join(',')];
const HEADING = [[0, 0, 1], [1, 0, 0], [0, 0, -1], [-1, 0, 0]];   // HeadingAxis(yaw) per quarter
d.props = [];
let propN = 0;
const prop = (propId, params, pos, yaw, piece) => { d.props.push({id: 'x' + (propN++), prop: propId, params, pos: pos.map(Math.round), yaw, piece}); return d.props[d.props.length - 1]; };
/** A point `a` cells along a socket's axis (+ = out of its piece) and `s` across (+ = right looking out). */
const offS = (sk, a, s, dy) => { const rt = HEADING[((headingYaw(sk.dir) / 90) + 1) & 3]; return [0, 1, 2].map(k => sk.pos[k] + sk.dir[k] * a + rt[k] * s + (k === 1 ? (dy || 0) : 0)); };

// THE SANCTUM DOOR: an iron-bound leaf in the sanctum's doorway, a few cells
// into its wall, swinging out toward the approach. The door ref names exactly
// the leaf's cells (refs_doors.h frame = ruins/door's frame).
const sIn = sockOf(sanctum, 'in');                     // dir points OUT of the sanctum, toward the approach
const doorYaw = headingYaw(sIn.dir);                   // opens toward the approach
const inward = sIn.dir.map(v => -v);
const DW = sIn.w - 2, DH = 30;                         // the leaf: the opening less a cell each side
const right = HEADING[((doorYaw / 90) + 1) & 3];       // HeadingAxis(yaw + 90): the leaf runs this way from the hinge
const doorOrigin = [0, 1, 2].map(k => sIn.pos[k] + inward[k] * 4 - right[k] * Math.floor(DW / 2));
prop('ruins/door', {width: DW, height: DH, thickness: 2, style: 'iron_bound', straps: 3, tympanum: 8}, doorOrigin, doorYaw, sanctum);
const rc = rotCell(doorYaw / 90, 0, 0);
const doorRefPos = [doorOrigin[0] + rc[0], doorOrigin[1], doorOrigin[2] + rc[1]];

// LIGHT AND DRESSING by hand (the pieces' own light sliders place the wall
// braziers; these are the author's): great braziers where the story wants the
// eye, candles where someone still keeps a vigil. Positions are in each
// piece's own frame (origin socket at 0,0,0, body along +Z) and carried into
// the dungeon frame by the piece's transform, as the tab does.
const inPiece = (id, c, yaw) => {
  const pc = d.pieces.find(p => p.id === id), qq = (pc.yaw / 90) & 3;
  const r = rotCell(qq, c[0], c[2]);
  return {pos: [pc.pos[0] + r[0], pc.pos[1] + c[1], pc.pos[2] + r[1]], yaw: ((yaw || 0) + pc.yaw) % 360};
};
const lightAt = (id, propId, params, c, yaw) => { const t = inPiece(id, c, yaw); return prop(propId, params, t.pos, t.yaw, id); };
// the sanctum: four great braziers down the nave of the pillar ring, a vigil at the dais
for (const z of [55, 115]) for (const s of [-1, 1]) lightAt(sanctum, 'ruins/brazier', {height: 1.5, bowl: 0.7, style: 'stone', fire: 0.9}, [s * 46, 0, z]);
for (const s of [-1, 1]) lightAt(sanctum, 'ruins/candle_cluster', {count: 14, spread: 0.6, tallest: 0.4, flame: 0.8}, [s * 22, 9, 178]);
// the hall: a pair of tripods halfway down the aisle (the fire burns inside its door)
for (const s of [-1, 1]) lightAt(hall, 'ruins/brazier', {style: 'iron', height: 1.1, bowl: 0.5, fire: 0.8}, [s * 22, 0, 150]);
// the narthex: votive candles either side of the crossing
for (const s of [-1, 1]) lightAt(narthex, 'ruins/candle_cluster', {count: 9, spread: 0.45, flame: 0.6}, [s * 36, 0, 30]);
// the narthex: a weathered colossus against the far wall, the first thing the stair shows you
lightAt(narthex, 'ruins/statue', {height: 3.4, pose: 'arms_raised', plinth: 0.6, weathering: 0.6}, [0, 0, 80], 180);
// the court: one column snapped at head height, its drums across the paving
lightAt('p0', 'ruins/broken_column', {radius: 0.35, stump: 2.1, drums: 2, fluted: true}, [-30, 0, 62], 90);
// the pit: what the bridge has already taken
lightAt(pit, 'ruins/bones', {}, [6, -70, 80], 30);
// the noble tomb: stub candles beside the sarcophagus that holds the key
lightAt(tomb, 'ruins/candle_cluster', {count: 4, tallest: 0.12, spread: 0.2, flame: 0.5}, [-18, 0, 55]);

// ---- entities (refs.json) ------------------------------------------------------
const refs = [];
const ref = (name, kind, pos, yaw, props, base) => { const r = {name, kind, pos: pos.map(Math.round), yaw: yaw || 0, props: props || {}}; if (base) r.base = base; refs.push(r); return r; };
const progNow = G.compile(d, {id: ID});
const sug = (id) => { const r = progNow.refs.find(x => x.id === id); if (!r) throw new Error('no suggestion ' + id); return r; };
// Waynodes (the tab's "Adopt waynodes": wn_<piece>_<node>) -- patrol routes name them.
const wn = adoptWaynodes(progNow, ref);
// The sanctum door, locked with the bone key.
ref('sanctum_door', 'door', doorRefPos, doorYaw, {width: DW, height: DH, thickness: 2, hinge: 'left', lock: 'bone_key', openAngle: 100});
// The key: in the noble tomb, lying on the middle sarcophagus's effigy.
const s1 = sug(tomb + '_sarcophagus1');
ref('bone_key', 'key', [s1.pos[0], s1.pos[1] + 9, s1.pos[2]], 0, {item: 'bone_key'});
// Grave goods.
for (const k of [0, 2]) { const s = sug(tomb + '_sarcophagus' + k); ref('tomb_offering_' + k, 'container', s.pos, s.yaw, {loot: 'crypt_offerings', title: 'sarcophagus'}); }
{ const s = sug(tomb + '_key_niche'); ref('tomb_niche', 'container', s.pos, s.yaw, {loot: 'crypt_offerings', title: 'burial niche'}); }
{ const s = sug(chapel + '_sarcophagus0'); ref('chapel_sarcophagus', 'container', s.pos, s.yaw, {loot: 'crypt_offerings', title: 'robbed sarcophagus'}); }
{ const s = sug(sanctum + '_offering_chest'); ref('sanctum_offerings', 'container', s.pos, s.yaw, {loot: 'crypt_offerings', title: 'offering chest'}); }
{ const s = sug(sanctum + '_altar'); ref('altar', 'marker', s.pos, s.yaw, {tags: ['altar']}); }

// The warden: stands before the sanctum door, facing the approach.
const post = offS(sIn, 22, 0);
ref('warden_post', 'marker', post, doorYaw, {tags: ['warden_post']});
ref('warden', 'npc', post, doorYaw, {name: 'the Cairn Warden', behavior: 'guardian', schedule: 'cairn_warden', post: 'warden_post',
  weapon: 'mace', outfit: ['robe#2f2a36', 'hood#1f1c22', 'boots']}, 'human');
// The hall watch: one of the dead walking the nave, door to the fallen end and back.
const hallNodes = progNow.waynodes.filter(w => w.name.startsWith(hall + '/c')).map(w => w.name);
// the door, the middle of the nave, and the last node short of the fallen end
const route = [hallNodes[0], hallNodes[2], hallNodes[hallNodes.length - 3]].map(wn);
ref('hall_watch', 'npc', progNow.waynodes.find(w => w.name === hallNodes[1]).pos, 0, {name: 'a hollow-eyed watcher', behavior: 'zombie', schedule: 'cairn_hall_watch',
  outfit: ['robe#3a2f4a', 'sash']}, 'zombie');
// The crypt dead: wake when someone steps into the bone gallery.
{
  const gin = sockOf(gallery, 'in');
  const b = placedOf(gallery).bounds;
  const mid = [Math.round((b.min[0] + b.max[0]) / 2), gin.pos[1], Math.round((b.min[2] + b.max[2]) / 2)];
  const a0 = offS(gin, -16, -18), a1 = offS(gin, -2, 18);
  const mn = [0, 2].map(k => Math.min(a0[k], a1[k])), mx = [0, 2].map(k => Math.max(a0[k], a1[k]));
  ref('gallery_threshold', 'region', [mn[0], gin.pos[1], mn[1]], 0, {size: [mx[0] - mn[0] + 1, 30, mx[1] - mn[1] + 1]});
  ref('crypt_dead', 'spawner', mid, 0, {mobs: [{base: 'zombie', weight: 3}, {base: 'human', weight: 1}], count: 3, radius: 3.5, respawn: 'onLeave',
    trigger: 'onEnter(gallery_threshold)', behavior: 'zombie', outfit: ['robe#3a2f4a']});
}
// The pit room's bridge gives way: a plate in the middle of the span, the
// region the slab over the pit. collapse_floor turns the limestone to gravel.
{
  const pc = d.pieces.find(x => x.id === pit), P = pc.params, qq = (pc.yaw / 90) & 3;
  const T = Math.round(P.wallThick * 10), L = Math.round(P.length * 10), zc = Math.round((T + T + L) / 2);
  const pl = Math.min(L - 16, Math.round(L * P.pitLength)), pz0 = zc - Math.round(pl / 2), pz1 = pz0 + pl, bw = Math.round(P.bridgeWidth * 10 / 2);
  const rv = (x, z) => [[x, z], [z, -x], [-x, -z], [-z, x]][qq];
  // a piece-local half-open box -> the dungeon frame, as {pos: min cell, size}
  const box = (x0, y0, z0, x1, y1, z1) => {
    const a = rv(x0, z0), b = rv(x1, z1);
    return {pos: [Math.min(a[0], b[0]) + pc.pos[0], y0 + pc.pos[1], Math.min(a[1], b[1]) + pc.pos[2]],
            size: [Math.abs(b[0] - a[0]), y1 - y0, Math.abs(b[1] - a[1])]};
  };
  const span = box(-bw, -6, pz0 + 2, bw, 0, pz1 - 2);          // the slab over the pit, not the ledges
  ref('bridge_span', 'region', span.pos, 0, {size: span.size});
  const zm = Math.round((pz0 + pz1) / 2), mid = box(-bw, 0, zm - 8, bw, 2, zm + 8);
  ref('bridge_plate', 'plate', mid.pos, 0, {size: mid.size, latch: true, who: 'any'});
  ref('bridge_collapse', 'trap', span.pos, 0, {type: 'collapse_floor', on: 'bridge_plate', region: 'bridge_span'});
}

// ---- outputs ---------------------------------------------------------------------
const SCHEDULES = {
  cairn_warden: {about: 'The Tarn Cairn (dungeon heather_cairn): the warden stands before the sanctum door all day, facing the way in (the marker its `post` role names).',
                 rows: [{from: '00:00', to: '24:00', do: 'guard', at: 'post'}]},
  cairn_hall_watch: {about: 'The Tarn Cairn (dungeon heather_cairn): one of the dead walking the columned hall, door to the fallen end and back.',
                     rows: [{from: '00:00', to: '24:00', do: 'patrol', route, mode: 'pingpong', wait: 3}]}
};
if (DRY) {
  d.refs = 'refs.json';
  process.exit(report(ID, d, refs, G.compile(d, {id: ID, materials: MATS})) ? 1 : 0);
}
writeSchedules(SCHEDULES);
const prog = writeDungeon(ID, d, refs, 'The Tarn Cairn: built by assets/dungeons/heather_cairn/build.mjs (edit there or in the Dungeons tab).');
process.exit(report(ID, d, refs, prog) ? 1 : 0);
