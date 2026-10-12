/* dungeon_teeth_caves.mjs — THE VERDANT TEETH SEA CAVES (dungeon `teeth_caves`),
 * authored through the same module API the tuner's Dungeons tab runs
 * (assets/editor/dungeon/dungeongen.js attachPiece / compile / validate,
 * ui_entities.js writeRefs / forValidate / refProblems), so re-running this
 * script reproduces the asset byte for byte, and opening the result in the
 * tab (Dungeons > dungeon "teeth_caves") is where it is inspected and tuned.
 *
 *   node assets/worldmap/default/scripts/dungeon_teeth_caves.mjs           write the dungeon + refs + shell
 *   node assets/worldmap/default/scripts/dungeon_teeth_caves.mjs --ground  ... and validate against the
 *        REAL ground: runs `sandvox.exe --heightmap` (GPU-free, no lock; SANDVOX_EXE picks the exe) over
 *        the placement and prints the validation panel the tab shows "in world"
 *   ... --map   also write the `teeth_caves` entry of assets/worldmap/default/map.json dungeons[]
 *
 * THE PLACE (docs/WORLD_tirnhallow.md: the Verdant Teeth, karst towers on the
 * coast of Teeth Sound). The map marker "The Verdant Teeth sea caves" sits on
 * the coastal flat at (-81000, 87500), 4 m above the sea; 330 m east-north-east
 * of it the first karst tower of the Teeth stands out of the flat: a 50 m
 * column, sheer on its sea-facing west side, 55 m tall over a plain 28 m above
 * the sea, with the great massif of the Teeth rising 60 m to the south of it.
 * The mouth opens at the foot of the tower's west cliff, looking out over the
 * flat to the sea; the cave climbs into the tower, and a shaft drops from the
 * tower's hollow heart to the sea-level caves under the massif.
 *
 * THE STORY THE ROCK TELLS. Smugglers used the caves: their rope still hangs in
 * the pitch, their cache sits on the island of the glowing pool (locked; the
 * key is with what is left of one of them in the snakes' den), and their dead
 * still walk the lower galleries. Lit for mood: the mouth by daylight, the
 * passage and the crawl DARK, the Hollow by cold crystal light from above,
 * the lower galleries by sparse warm fungus, the grotto by its own glow.
 */
import * as G from '../../../editor/dungeon/dungeongen.js';
import * as E from '../../../editor/dungeon/ui_entities.js';
import { readFileSync, writeFileSync, mkdirSync, readdirSync } from 'fs';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const HERE = dirname(fileURLToPath(import.meta.url));
const ASSETS = join(HERE, '..', '..', '..');
const ROOT = join(ASSETS, '..');
const ID = 'teeth_caves';
const OUT = join(ASSETS, 'dungeons', ID);
const args = process.argv.slice(2);

// ---- the placement (map.json dungeons[]) -----------------------------------------
export const PLACEMENT = {id: ID, name: 'The Verdant Teeth sea caves', dungeon: ID, at: [-78684, 89590], yOffset: 0, yaw: 90};

// ---- the pieces --------------------------------------------------------------------
let d = G.newDungeon({name: 'The Verdant Teeth sea caves', style: 'caves', seed: 61873});
// Jungle karst palette: pale limestone, white flowstone, ferns on the sills,
// vines hanging from the brows, wet moss, cold cyan / violet crystal.
// light = candle_flame: the smugglers' fire baskets and lanterns burn warm against the caves' cold crystal.
d.palette = {variant: 'limestone', soil: 'shore_mud', hang: 'vine_hang', veg: 'fern', moss: 'wet_moss', trim: 'chalk_rock', light: 'candle_flame'};

const ids = {};
function first(name, spec) {
  d.pieces.push(G.newPiece(d, spec));
  ids[name] = d.pieces[d.pieces.length - 1].id;
  return ids[name];
}
function attach(name, from, socket, spec) {
  d = G.attachPiece(d, {piece: ids[from], socket}, Object.assign({fit: false, checkOverlap: true}, spec)).dungeon;
  ids[name] = d.pieces[d.pieces.length - 1].id;
  const pc = d.pieces[d.pieces.length - 1];
  if (spec.label) pc.label = spec.label;
  return ids[name];
}

