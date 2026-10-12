/* build.mjs — THE SHARDWELL, the example mine dungeon (package D6, D8 variety
 * pass), authored through the Dungeons tab's own module API:
 * dungeongen.newPiece / attachPiece / compile / validate and ui_entities'
 * refs.json format. Re-running it rewrites
 * assets/dungeons/shardwell/{dungeon.json, shell.json, shell.bin, refs.json}
 * byte-identically; the tab opens the result for inspection and tuning (a
 * slider moved in the tab and saved is the new truth -- this script is the
 * record of how it was built, not a generator to re-run over edits).
 *
 *   node assets/dungeons/shardwell/build.mjs            build + validate (map-free)
 *   node assets/dungeons/shardwell/build.mjs --plan     also print sockets / waynodes (authoring aid)
 *
 * MAP-FREE. Its home placement (the default map's Shardspire crater) is made
 * and ground-checked by scripts/place_dungeon.mjs ("Portability" in
 * docs/PLAN_dungeons.md):
 *   node scripts/place_dungeon.mjs --map default --dungeon shardwell --at 67209,-49564 --yaw 270 --write
 * The adit's cut (cutLength / cutHeight / headwallLean / apronLength) is what
 * a different hillside adjusts (`--set p0.cutLength=6 --as <variant>`).
 *
 * THE STORY. The company drove an adit into the foot of a quartzite cliff
 * shot with crystal, found the crystal 18 m down and worked it out by
 * room-and-pillar until the deep level flooded and the north drift caved in.
 * Then the miners stopped coming up.
 *
 *   ADIT LEVEL (y 0)                          DEEP LEVEL (y -18 m)
 *   adit (boarded portal, spoil, rails out)   incline foot: the watchman (guard)
 *    -> main haulage drift (lanterns)          -> loading drift -> the crossing
 *    -> the hub crossing (patrol)                  W: the Glimmer stope (crystal,
 *        N: old drift -> the miners' camp             pillar grid, spawner)
 *           (foreman's lever, stores)              S: the drowned sump -> the
 *        S: the cave-in (sealed; loose roof          worked-out stope (assay chest)
 *           trap) -> the dark north drift          E: a drift to the winze foot
 *           -> the winze collar                       (the dead shift boss' key)
 *        W: the haulage incline down, barred
 *           by a portcullis the camp lever lifts
 *   The winze joins the two levels (ladder): climb it from the deep level to
 *   reach the far side of the cave-in.
 *
 * D8: the drifts follow the crystal vein (wander + a vein band in one rib),
 * their sets are irregular (racked, sagging, missing), the haulage drift
 * climbs a little to the hub, the stopes have bays and offset lobes, the
 * Glimmer a crystal key light, the camp its fire, and the lamps carry.
 * Slope is used only where it cannot break the winze loop (the two levels
 * must meet the winze's fixed depth): on the haulage drift (both levels move
 * with the hub) and the dead-end drift to the camp.
 *
 * FRAME. The dungeon frame is the adit's: the entrance socket at (0,0,0),
 * +Z into the cliff.
 */
import { G, U, MATS, writeSchedules, writeDungeon, report, adoptWaynodes, placedAll, openFloor, dropToFloor } from '../../../scripts/dungeon_build_lib.mjs';

const args = process.argv.slice(2);
const flag = (f) => args.includes(f);
const ID = 'shardwell';

// =============================================================================
// 1. the pieces
// =============================================================================
let d = G.newDungeon({name: 'The Shardwell', style: 'mines', seed: 81723});
// Quartzite shot with crystal: the crystal palette (walls and dressed rock
// quartzite, ore = emissive crystal, so the vein the drifts follow glows),
// timber that burns, gravel that falls.
d.palette = {variant: 'crystal'};