// LEVEL 0 -- the tower.
first('mouth', {kit: 'caves', type: 'mouth', params: {
  variant: 'hillside', width: 7.5, height: 5.5, length: 11, flare: 1.7, drop: 1, innerWidth: 3.2, lip: 0.95, apron: 3, talus: 0.55,
  wallRough: 0.75, wallScale: 2.2, floorUneven: 0.5, moisture: 0.7, decay: 0.35, asym: 0.8, blend: 8}});
d.entrance = {piece: ids.mouth, socket: 'surface'};
attach('passage', 'mouth', 'in', {kit: 'caves', type: 'tunnel', params: {
  width: 2.6, length: 11, rise: 1.2, endShift: -1.5, meander: 2.2, meanderFreq: 1.4, squeezes: 1, squeeze: 0.7, radiusVar: 0.35, flatten: 0.55,
  wallRough: 0.6, wallScale: 1.4, moisture: 0.55, decay: 0.3, stalactites: 0.25, stalagmites: 0.1, stalLength: 0.6, lightCount: 0, blend: 8, asym: 0.6}});
attach('crawl', 'passage', 'out', {kit: 'caves', type: 'crawl', params: {
  width: 2.2, height: 1.05, length: 7, meander: 1.2, pinches: 2, pinch: 0.8, floorRubble: 0.6, rise: 0.4, wallRough: 0.4, moisture: 0.3, decay: 0.5}});
attach('hollow', 'crawl', 'out', {kit: 'caves', type: 'cavern', params: {
  mouth: 2.2, width: 24, length: 17, height: 8, ceiling: 18, clusters: 4, flatten: 0.75, exits: 3,
  stalactites: 0.55, stalagmites: 0.25, stalLength: 2.8, columns: 2, flowstone: 0.8, crystals: 0.8, mushrooms: 0, pool: 0, boulders: 5,
  fangs: 8, fangLength: 11, fangGirth: 1.3, fangUp: 0.3, fangJoin: 0.25,
  wallRough: 0.85, wallScale: 4, floorUneven: 0.6, moisture: 0.45, decay: 0.45, lightCount: 8, lightType: 'crystal', warmth: 0.25, blend: 12, asym: 0.9}});
attach('geode', 'hollow', 'out', {kit: 'caves', type: 'cavern', params: {
  mouth: 2.2, width: 8, length: 7, height: 3, ceiling: 3.5, clusters: 2, flatten: 0.85, exits: 0, crystals: 1, mushrooms: 0, pool: 0, boulders: 0, columns: 0,
  flowstone: 0.2, stalactites: 0.4, stalLength: 0.8, wallRough: 0.55, wallScale: 1.2, lightCount: 5, lightType: 'crystal', warmth: 0.6, moisture: 0.1, decay: 0.1, blend: 6}});
attach('cleft', 'hollow', 'right', {kit: 'caves', type: 'crawl', params: {
  width: 3.4, height: 1.25, length: 4, meander: 0.6, pinches: 1, pinch: 0.85, floorRubble: 0.3, rise: 0.8, wallRough: 0.5, wallScale: 0.8, moisture: 0.1, decay: 0.3}});
attach('stash', 'cleft', 'out', {kit: 'caves', type: 'cavern', params: {
  mouth: 2.4, width: 7, length: 6, height: 2.6, ceiling: 1.6, clusters: 2, flatten: 0.9, exits: 0, crystals: 0, mushrooms: 0, pool: 0, boulders: 1, columns: 0,
  flowstone: 0.4, stalactites: 0.5, stalagmites: 0, stalLength: 0.5, wallRough: 0.45, wallScale: 1, decay: 0.2, moisture: 0.05, lightCount: 0, lightType: 'none', blend: 6}});
attach('pitch', 'hollow', 'left', {kit: 'caves', type: 'shaft', params: {
  entry: 'side', lipLength: 3.5, lipWidth: 2.2, lipBrow: 3, width: 5, length: 22, height: 4, wander: 0.8, ledges: 4, ledgeWidth: 1.1, rope: true, footPool: 0,
  wallRough: 0.7, wallScale: 2, moisture: 0.6, decay: 0.3, lightCount: 1, lightType: 'fungus', warmth: 1, blend: 8}});

// LEVEL -1 -- sea level, under the massif.
attach('descent', 'pitch', 'foot', {kit: 'caves', type: 'tunnel', params: {
  width: 3, length: 20, rise: -1, endShift: 4, meander: 3.5, meanderFreq: 1.7, squeezes: 2, squeeze: 0.6, radiusVar: 0.4, flatten: 0.6,
  wallRough: 0.7, wallScale: 1.8, moisture: 0.75, decay: 0.25, stalactites: 0.4, stalagmites: 0.2, stalLength: 1.4, lightCount: 1, lightType: 'fungus', blend: 8, asym: 0.7}});
attach('rift', 'descent', 'out', {kit: 'caves', type: 'chasm', params: {
  width: 5, length: 22, height: 5, ceiling: 9, crackWidth: 7, crackLength: 34, crackDepth: 14, crackAngle: 0.55, bridge: 'natural', bridgeWidth: 2.2, fogDrop: true,
  wallRough: 0.8, wallScale: 2.6, moisture: 0.6, decay: 0.3, lightCount: 2, lightType: 'fungus', warmth: 0.5, blend: 10, asym: 0.6}});
attach('maw', 'rift', 'out', {kit: 'caves', type: 'cavern', params: {
  mouth: 4.6, width: 38, length: 46, height: 9, ceiling: 15, clusters: 6, flatten: 0.7, exits: 3,
  stalactites: 0.7, stalagmites: 0.35, stalLength: 3, columns: 3, flowstone: 0.7, crystals: 0.6, mushrooms: 10, pool: 0.35, boulders: 8,
  fangs: 14, fangLength: 12, fangGirth: 1.8, fangUp: 0.35, fangJoin: 0.2,
  wallRough: 1, wallScale: 5, floorUneven: 0.8, moisture: 0.6, decay: 0.6, lightCount: 8, lightType: 'fungus', warmth: 0.85, blend: 14, asym: 1}});
attach('grotto', 'maw', 'left', {kit: 'caves', type: 'grotto_pool', params: {
  mouth: 3.2, width: 18, length: 20, height: 4, ceiling: 7, clusters: 3, exits: 0, poolSize: 0.75, poolDepth: 3, waterLevel: 0.9, beach: 0.3, island: true, glow: 1,
  stalactites: 0.65, stalLength: 2.2, fangs: 5, fangLength: 5, fangGirth: 0.7, fangUp: 0, fangJoin: 0,
  wallRough: 0.6, wallScale: 2.4, moisture: 1, decay: 0.1, lightCount: 6, lightType: 'fungus', warmth: 0.2, blend: 10}});
attach('choke', 'maw', 'right', {kit: 'caves', type: 'rubble_choke', params: {
  width: 3.4, length: 12, height: 3, choke: 0.85, chokeLength: 0.7, gap: 0.7, boulders: 6, radiusVar: 0.4, wallRough: 0.6, decay: 0.8, moisture: 0.3}});
attach('den', 'choke', 'out', {kit: 'caves', type: 'cavern', params: {
  mouth: 3.2, width: 14, length: 13, height: 3.2, ceiling: 3, clusters: 4, flatten: 0.55, exits: 0, crystals: 0, mushrooms: 2, pool: 0, boulders: 4, columns: 1,
  flowstone: 0.3, stalactites: 0.4, stalagmites: 0.5, stalLength: 1.2, wallRough: 0.75, wallScale: 1.6, floorUneven: 0.9, decay: 0.95, moisture: 0.25,
  lightCount: 1, lightType: 'fungus', warmth: 1, blend: 8, asym: 1}});
attach('throat', 'maw', 'out', {kit: 'caves', type: 'tunnel', params: {
  width: 4.4, length: 18, rise: -1, endShift: -5, meander: 2, meanderFreq: 0.9, squeezes: 0, radiusVar: 0.2, flatten: 0.45,
  wallRough: 0.35, wallScale: 3, moisture: 0.9, decay: 0.1, stalactites: 0.05, lightCount: 0, blend: 10}});