const T = (kit, type, preset, params) => ({kit, type, preset, params});
const at = (piece, socket, spec, extra) => {
  const r = G.attachPiece(d, {piece, socket}, Object.assign({}, spec, extra || {}));
  d = r.dungeon;
  return r.piece.id;
};
const sock = (pid, name) => {
  const pc = G.placePiece(d, d.pieces.find(p => p.id === pid));
  const s = pc.sockets.find(q => q.name === name);
  if (!s) throw new Error(pid + ' has no socket ' + name);
  return s;
};
// The lamps: big enough to light a drift (a lamp's light carries as far as its
// flame's glowing surface is big).
const LAMP = {lightType: 'lantern', lightPower: 0.7};

// --- the adit: a timber portal in the cliff foot, half boarded, spoil tipped
// beside the cut, the track running out onto the crater floor.
d.pieces.push(G.newPiece(d, {kit: 'mines', type: 'adit_entrance', preset: 'abandoned', params: Object.assign({
  width: 3.0, height: 2.7, length: 10, ceiling: 0.4,
  cutLength: 8, cutHeight: 7, cutFlare: 0.35, headwallLean: 0.6, cutRough: 0.8, apronLength: 0.6, portal: 'timber', headboard: true, boarded: 0.35,
  spoilHeap: 1, railsOut: true, rails: true, gauge: 0.6, sleeperSpacing: 0.7,
  setSpacing: 1.5, beam: 0.3, sag: 0.45, broken: 0.25, lagging: 0.5, overbreak: 0.5, overbreakScale: 1.2,
  wander: 0.9, wanderWave: 12, jitter: 0.6, heightVar: 0.12,
  decay: 0.45, moisture: 0.1, wallRough: 0.55, wallScale: 1.1, floorUneven: 0.35, asym: 0.6,
  lightCount: 1, warmth: 1, blend: 6}, LAMP)}));
d.entrance = {piece: 'p0', socket: 'surface'};

// --- main haulage drift: still lit, the best-kept timber in the mine; it
// bends along the vein and climbs a metre to the hub.
const p1 = at('p0', 'in', T('mines', 'drift', 'haulage', Object.assign({
  width: 3.0, height: 2.7, length: 23, ceiling: 0.5,
  setSpacing: 2.2, beam: 0.3, sag: 0.3, broken: 0.08, lagging: 0.7, lampSpacing: 4.4, overbreak: 0.6, overbreakScale: 2.0,
  rails: true, gauge: 0.6, sleeperSpacing: 0.65, niches: 0.35, nicheDepth: 1.4, spoil: 0.25,
  wander: 1.6, wanderWave: 15, slope: 1.0, jitter: 0.4, widthVar: 0.12, heightVar: 0.1, vein: 0.6,
  decay: 0.2, moisture: 0.1, deco: 0.7, wallRough: 0.45, wallScale: 1.4, floorUneven: 0.3, asym: 0.45,
  lightCount: 4, warmth: 0.9, blend: 6}, LAMP)));

// --- the hub: a timbered crossing, wider and taller than the drifts.
const p2 = at(p1, 'out', T('mines', 'junction', 'crossroads', Object.assign({
  shape: 'X', width: 3.0, height: 2.8, length: 3, ceiling: 1.2, hubPad: 1.6, cornerPosts: true,
  setSpacing: 1.8, beam: 0.35, sag: 0.2, broken: 0.1, rails: true, gauge: 0.6, overbreak: 1.0, overbreakScale: 2.4, jitter: 0.5,
  decay: 0.3, moisture: 0.15, wallRough: 0.5, wallScale: 1.8, asym: 0.5, lightCount: 2, blend: 8}, LAMP)));