attach('sump', 'throat', 'out', {kit: 'caves', type: 'grotto_pool', params: {
  mouth: 4.4, width: 26, length: 28, height: 5, ceiling: 6, clusters: 4, exits: 0, poolSize: 0.9, poolDepth: 4.5, waterLevel: 0.95, beach: 1, island: false, glow: 0.15,
  stalactites: 0.2, stalLength: 1.5, wallRough: 0.45, wallScale: 3.5, moisture: 1, decay: 0.15, lightCount: 2, lightType: 'crystal', warmth: 0, blend: 12}});

// ---- L2 props + L4 entities ------------------------------------------------------
// Everything is placed off what the pieces generated (their waynodes and prims),
// the way the tab's inspector shows them, so a slider change upstream carries the
// furniture with the rock instead of leaving it in the wall.
const MATS = JSON.parse(readFileSync(join(ASSETS, 'materials', 'materials.json'), 'utf8')).materials.map(m => m.id);
const prog0 = G.compile(d, {id: ID, materials: MATS});
const A = prog0.entrance.dungeonPos;                       // anchor frame -> dungeon frame offset
const toD = (p) => [p[0] + A[0], p[1] + A[1], p[2] + A[2]];
const way = (n) => { const w = prog0.waynodes.find(x => x.name === ids[n.split('/')[0]] + '/' + n.split('/')[1]); if (!w) throw new Error('no waynode ' + n); return w.pos.slice(); };
const off = (p, dx, dy, dz) => [p[0] + dx, p[1] + dy, p[2] + dz];
const placedOf = (name) => G.placePiece(d, d.pieces.find(p => p.id === ids[name]));
// The grotto's island: the wall-material ellipsoid fill on layer 6 (kit_caves GROTTO).
const islandPrim = placedOf('grotto').prims.find(p => p.op === 'fill' && p.shape === 'ellipsoid' && p.mat === 'wall' && p.layer === 6);
const island = [Math.round((islandPrim.min[0] + islandPrim.max[0]) / 2) - A[0], islandPrim.max[1] - A[1], Math.round((islandPrim.min[2] + islandPrim.max[2]) / 2) - A[2]];

const P = (id, prop, params, pos, yaw, piece, seed) => d.props.push({id, prop, params, seed, pos: toD(pos), yaw, piece: ids[piece]});
// The smugglers' cache on the island of the glowing pool (locked: old_key).
P('x_cache', 'caves/sea_chest', {length: 1.1, depth: 0.6, height: 0.65, bands: 3, domed: true, rot: 0.15, footing: 5, clear: 2}, island, 90, 'grotto', 11);
// Their stash behind the cleft in the tower: a rotten chest and a cold hearth.
P('x_stash', 'caves/sea_chest', {length: 0.9, depth: 0.5, height: 0.5, bands: 1, domed: false, rot: 0.55, footing: 4, clear: 2}, off(way('stash/c'), 8, 0, 6), 180, 'stash', 12);
P('x_hearth', 'caves/campfire_remains', {radius: 0.45, logs: 2, pot: false, bedrolls: 1}, off(way('stash/c'), -10, 0, -4), 0, 'stash', 13);
// The den: what the snakes left of a smuggler, with his key.
P('x_bones', 'caves/bone_pile', {radius: 1.4, height: 0.5, skulls: 2, gore: 0.3}, off(way('den/c'), 14, 0, 18), 0, 'den', 14);
P('x_bones2', 'caves/bone_pile', {radius: 0.7, height: 0.3, skulls: 1, gore: 0.1}, off(way('den/c'), -22, 0, -6), 0, 'den', 15);
// The sea's leavings on the beach of the sump.
P('x_drift1', 'caves/driftwood', {length: 3.2, girth: 0.25, logs: 3, root: true, kelp: 0.6, sink: 1}, off(way('sump/shore'), 20, 0, 24), 0, 'sump', 16);
P('x_drift2', 'caves/driftwood', {length: 1.8, girth: 0.18, logs: 1, root: false, kelp: 0.3, sink: 1}, off(way('sump/shore'), -26, 0, -16), 90, 'sump', 17);
// The smugglers' fire baskets in the Maw (iron tripods) and a lantern at the foot of the pitch.
P('x_basket1', 'ruins/brazier', {style: 'iron', height: 1.1, bowl: 0.45, lit: 'light'}, [-900, way('maw/c')[1], 400], 0, 'maw', 21);
P('x_basket2', 'ruins/brazier', {style: 'iron', height: 0.9, bowl: 0.4, lit: 'light'}, [-1100, way('maw/c')[1], 480], 0, 'maw', 22);
P('x_basket3', 'ruins/brazier', {style: 'iron', height: 1.2, bowl: 0.5, lit: 'light'}, [-960, way('maw/c')[1], 300], 0, 'maw', 23);
P('x_lamp_pitch', 'mines/lantern', {mount: 'floor', lit: 'light'}, off(way('pitch/exit'), 23, 1, 4), 0, 'pitch', 24);
P('x_lamp_stash', 'mines/lantern', {mount: 'floor', lit: 'light'}, off(way('stash/c'), -4, 0, 10), 0, 'stash', 25);
// The rope the smugglers left at the mouth of the Hollow's pitch is the shaft's own ladder.