// --- north: an older, narrower, crooked drift to the miners' camp (candles).
const p3 = at(p2, 'left', T('mines', 'drift', 'narrow_old', {
  width: 2.2, height: 2.2, length: 11, ceiling: 0.2,
  setSpacing: 1.1, beam: 0.2, sag: 0.65, broken: 0.3, lagging: 0.25, overbreak: 0.9, overbreakScale: 0.9, rails: false, niches: 0.5, nicheDepth: 1.0, spoil: 0.4,
  wander: 1.8, wanderWave: 9, slope: -0.6, jitter: 0.9, widthVar: 0.2, heightVar: 0.15,
  decay: 0.55, moisture: 0.35, deco: 0.6, wallRough: 0.7, wallScale: 0.9, floorUneven: 0.45, asym: 0.7,
  lightCount: 1, lightType: 'candles', lightPower: 0.6, warmth: 1, blend: 6}));
const p4 = at(p3, 'out', T('mines', 'camp', 'bunkroom', {
  width: 6.5, length: 8.5, height: 2.6, ceiling: 0.6,
  bunks: 4, table: true, crates: 5, stove: true, tools: 2, exits: 'dead end',
  setSpacing: 2.4, beam: 0.3, sag: 0.35, broken: 0.15, overbreak: 0.4, overbreakScale: 1.5,
  jitter: 0.6, bays: 1, bayDepth: 1.4, lobe: 0.3,
  decay: 0.35, moisture: 0.1, deco: 0.8, wallRough: 0.5, wallScale: 1.6, asym: 0.5,
  lightCount: 2, lightType: 'candles', lightPower: 0.7, warmth: 1, blend: 6}));

// --- south: the cave-in that sealed the north drift, then the dark drift
// beyond it, turning west to the winze.
const p5 = at(p2, 'right', T('mines', 'collapse', 'sealed', {
  width: 3.0, height: 2.7, length: 13, ceiling: 0.3,
  blockage: 1, coneLength: 9, boulders: 5, cavity: 3.2, looseCeiling: true,
  setSpacing: 1.7, beam: 0.3, sag: 0.8, broken: 0.6, lagging: 0.2, overbreak: 1.4, overbreakScale: 1.1,
  wander: 0.8, jitter: 1, heightVar: 0.2,
  decay: 0.9, moisture: 0.3, wallRough: 0.6, wallScale: 1.3, asym: 0.7, lightCount: 0, lightType: 'none', blend: 6}));
const p6 = at(p5, 'out', T('mines', 'junction', 'bend_old', {
  shape: 'L', side: 'left', width: 2.6, height: 2.4, length: 2.5, ceiling: 0.4, hubPad: 0.4, cornerPosts: false,
  setSpacing: 1.4, beam: 0.25, sag: 0.7, broken: 0.55, rails: false, overbreak: 0.8, overbreakScale: 1.0, jitter: 0.9,
  decay: 0.8, moisture: 0.45, wallRough: 0.65, wallScale: 1.1, asym: 0.7, lightCount: 0, lightType: 'none', blend: 6}));

// --- west: the haulage incline down to the deep level (rails, steep).
const p7 = at(p2, 'out', T('mines', 'incline', 'haulage_up', Object.assign({
  direction: 'down', mode: 'rails', rise: 18, length: 36.2, width: 3.0, height: 2.8,
  setSpacing: 2.6, beam: 0.3, sag: 0.25, broken: 0.12, lagging: 0.4, handrail: true, overbreak: 0.7, overbreakScale: 1.8, jitter: 0.6,
  decay: 0.3, moisture: 0.25, wallRough: 0.45, wallScale: 1.5, floorUneven: 0.1, asym: 0.4,
  lightCount: 3, warmth: 0.7, blend: 6}, LAMP)));

// --- the deep level.
const p8 = at(p7, 'out', T('mines', 'drift', 'standard', Object.assign({
  width: 3.0, height: 2.7, length: 6, ceiling: 0.6,
  setSpacing: 1.9, beam: 0.3, sag: 0.3, broken: 0.15, lagging: 0.5, lampSpacing: 3.8, overbreak: 0.8, overbreakScale: 1.4,
  rails: true, gauge: 0.6, sleeperSpacing: 0.6, niches: 0, spoil: 0.35, jitter: 0.5, vein: 0.5,
  decay: 0.35, moisture: 0.4, deco: 0.5, wallRough: 0.5, wallScale: 1.2, floorUneven: 0.3, asym: 0.5,
  lightCount: 1, warmth: 0.6, blend: 6}, LAMP)));
const p9 = at(p8, 'out', T('mines', 'junction', 'crossroads', Object.assign({
  shape: 'X', width: 3.0, height: 2.8, length: 2.6, ceiling: 1.8, hubPad: 2.2, cornerPosts: true,
  setSpacing: 2.0, beam: 0.35, sag: 0.4, broken: 0.25, rails: true, gauge: 0.6, overbreak: 1.3, overbreakScale: 2.6, jitter: 0.7,
  decay: 0.45, moisture: 0.45, wallRough: 0.6, wallScale: 2.2, asym: 0.6, lightCount: 1, warmth: 0.5, blend: 10}, LAMP)));

// The Glimmer: the crystal stope, room-and-pillar, scaffolds still up, its
// pillars left as the crystal allowed, a worked-out pocket off one side, lit
// by a cluster of crystal the miners never cut.
const p10 = at(p9, 'left', T('mines', 'stope', 'crystal_gallery', Object.assign({
  width: 24, length: 26, height: 6.5, ceiling: 3.2,
  oreType: 'palette', oreDensity: 0.3, pillarSpacing: 7.5, pillarSize: 2.6, pillarRough: 2.6, robbed: 0.35,
  benches: 1, scaffolds: 3, scaffoldHeight: 3.2, exits: 'dead end',
  jitter: 0.8, bays: 3, bayDepth: 3.5, lobe: 0.55,
  keyLight: 'crystals', keySize: 0.85, keyAt: 0.62, keySide: 0.35,
  decay: 0.35, moisture: 0.3, deco: 0.7, wallRough: 0.85, wallScale: 2.6, floorUneven: 0.5, asym: 0.8,
  lightCount: 3, lightDensity: 0.3, warmth: 0.6, blend: 12}, LAMP)));

// The drowned sump, then the stope it drowned.
const p11 = at(p9, 'out', T('mines', 'flooded_sump', 'sump', {
  width: 9, length: 12, height: 3.4, ceiling: 1.4,
  sumpDepth: 3.2, waterLevel: 0.9, sills: false, ledge: 1.6, pumpGear: true,
  bays: 1, bayDepth: 2,
  decay: 0.5, moisture: 0.95, deco: 0.6, wallRough: 0.75, wallScale: 1.9, floorUneven: 0.4, asym: 0.7,
  lightCount: 1, lightType: 'lantern', lightPower: 0.5, warmth: 0, blend: 8}));
const p12 = at(p11, 'out', T('mines', 'stope', 'worked_out', {
  width: 15, length: 17, height: 4.2, ceiling: 1.6,
  oreType: 'palette', oreDensity: 0.12, pillarSpacing: 5.2, pillarSize: 1.7, pillarRough: 2.0, robbed: 0.75,
  benches: 2, scaffolds: 1, scaffoldHeight: 2.2, exits: 'dead end',
  jitter: 1, bays: 2, bayDepth: 2.5, lobe: 0.4,
  decay: 0.8, moisture: 0.6, deco: 0.35, wallRough: 0.8, wallScale: 1.7, floorUneven: 0.6, asym: 0.8,
  lightCount: 0, lightType: 'none', blend: 10}));

// ---- the winze: closes the loop between the two levels -----------------------
// The collar hangs off a drift from p6's arm (the dark north drift); the foot
// opens west toward the deep crossing's east arm (p9.right). Two lengths make
// it meet: the north drift's (so the shaft's axis lands on the deep crossing's
// line) and the deep east drift's (so the foot meets its end). Neither drift
// slopes (the winze's depth is fixed); both wander (the sockets stay on axis).
const WZ = {width: 2.6, length: 18, height: 2.5, stationDepth: 2.4, collarStation: true, headroom: 1.4, windlass: true,
            footSide: '-x', landings: 0, ladder: true, cribSpacing: 1.7, cage: false, sump: 0,
            decay: 0.6, moisture: 0.55, wallRough: 0.35, wallScale: 0.8, asym: 0.5, lightCount: 0, lightType: 'none', blend: 4};