const refs = [];
const R = (name, kind, pos, yaw, props, base) => { const r = {name, kind, pos: pos.map(Math.round), yaw: yaw || 0, props: props || {}}; if (base) r.base = base; refs.push(r); return r; };
// Waynode refs (the tab's "Adopt waynodes": wn_<piece>_<node>), for the patrol routes.
const wnName = (n) => 'wn_' + n.replace(/\//g, '_');
for (const w of prog0.waynodes) {
  if (w.pit) continue;
  R(wnName(w.name), 'waynode', w.pos, 0, {links: w.links.filter(l => !prog0.waynodes.find(o => o.name === l && o.pit)).map(wnName)});
}
const wnRef = (n) => wnName(ids[n.split('/')[0]] + '/' + n.split('/')[1]);
// Posts the schedules name.
R('post_pitch', 'marker', way('pitch/exit'), 90, {tags: ['teeth_post_pitch']});
R('lair_nest', 'marker', off(way('den/c'), 10, 0, 12), 0, {tags: ['teeth_lair_nest']});
R('ambush_bridge', 'marker', way('rift/w3'), 270, {tags: ['teeth_ambush_bridge']});
// The guard at the foot of the pitch: whoever comes down the rope meets him.
R('drowned_guard', 'npc', way('pitch/exit'), 90, {behavior: 'zombie', schedule: 'teeth_guard'}, 'zombie');
// The bridge: one of them stands stock still on the far side until you are on it.
R('bridge_lurker', 'npc', off(way('rift/w3'), 0, 0, 6), 270, {behavior: 'zombie', schedule: 'teeth_ambush'}, 'zombie');
// The den: a snake asleep on the bones, and the key among them.
R('den_snake', 'npc', off(way('den/c'), 6, 0, 10), 180, {behavior: 'snake', schedule: 'teeth_lair'}, 'snake');
R('den_key', 'key', off(way('den/c'), 14, 3, 18), 0, {item: 'old_key'});
// The Maw: the drowned crew rise when you walk in, and walk the hall end to end.
const mawDoor = way('maw/door');
R('maw_hall', 'region', off(mawDoor, -60, -6, -70), 0, {size: [70, 60, 140]});
R('maw_dead', 'spawner', way('maw/c'), 90, {mobs: [{base: 'zombie', weight: 3}], count: 3, radius: 4, respawn: 'onLeave', trigger: 'onEnter(maw_hall)',
  behavior: 'zombie', schedule: 'teeth_patrol'});
// The grotto: piranhas in the glowing pool, only once you are on its shore.
const shore = way('grotto/shore');
R('grotto_shore', 'region', off(shore, -40, -6, -20), 0, {size: [80, 50, 60]});
R('pool_teeth', 'spawner', [island[0], island[1] - 18, island[2] + 40], 0, {mobs: [{base: 'piranha', weight: 1}], count: 3, radius: 3, respawn: 'never',
  trigger: 'onEnter(grotto_shore)', behavior: 'piranha'});
// The caches.
R('smugglers_cache', 'container', island, 90, {loot: 'cave_cache', title: 'sea chest', lock: 'old_key'});
R('stash_box', 'container', off(way('stash/c'), 8, 0, 6), 180, {loot: 'cave_cache', title: 'rotten chest'});
// The descent's tripwire: a plate across the passage and a dart from the wall.
const tw = way('descent/w3');
R('tripwire', 'plate', off(tw, -10, 0, -12), 0, {size: [20, 2, 24], who: 'any'});
R('dart_wall', 'trap', off(tw, 0, 12, 12), 180, {type: 'dart', on: 'tripwire', damage: 18, range: 60, cooldown: 4});

// Schedules: shared files, one per role, all-day rows.
const SCHED = {
  teeth_guard: {about: 'Verdant Teeth sea caves: the drowned smuggler who stands at the foot of the pitch (dungeon teeth_caves).',
                rows: [{from: '00:00', to: '24:00', do: 'guard', at: 'post_pitch'}]},
  teeth_lair: {about: 'Verdant Teeth sea caves: the snake asleep on the bones in the den; wakes for anyone within 5 m (dungeon teeth_caves).',
               rows: [{from: '00:00', to: '24:00', do: 'lair', at: 'lair_nest', radius: 5}]},
  teeth_ambush: {about: 'Verdant Teeth sea caves: the dead man waiting on the far side of the rock bridge (dungeon teeth_caves).',
                 rows: [{from: '00:00', to: '24:00', do: 'ambush', at: 'ambush_bridge', radius: 6}]},
  teeth_patrol: {about: 'Verdant Teeth sea caves: the drowned crew walking the Maw from the bridge to the sump passage and back (dungeon teeth_caves).',
                 rows: [{from: '00:00', to: '24:00', do: 'patrol', route: [wnRef('rift/w4'), wnRef('maw/door'), wnRef('maw/c'), wnRef('maw/x_out'), wnRef('throat/w2')], mode: 'pingpong', wait: 3}]}
};

// ---- outputs ---------------------------------------------------------------------
mkdirSync(OUT, {recursive: true});
d.refs = 'refs.json';
const text = G.dungeonJsonText(d);
writeFileSync(join(OUT, 'dungeon.json'), text);
const prog = G.compile(d, {id: ID, sourceText: text, materials: MATS});
writeFileSync(join(OUT, 'shell.json'), G.emitShellJson(prog));
writeFileSync(join(OUT, 'shell.bin'), G.emitShellBin(prog));
writeFileSync(join(OUT, 'refs.json'), E.writeRefs(ID, 'The Verdant Teeth sea caves (assets/worldmap/default/scripts/dungeon_teeth_caves.mjs): the smugglers\' dead, the snakes\' den, the cache on the island and its key, the tripwire in the descent.', refs));
for (const [name, s] of Object.entries(SCHED)) {
  const L = ['{', '  "about": ' + JSON.stringify(s.about) + ',', '  "rows": ['];
  s.rows.forEach((r, i) => L.push('    ' + JSON.stringify(r).replace(/,"/g, ', "').replace(/":/g, '": ') + (i + 1 < s.rows.length ? ',' : '')));
  L.push('  ]', '}');
  writeFileSync(join(ASSETS, 'schedules', name + '.json'), L.join('\n') + '\n');
}

// The catalog the tab's pickers and its ref checks read (tuner_server.py _dungeon_catalog).
function catalog() {
  const stems = (dir, ext) => { try { return readdirSync(dir).filter(n => n.endsWith(ext)).map(n => n.slice(0, -ext.length)); } catch (e) { return []; } };
  let mobs = stems(join(ASSETS, 'mobs'), '.vox');
  for (const sub of readdirSync(join(ASSETS, 'mobs'), {withFileTypes: true}))
    if (sub.isDirectory() && sub.name !== 'effects' && sub.name !== 'pool' && !sub.name.startsWith('.')) mobs = mobs.concat(stems(join(ASSETS, 'mobs', sub.name), '.vox'), stems(join(ASSETS, 'mobs', sub.name), '.json'));
  const items = JSON.parse(readFileSync(join(ASSETS, 'items', 'items.json'), 'utf8'));
  const keys = (items.items || items).filter(it => it.kind === 'key').map(it => ({id: it.id, unlocks: it.unlocks || it.id}));
  const behaviors = JSON.parse(readFileSync(join(ASSETS, 'mobs', 'behaviors.json'), 'utf8')).profiles.map(p => p.name);
  return {loot: stems(join(ASSETS, 'loot'), '.json'), schedules: stems(join(ASSETS, 'schedules'), '.json'), mobs, behaviors, keys};
}
const CAT = catalog();
const refIssues = () => E.refProblems(refs, CAT, new Set(prog.waynodes.map(w => w.name)));
for (const r of refs) if ((r.kind === 'npc' || r.kind === 'spawner') && r.props.behavior && !CAT.behaviors.includes(r.props.behavior)) console.log('  REF ERROR ' + r.name + ': no behaviour ' + r.props.behavior);
console.log(refs.length + ' refs (' + Object.entries(refs.reduce((a, r) => (a[r.kind] = (a[r.kind] || 0) + 1, a), {})).map(([k, n]) => n + ' ' + k).join(', ') + ')');
console.log(ID + ': ' + prog.pieces.length + ' pieces, ' + prog.prims.length + ' prims, ' + prog.props.instances.length + ' prop instances, ' +
            prog.waynodes.length + ' waynodes; worst 64^3 cell ' + prog.limits.primsPerCell + ' prims / ' + prog.limits.instancesPerCell + ' instances');
console.log('bounds (anchor frame) ' + JSON.stringify(prog.bounds));
for (const e of prog.errors) console.log('  COMPILE ERROR ' + (e.piece || '-') + ': ' + e.msg);

// ---- the map placement -----------------------------------------------------------
// Only the `teeth_caves` entry of dungeons[] is touched (the array is added after
// `locales` if the map has none); the rest of map.json is left byte for byte.
if (args.includes('--map')) {
  const mp = join(ASSETS, 'worldmap', 'default', 'map.json');
  let mt = readFileSync(mp, 'utf8');
  const entry = '    ' + JSON.stringify(PLACEMENT).replace(/,"/g, ', "').replace(/":/g, '": ').replace(/^\{/, '{ ').replace(/\}$/, ' }');
  const eol = mt.includes('\r\n') ? '\r\n' : '\n';
  const lines = mt.split('\n').map(l => l.replace(/\r$/, ''));
  const mine = lines.findIndex(l => l.includes('"id": "' + ID + '"') && l.includes('"dungeon"'));
  if (mine >= 0) {
    lines[mine] = entry + (lines[mine].trimEnd().endsWith(',') ? ',' : '');
  } else {
    const open = lines.findIndex(l => /^  "dungeons": \[/.test(l));
    if (open >= 0) {
      if (/\]\s*,?\s*$/.test(lines[open])) {            // "dungeons": [] on one line
        const tail = lines[open].trimEnd().endsWith(',') ? ',' : '';
        lines.splice(open, 1, '  "dungeons": [', entry, '  ]' + tail);
      } else lines.splice(open + 1, 0, entry + ',');
    } else {
      const loc = lines.findIndex(l => /^  "locales": \[/.test(l));
      let end = loc;
      while (end < lines.length && !/^  \],?\s*$/.test(lines[end])) end++;
      lines.splice(end + 1, 0, '  "dungeons": [', entry, '  ],');
    }
  }
  mt = lines.join(eol);
  JSON.parse(mt);
  writeFileSync(mp, mt);
  console.log('map.json dungeons[] ' + ID + ': ' + entry.trim());
}

// ---- the real ground -------------------------------------------------------------
// The anchor frame -> world: world = at + R(yaw)(local), R(90): (x, z) -> (z, -x).
const q = (PLACEMENT.yaw / 90) & 3;
const toWorld = (x, z) => { const r = [[x, z], [z, -x], [-x, -z], [-z, x]][q]; return [PLACEMENT.at[0] + r[0], PLACEMENT.at[1] + r[1]]; };
function loadHeights(box) {
  let cx, cz, span, res;
  if (box) [cx, cz, span, res] = box;
  else {
    const b = prog.bounds;
    const cs = [[b.min[0], b.min[2]], [b.max[0], b.min[2]], [b.min[0], b.max[2]], [b.max[0], b.max[2]]].map(c => toWorld(c[0], c[1]));
    const xs = cs.map(c => c[0]), zs = cs.map(c => c[1]);
    cx = Math.round((Math.min(...xs) + Math.max(...xs)) / 2); cz = Math.round((Math.min(...zs) + Math.max(...zs)) / 2);
    span = Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...zs) - Math.min(...zs)) + 256;
    res = Math.min(1024, Math.ceil(span / 2));
  }
  const exe = process.env.SANDVOX_EXE || join(ROOT, 'build', 'Release', 'sandvox.exe');
  const out = join(ROOT, 'build', 'teeth_heights.bin');
  mkdirSync(dirname(out), {recursive: true});
  execFileSync(exe, ['--heightmap', [cx, cz, span, res].join(','), '--heightmap-out', out],
               {stdio: ['ignore', 'ignore', 'inherit'], env: Object.assign({}, process.env, {SANDVOX_ASSET_DIR: ASSETS, SANDVOX_MAP: 'default', SANDVOX_NO_CRASH_DIALOG: '1'})});
  const buf = readFileSync(out);
  const n = buf.readUInt32LE(8), sp = buf.readUInt32LE(12), hx = buf.readInt32LE(16), hz = buf.readInt32LE(20);
  const at = (x, z) => {
    const i = Math.floor((x - (hx - sp / 2)) / sp * n), j = Math.floor((z - (hz - sp / 2)) / sp * n);
    if (i < 0 || j < 0 || i >= n || j >= n) return null;
    const o = 48 + (j * n + i) * 8, h = buf.readInt32LE(o), w = buf.readInt16LE(o + 4);
    return {ground: h, water: w === -32768 ? null : h + w};
  };
  return at;
}

if (args.includes('--ground')) {
  const H = loadHeights();
  // The anchor column itself at one-voxel resolution (the coarse map's cell may straddle the cliff foot).
  const g0 = loadHeights([PLACEMENT.at[0], PLACEMENT.at[1], 16, 16])(PLACEMENT.at[0], PLACEMENT.at[1]);
  const AY = g0.ground + PLACEMENT.yOffset;
  console.log('anchor world (' + PLACEMENT.at[0] + ', ' + AY + ', ' + PLACEMENT.at[1] + '), ground ' + g0.ground + ' (' + ((g0.ground - 112) / 10).toFixed(1) + ' m above the sea)');
  const groundAt = (x, z) => { const w = toWorld(x + 0.5, z + 0.5); const r = H(w[0], w[1]); return r ? r.ground - AY : Infinity; };
  const waterAt = (x, z) => { const w = toWorld(x + 0.5, z + 0.5); const r = H(w[0], w[1]); return r && r.water !== null ? r.water - AY : null; };
  // Cover per piece: the least rock over its bounds' top (un-noised; the validator does the exact test).
  for (const pc of prog.pieces) {
    let worst = Infinity, at = null;
    const b = pc.bounds;
    for (let x = b.min[0]; x <= b.max[0]; x += 8) for (let z = b.min[2]; z <= b.max[2]; z += 8) {
      const c = groundAt(x, z) - b.max[1];
      if (c < worst) { worst = c; at = [x, z]; }
    }
    const w = toWorld(Math.round((b.min[0] + b.max[0]) / 2), Math.round((b.min[2] + b.max[2]) / 2));
    console.log('  ' + pc.id.padEnd(4) + (d.pieces.find(p => p.id === pc.id).label || pc.type).padEnd(14) + ' floor~' + String(b.min[1]).padStart(5) + '..' + String(b.max[1]).padStart(4) +
                '  centre world (' + w[0] + ', ' + w[1] + ')  min cover over box top ' + worst + (pc.surface ? ' (surface piece)' : '') + ' at ' + at);
  }
  const panel = G.validate(d, {materials: MATS, groundAt, waterAt, refs: E.forValidate(refs, CAT), keyItems: CAT.keys}).concat(refIssues());
  console.log('validation: ' + panel.filter(v => v.severity === 'error').length + ' errors, ' + panel.filter(v => v.severity === 'warn').length + ' warnings');
  for (const v of panel) if (v.severity !== 'info') console.log('  ' + v.severity.padEnd(5) + ' ' + v.code.padEnd(11) + ' ' + (v.piece || '-') + ': ' + v.msg);
}