const wzLocal = G.getType('mines', 'shaft').generate(WZ, 1);
const wzFoot = wzLocal.sockets.find(s => s.name === 'foot');            // local, collar at the origin facing -z
const arm6 = sock(p6, 'left'), east9 = sock(p9, 'right');
if (arm6.dir[2] !== 1 || east9.dir[0] !== 1) throw new Error('the loop assumes p6.left faces +z and p9.right faces +x');
const northLen = (east9.pos[2] - wzFoot.pos[2]) - arm6.pos[2];
const p13 = at(p6, 'left', T('mines', 'drift', 'abandoned', {
  width: 2.6, height: 2.4, length: northLen / 10, ceiling: 0.3,
  setSpacing: 1.5, beam: 0.25, sag: 0.85, broken: 0.55, lagging: 0.15, overbreak: 1.1, overbreakScale: 1.2, rails: false, gauge: 0.6, sleeperSpacing: 1.2,
  niches: 0.3, nicheDepth: 1.1, spoil: 0.75, wander: 1.4, wanderWave: 13, jitter: 0.7, heightVar: 0.2, vein: 0.4,
  decay: 0.8, moisture: 0.5, deco: 0.4, wallRough: 0.7, wallScale: 1.0, floorUneven: 0.5, asym: 0.8,
  lightCount: 0, lightType: 'none', blend: 6}));
const p14 = at(p13, 'out', T('mines', 'shaft', 'level_winze', WZ));
const foot = sock(p14, 'foot');
if (foot.pos[1] !== east9.pos[1]) throw new Error('the winze foot (' + foot.pos[1] + ') is not on the deep level (' + east9.pos[1] + ')');
const eastLen = foot.pos[0] - east9.pos[0];
const p15 = at(p9, 'right', T('mines', 'drift', 'narrow_old', {
  width: 2.4, height: 2.4, length: eastLen / 10, ceiling: 0.4,
  setSpacing: 1.6, beam: 0.25, sag: 0.55, broken: 0.35, lagging: 0.3, overbreak: 0.9, overbreakScale: 0.8, rails: false, niches: 0.25, nicheDepth: 1.3, spoil: 0.5,
  wander: 1.1, wanderWave: 10, jitter: 0.8, widthVar: 0.2,
  decay: 0.6, moisture: 0.6, deco: 0.5, wallRough: 0.6, wallScale: 1.4, floorUneven: 0.4, asym: 0.6,
  lightCount: 0, lightType: 'none', blend: 6}));
{
  const end = sock(p15, 'out');
  if (end.pos.join() !== foot.pos.join()) throw new Error('the winze does not close: ' + end.pos + ' vs foot ' + foot.pos);
  d.pieces.find(p => p.id === p15).links.out = {piece: p14, socket: 'foot'};
  d.pieces.find(p => p.id === p14).links = Object.assign(d.pieces.find(p => p.id === p14).links || {}, {foot: {piece: p15, socket: 'out'}});
}

// =============================================================================
// 2. props + entities. The adit sits at the origin with its entrance socket
//    at (0,0,0), so the dungeon frame IS the anchor frame refs.json uses.
// =============================================================================
const probe = G.compile(d, {id: ID, materials: MATS});
const WN = new Map(probe.waynodes.map(w => [w.name, w]));
const wp = (name) => { const w = WN.get(name); if (!w) throw new Error('no waynode ' + name); return w.pos.slice(); };
const S = (pid, name) => { const s = probe.sockets.find(q => q.piece === pid && q.name === name); if (!s) throw new Error('no socket ' + pid + '.' + name); return s; };
const PC = (pid) => d.pieces.find(p => p.id === pid);
const placed = placedAll(d);
const placedById = new Map(placed.map(p => [p.id, p]));
/** A piece-local cell -> the dungeon frame (3.4.1 cell rotation). */
const L = (pid, v) => U.xformCell(U.quarter(PC(pid).yaw), PC(pid).pos, v);
const Lyaw = (pid, y) => ((y + PC(pid).yaw) % 360 + 360) % 360;
const sugg = (pid, kind) => probe.refs.filter(r => r.piece === pid && r.kind === kind);
/** On the floor under `pos`, then the nearest open cell. */
const need = (what, pos, h) => openFloor(placed, what, dropToFloor(placed, pos), h, 24, flag('--plan'));

// ---- authored props: the story the pieces do not tell by themselves ----------
d.props = [];
const prop = (piece, id, propId, params, pos, yaw, seed) => d.props.push({id, prop: propId, params: params || {}, seed: seed || 0, pos: pos.map(Math.round), yaw: ((yaw || 0) % 360 + 360) % 360, piece});
// A cart of waste left on the track inside the cut, as if the last shift
// pushed it out and walked away.
prop('p0', 'x_cut_cart', 'mines/ore_cart', {load: 'rubble'}, need('cut cart', [0, 0, 32], 9), 0, 11);
// The hub: a cart thrown over by the fall that sealed the north drift.
const hub = wp(p2 + '/hub');
prop(p2, 'x_wreck', 'mines/ore_cart', {tipped: true, load: 'rubble'}, need('wreck', [hub[0] + 44, hub[1], hub[2] + 14], 8), 90, 12);
// The camp: the lever that lifts the deep gate, by the back wall.
const leverPos = need('lever', L(p4, [-PC(p4).params.width * 5 + 6, 0, 70]), 18);
prop(p4, 'x_lever', 'mines/lever', {height: 1.3, pulled: false, post: true}, leverPos, Lyaw(p4, 90), 13);
// The incline foot: a cart of crystal that never went up.
const inFoot = S(p7, 'out');
prop(p8, 'x_crystal_cart', 'mines/ore_cart', {load: 'crystal'}, need('crystal cart', [inFoot.pos[0], inFoot.pos[1], inFoot.pos[2] + 22], 9), 0, 14);
// The Glimmer: crystal clusters grown out of the floor between the pillars.
const glim = (lx, lz, k, sz) => prop(p10, 'x_glim' + k, 'caves/crystal_cluster', {size: sz}, need('glimmer crystal ' + k, L(p10, [lx, 20, lz]), 6), Lyaw(p10, (k * 90) % 360), 20 + k);
glim(-80, 40, 0, 1.4); glim(85, 150, 1, 1.0); glim(-30, 230, 2, 1.8); glim(60, 60, 3, 0.8);
// The winze foot: what is left of the shift boss's mate, and his key.
const footSock = S(p14, 'foot');
const bonesPos = need('bones', [footSock.pos[0] + 12, footSock.pos[1], footSock.pos[2] + 8], 4);
prop(p14, 'x_bones', 'ruins/bones', {}, bonesPos, 90, 31);
// The worked-out stope: the assay chest, the one thing they meant to come back for.
const st12 = PC(p12).params;
const assayPos = need('assay chest', L(p12, [-Math.round(st12.width * 5) + 14, 10, 10 + Math.round(st12.length * 10 * 0.18)]), 12);
prop(p12, 'x_assay', 'mines/crate', {size: 1.1, stack: 1}, assayPos, Lyaw(p12, 90), 32);
// The drowned sump: a body on the ledge.
prop(p11, 'x_sump_bones', 'ruins/bones', {}, need('sump bones', L(p11, [-Math.round(PC(p11).params.width * 5) + 6, 0, 60]), 3), Lyaw(p11, 0), 33);

// =============================================================================
// 3. refs
// =============================================================================
const prog = G.compile(d, {id: ID, materials: MATS});
const refs = [];
const ref = (name, kind, pos, yaw, props, base) => { const r = {name, kind, pos: pos.map(Math.round), yaw: yaw || 0, props: props || {}}; if (base) r.base = base; refs.push(r); return r; };
// Every generated waynode becomes a waynode ref (the tab's "Adopt waynodes"):
// the patrol route names them.
adoptWaynodes(prog, ref);

// ---- the sign over the portal -------------------------------------------------
{ const s = sugg('p0', 'marker').find(r => (r.props.tags || []).includes('sign'));
  if (s) ref('portal_sign', 'marker', s.pos, 180, {tags: ['sign'], text: 'SHARDWELL - NO LIGHTS PAST THE CROSSING'}); }

// ---- the portcullis at the head of the incline, lifted by the camp lever ------
{
  const head = S(p7, 'in');                                   // faces back up to the hub (-z)
  const pw = PC(p7).params, W = Math.round(pw.width * 10), H = Math.round(pw.height * 10);
  // pos = a bottom END cell of the bars; yaw 180 = facing the hub, so RIGHT is
  // -x and the bars run from +W/2-1 down to -W/2, two cells into the incline.
  // Taller than the opening: the top rows sit in the rock (never built), so
  // the bars hang from the roof.
  ref('incline_gate', 'gate', [head.pos[0] + Math.round(W / 2) - 1, head.pos[1], head.pos[2] + 2], 180,
      {width: W, height: H + 6, thickness: 1, material: 'iron', pattern: 'bars', spacing: 3, speed: 2, on: 'deep_gate'});
  ref('camp_lever', 'lever', leverPos, Lyaw(p4, 90), {signal: 'deep_gate', startOn: false, label: 'Throw the lever (the deep-level gate)'});
}

// ---- the camp door: the compiler's suggestion (hinge cell + width = the opening) ----
{
  const s = prog.refs.find(r => r.kind === 'door' && r.piece === p4);
  ref('camp_door', 'door', s.pos, s.yaw, {width: s.props.width - 2, height: 21, thickness: 1, hinge: 'left'});
}

// ---- the cave-in: a loose roof that comes down on whoever digs at the toe ------
{
  const t = sugg(p5, 'trap')[0];
  const R = t.props.region;
  ref('roof_slab', 'region', R.min, 0, {size: [0, 1, 2].map(i => R.max[i] - R.min[i])});
  const inS = S(p5, 'in');                                     // the hub side of the collapse (faces -x)
  ref('roof_plate', 'plate', [inS.pos[0] + 2, inS.pos[1], inS.pos[2] - 7], 0, {size: [Math.max(6, R.min[0] - inS.pos[0] - 4), 3, 15], latch: false, who: 'player'});
  ref('roof_fall', 'trap', R.min, 0, {type: 'collapse_floor', on: 'roof_plate', region: 'roof_slab', once: true});
}

// ---- containers ----------------------------------------------------------------------
{
  const nc = sugg(p1, 'container')[0];
  if (nc) ref('haulage_niche', 'container', nc.pos, 0, {loot: 'mine_supplies', title: 'crate'});
  const campCrate = placedById.get(p4).props.find(pr => pr.prop === 'mines/crate');
  ref('camp_stores', 'container', campCrate.pos, campCrate.yaw, {loot: 'mine_supplies', title: 'miners\' stores'});
  const dc = sugg(p13, 'container')[0];
  if (dc) ref('north_drift_cache', 'container', dc.pos, 0, {loot: 'mine_supplies', title: 'crate'});
  ref('assay_chest', 'container', assayPos, Lyaw(p12, 90), {loot: 'cave_cache', title: 'assay chest', lock: 'iron_key'});
  ref('shift_key', 'key', bonesPos, 0, {item: 'iron_key'});
}

// ---- creatures --------------------------------------------------------------------------
{
  // The shift boss walks his round on the adit level: the haulage drift, the
  // crossing and the north drift to the camp door, back and forth.
  ref('shift_boss', 'npc', wp(p2 + '/hub'), 0, {name: 'the shift boss', behavior: 'zombie', schedule: 'shardwell_shift',
      outfit: ['smock#5a4b36', 'breeches#3b3127', 'iron_helm']}, 'zombie');
  // The watchman, an old boiler automaton, still guards the incline foot.
  const post = wp(p8 + '/w0');
  ref('watch_post', 'marker', post, 180, {});
  ref('watchman', 'npc', post, 180, {name: 'the watchman', behavior: 'guardian', schedule: 'shardwell_watch', weapon: 'mace'}, 'boilerman');
  // The Glimmer's dead, asleep among the pillars until someone walks in.
  const heart = wp(p10 + '/c');
  ref('glimmer_heart', 'marker', heart, 0, {});
  const stIn = S(p10, 'in');                                   // the stope's door (faces +x, toward the crossing)
  ref('glimmer', 'region', [stIn.pos[0] - 80, stIn.pos[1], stIn.pos[2] - 50], 0, {size: [70, 40, 100]});
  ref('glimmer_dead', 'spawner', heart, 0, {mobs: [{base: 'zombie', weight: 3}, {base: 'tinker', weight: 1}], count: 3, radius: 3,
      respawn: 'onLeave', behavior: 'zombie', schedule: 'shardwell_lair', trigger: 'onEnter(glimmer)', outfit: ['smock#4e5560', 'breeches#33302b']});
}

// ---- the creatures' schedules (assets/schedules/, shared files) ---------------------------
const SCHED = {
  shardwell_shift: {about: 'The Shardwell (dungeon): the dead shift boss walking his round on the adit level, back and forth.',
    rows: [{from: '00:00', to: '24:00', do: 'patrol', route: ['wn_p1_w1', 'wn_p1_w4', 'wn_p2_hub', 'wn_p2_l0', 'wn_p3_w1', 'wn_p3_w2'], mode: 'pingpong', wait: 4}]},
  shardwell_watch: {about: 'The Shardwell (dungeon): the watchman automaton at the foot of the haulage incline.',
    rows: [{from: '00:00', to: '24:00', do: 'guard', at: 'watch_post'}]},
  shardwell_lair: {about: 'The Shardwell (dungeon): the Glimmer\'s dead, asleep in the crystal stope until someone comes close.',
    rows: [{from: '00:00', to: '24:00', do: 'lair', at: 'glimmer_heart', radius: 7}]}
};
for (const r of SCHED.shardwell_shift.rows[0].route) if (!refs.some(x => x.name === r)) throw new Error('patrol route names ' + r + ', not a waynode ref');
writeSchedules(SCHED);
if (flag('--plan')) {
  for (const pc of prog.pieces) console.log(pc.id.padEnd(4), (pc.kit + '/' + pc.type).padEnd(22), 'bounds', JSON.stringify(pc.bounds.min), JSON.stringify(pc.bounds.max), pc.prims + ' prims');
  for (const s of prog.sockets) console.log('  sock', s.piece, s.name.padEnd(7), JSON.stringify(s.pos), JSON.stringify(s.dir), s.w + 'x' + s.h, s.linked ? 'linked' : 'OPEN');
  if (flag('--way')) for (const w of prog.waynodes) console.log('  way', w.name.padEnd(14), JSON.stringify(w.pos), w.links.join(' '));
  for (const r of prog.refs) console.log('  sugg', r.id.padEnd(18), r.kind.padEnd(9), JSON.stringify(r.pos), JSON.stringify(r.props || {}));
}

// =============================================================================
// 4. write + validate (map-free)
// =============================================================================
const out = writeDungeon(ID, d, refs, 'The Shardwell, the abandoned crystal mine (assets/dungeons/shardwell/build.mjs). ' +
  'Lever camp_lever (miners\' camp) lifts incline_gate (head of the haulage incline). The winze links the deep level to the far side of the cave-in.');
process.exit(report(ID, d, refs, out) ? 1 : 0);
