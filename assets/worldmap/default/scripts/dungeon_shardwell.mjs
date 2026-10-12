/* dungeon_shardwell.mjs — THE SHARDWELL, the example mine dungeon (package D6,
 * docs/PLAN_dungeons.md §7 wave 3), authored through the Dungeons tab's own
 * module API: dungeongen.newPiece / attachPiece / snapTransform / compile /
 * validate and ui_entities' refs.json format. Re-running it rewrites
 * assets/dungeons/shardwell/{dungeon.json, shell.json, shell.bin, refs.json}
 * byte-identically; the tab opens the result for inspection and tuning (a
 * slider moved in the tab and saved is the new truth -- this script is the
 * record of how it was first built, not a generator to re-run over edits).
 *
 *   node assets/worldmap/default/scripts/dungeon_shardwell.mjs            build + validate (ground from the cached heightmap)
 *   node assets/worldmap/default/scripts/dungeon_shardwell.mjs --ground   refresh the heightmap first (sandvox --heightmap, GPU-free)
 *   node assets/worldmap/default/scripts/dungeon_shardwell.mjs --plan     also print sockets / waynodes (authoring aid)
 *
 * THE STORY. The Shardwell is the crater on the Shardspire's caprock
 * (docs/WORLD_tirnhallow.md). Its west wall is a 100 m cliff of lilac
 * quartzite shot with crystal; the company drove an adit into the foot of it,
 * found the crystal 18 m down and worked it out by room-and-pillar until the
 * deep level flooded and the north drift caved in. Then the miners stopped
 * coming up.
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
 * FRAME. The dungeon frame is the adit's: the entrance socket at (0,0,0),
 * +Z into the cliff. The map places it with yaw 270 (local +Z = world -X,
 * local +X = world +Z) at the cliff foot, [67209, -49564].
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
import { spawnSync } from 'child_process';
import { dirname, join } from 'path';
import { fileURLToPath, pathToFileURL } from 'url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ASSETS = join(HERE, '..', '..', '..');
const ROOT = join(ASSETS, '..');
const G = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/dungeongen.js')).href);
const E = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/ui_entities.js')).href);
const U = await import(pathToFileURL(join(ASSETS, 'editor/dungeon/dgutil.js')).href);
const args = process.argv.slice(2);
const flag = (f) => args.includes(f);

const ID = 'shardwell';
const PLACE = {at: [67209, -49564], yOffset: 0, yaw: 270};

// =============================================================================
// 1. the pieces
// =============================================================================
let d = G.newDungeon({name: 'The Shardwell', style: 'mines', seed: 81723});
// The Shardspire is quartzite shot with crystal: the crystal palette (walls
// and dressed rock quartzite, ore = emissive crystal), timber that burns,
// gravel that falls.
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
const setLen = (pid, key, metres) => { const p = d.pieces.find(q => q.id === pid); p.params[key] = metres; };

// --- the adit: a timber portal in the cliff foot, half boarded, spoil tipped
// beside the cut, the track running out onto the crater floor.
d.pieces.push(G.newPiece(d, {kit: 'mines', type: 'adit_entrance', preset: 'abandoned', params: {
  width: 3.0, height: 2.7, length: 10, ceiling: 0.4,
  cutLength: 8, cutHeight: 7, cutFlare: 0.35, portal: 'timber', headboard: true, boarded: 0.35,
  spoilHeap: 1, railsOut: true, rails: true, gauge: 0.6, sleeperSpacing: 0.7,
  setSpacing: 1.5, beam: 0.3, sag: 0.45, broken: 0.25, lagging: 0.5,
  decay: 0.45, moisture: 0.1, wallRough: 0.55, wallScale: 1.1, floorUneven: 0.35, asym: 0.6,
  lightCount: 1, lightType: 'lantern', warmth: 1, blend: 6}}));
d.entrance = {piece: 'p0', socket: 'surface'};

// --- main haulage drift: still lit, the best-kept timber in the mine.
const p1 = at('p0', 'in', T('mines', 'drift', 'haulage', {
  width: 3.0, height: 2.7, length: 23, ceiling: 0.5,
  setSpacing: 2.2, beam: 0.3, sag: 0.3, broken: 0.08, lagging: 0.7, lampSpacing: 4.4,
  rails: true, gauge: 0.6, sleeperSpacing: 0.65, niches: 0.35, nicheDepth: 1.4, spoil: 0.25,
  decay: 0.2, moisture: 0.1, deco: 0.7, wallRough: 0.45, wallScale: 1.4, floorUneven: 0.3, asym: 0.45,
  lightCount: 4, lightType: 'lantern', warmth: 0.9, blend: 6}));

// --- the hub: a timbered crossing, wider and taller than the drifts.
const p2 = at(p1, 'out', T('mines', 'junction', 'crossroads', {
  shape: 'X', width: 3.0, height: 2.8, length: 3, ceiling: 1.2, hubPad: 1.6, cornerPosts: true,
  setSpacing: 1.8, beam: 0.35, sag: 0.2, broken: 0.1, rails: true, gauge: 0.6,
  decay: 0.3, moisture: 0.15, wallRough: 0.5, wallScale: 1.8, asym: 0.5, lightCount: 2, lightType: 'lantern', blend: 8}));

// --- north: an older, narrower drift to the miners' camp (candles).
const p3 = at(p2, 'left', T('mines', 'drift', 'narrow_old', {
  width: 2.2, height: 2.2, length: 11, ceiling: 0.2,
  setSpacing: 1.1, beam: 0.2, sag: 0.65, broken: 0.3, lagging: 0.25, rails: false, niches: 0.5, nicheDepth: 1.0, spoil: 0.4,
  decay: 0.55, moisture: 0.35, deco: 0.6, wallRough: 0.7, wallScale: 0.9, floorUneven: 0.45, asym: 0.7,
  lightCount: 1, lightType: 'candles', warmth: 1, blend: 6}));
const p4 = at(p3, 'out', T('mines', 'camp', 'bunkroom', {
  width: 6.5, length: 8.5, height: 2.6, ceiling: 0.6,
  bunks: 4, table: true, crates: 5, stove: true, tools: 2, exits: 'dead end',
  setSpacing: 2.4, beam: 0.3, sag: 0.35, broken: 0.15,
  decay: 0.35, moisture: 0.1, deco: 0.8, wallRough: 0.5, wallScale: 1.6, asym: 0.5,
  lightCount: 2, lightType: 'candles', warmth: 1, blend: 6}));

// --- south: the cave-in that sealed the north drift, then the dark drift
// beyond it, turning west to the winze.
const p5 = at(p2, 'right', T('mines', 'collapse', 'sealed', {
  width: 3.0, height: 2.7, length: 13, ceiling: 0.3,
  blockage: 1, coneLength: 9, boulders: 5, cavity: 3.2, looseCeiling: true,
  setSpacing: 1.7, beam: 0.3, sag: 0.8, broken: 0.6, lagging: 0.2,
  decay: 0.9, moisture: 0.3, wallRough: 0.6, wallScale: 1.3, asym: 0.7, lightCount: 0, lightType: 'none', blend: 6}));
const p6 = at(p5, 'out', T('mines', 'junction', 'bend_old', {
  shape: 'L', side: 'left', width: 2.6, height: 2.4, length: 2.5, ceiling: 0.4, hubPad: 0.4, cornerPosts: false,
  setSpacing: 1.4, beam: 0.25, sag: 0.7, broken: 0.55, rails: false,
  decay: 0.8, moisture: 0.45, wallRough: 0.65, wallScale: 1.1, asym: 0.7, lightCount: 0, lightType: 'none', blend: 6}));

// --- west: the haulage incline down to the deep level (rails, steep).
const p7 = at(p2, 'out', T('mines', 'incline', 'haulage_up', {
  direction: 'down', mode: 'rails', rise: 18, length: 36.2, width: 3.0, height: 2.8,
  setSpacing: 2.6, beam: 0.3, sag: 0.25, broken: 0.12, lagging: 0.4, handrail: true,
  decay: 0.3, moisture: 0.25, wallRough: 0.45, wallScale: 1.5, floorUneven: 0.1, asym: 0.4,
  lightCount: 2, lightType: 'lantern', warmth: 0.7, blend: 6}));

// --- the deep level.
const p8 = at(p7, 'out', T('mines', 'drift', 'standard', {
  width: 3.0, height: 2.7, length: 6, ceiling: 0.6,
  setSpacing: 1.9, beam: 0.3, sag: 0.3, broken: 0.15, lagging: 0.5, lampSpacing: 3.8,
  rails: true, gauge: 0.6, sleeperSpacing: 0.6, niches: 0, spoil: 0.35,
  decay: 0.35, moisture: 0.4, deco: 0.5, wallRough: 0.5, wallScale: 1.2, floorUneven: 0.3, asym: 0.5,
  lightCount: 1, lightType: 'lantern', warmth: 0.6, blend: 6}));
const p9 = at(p8, 'out', T('mines', 'junction', 'crossroads', {
  shape: 'X', width: 3.0, height: 2.8, length: 2.6, ceiling: 1.8, hubPad: 2.2, cornerPosts: true,
  setSpacing: 2.0, beam: 0.35, sag: 0.4, broken: 0.25, rails: true, gauge: 0.6,
  decay: 0.45, moisture: 0.45, wallRough: 0.6, wallScale: 2.2, asym: 0.6, lightCount: 1, lightType: 'lantern', warmth: 0.5, blend: 10}));

// The Glimmer: the crystal stope, room-and-pillar, scaffolds still up.
const p10 = at(p9, 'left', T('mines', 'stope', 'crystal_gallery', {
  width: 24, length: 26, height: 6.5, ceiling: 3.2,
  oreType: 'palette', oreDensity: 0.75, pillarSpacing: 7.5, pillarSize: 2.4, robbed: 0.35,
  benches: 1, scaffolds: 3, scaffoldHeight: 3.2, exits: 'dead end',
  decay: 0.35, moisture: 0.3, deco: 0.7, wallRough: 0.85, wallScale: 2.6, floorUneven: 0.5, asym: 0.8,
  lightCount: 3, lightType: 'crystal', warmth: 0.3, blend: 12}));

// The drowned sump, then the stope it drowned.
const p11 = at(p9, 'out', T('mines', 'flooded_sump', 'sump', {
  width: 9, length: 12, height: 3.4, ceiling: 1.4,
  sumpDepth: 3.2, waterLevel: 0.9, sills: false, ledge: 1.6, pumpGear: true,
  decay: 0.5, moisture: 0.95, deco: 0.6, wallRough: 0.75, wallScale: 1.9, floorUneven: 0.4, asym: 0.7,
  lightCount: 1, lightType: 'crystal', warmth: 0, blend: 8}));
const p12 = at(p11, 'out', T('mines', 'stope', 'worked_out', {
  width: 15, length: 17, height: 4.2, ceiling: 1.6,
  oreType: 'palette', oreDensity: 0.12, pillarSpacing: 5.2, pillarSize: 1.5, robbed: 0.75,
  benches: 2, scaffolds: 1, scaffoldHeight: 2.2, exits: 'dead end',
  decay: 0.8, moisture: 0.6, deco: 0.35, wallRough: 0.8, wallScale: 1.7, floorUneven: 0.6, asym: 0.8,
  lightCount: 0, lightType: 'none', blend: 10}));

// ---- the winze: closes the loop between the two levels -----------------------
// The collar hangs off a drift from p6's arm (the dark north drift); the foot
// opens west toward the deep crossing's east arm (p9.right). Two lengths make
// it meet: the north drift's (so the shaft's axis lands on the deep crossing's
// line) and the deep east drift's (so the foot meets its end).
const WZ = {width: 2.6, length: 18, height: 2.5, stationDepth: 2.4, collarStation: true, headroom: 1.4, windlass: true,
            footSide: '-x', landings: 0, ladder: true, cribSpacing: 1.7, cage: false, sump: 0,
            decay: 0.6, moisture: 0.55, wallRough: 0.35, wallScale: 0.8, asym: 0.5, lightCount: 0, lightType: 'none', blend: 4};
const wzLocal = G.getType('mines', 'shaft').generate(WZ, 1);
const wzFoot = wzLocal.sockets.find(s => s.name === 'foot');            // local, collar at the origin facing -z
const arm6 = sock(p6, 'left'), east9 = sock(p9, 'right');
if (arm6.dir[2] !== 1 || east9.dir[0] !== 1) throw new Error('the loop assumes p6.left faces +z and p9.right faces +x');
// collar at z = c, foot at (x0 + wzFoot.x, z = c + wzFoot.z): want z = east9.z.
const northLen = (east9.pos[2] - wzFoot.pos[2]) - arm6.pos[2];
const p13 = at(p6, 'left', T('mines', 'drift', 'abandoned', {
  width: 2.6, height: 2.4, length: northLen / 10, ceiling: 0.3,
  setSpacing: 1.3, beam: 0.25, sag: 0.85, broken: 0.55, lagging: 0.15, rails: true, gauge: 0.6, sleeperSpacing: 0.8,
  niches: 0.3, nicheDepth: 1.1, spoil: 0.75,
  decay: 0.8, moisture: 0.5, deco: 0.4, wallRough: 0.7, wallScale: 1.0, floorUneven: 0.5, asym: 0.8,
  lightCount: 0, lightType: 'none', blend: 6}));
const p14 = at(p13, 'out', T('mines', 'shaft', 'level_winze', WZ));
const foot = sock(p14, 'foot');
const eastLen = foot.pos[0] - east9.pos[0];
const p15 = at(p9, 'right', T('mines', 'drift', 'narrow_old', {
  width: 2.4, height: 2.4, length: eastLen / 10, ceiling: 0.4,
  setSpacing: 1.6, beam: 0.25, sag: 0.55, broken: 0.35, lagging: 0.3, rails: false, niches: 0.25, nicheDepth: 1.3, spoil: 0.5,
  decay: 0.6, moisture: 0.6, deco: 0.5, wallRough: 0.6, wallScale: 1.4, floorUneven: 0.4, asym: 0.6,
  lightCount: 0, lightType: 'none', blend: 6}));
{
  const end = sock(p15, 'out');
  if (end.pos.join() !== foot.pos.join()) throw new Error('the winze does not close: ' + end.pos + ' vs foot ' + foot.pos);
  d.pieces.find(p => p.id === p15).links.out = {piece: p14, socket: 'foot'};
  d.pieces.find(p => p.id === p14).links = Object.assign(d.pieces.find(p => p.id === p14).links || {}, {foot: {piece: p15, socket: 'out'}});
}
void setLen;


// =============================================================================
// 2. props + entities. The adit sits at the origin with its entrance socket
//    at (0,0,0), so the dungeon frame IS the anchor frame refs.json uses.
// =============================================================================
const MATS = JSON.parse(readFileSync(join(ASSETS, 'materials/materials.json'), 'utf8')).materials.map(m => m.id);
const probe = G.compile(d, {id: ID, materials: MATS});
const WN = new Map(probe.waynodes.map(w => [w.name, w]));
const wp = (name) => { const w = WN.get(name); if (!w) throw new Error('no waynode ' + name); return w.pos.slice(); };
const S = (pid, name) => { const s = probe.sockets.find(q => q.piece === pid && q.name === name); if (!s) throw new Error('no socket ' + pid + '.' + name); return s; };
const PC = (pid) => d.pieces.find(p => p.id === pid);
const placed = new Map(d.pieces.map(p => [p.id, G.placePiece(d, p)]));
/** A piece-local cell -> the dungeon frame (3.4.1 cell rotation). */
const L = (pid, v) => U.xformCell(U.quarter(PC(pid).yaw), PC(pid).pos, v);
const Lyaw = (pid, y) => ((y + PC(pid).yaw) % 360 + 360) % 360;
const sugg = (pid, kind) => probe.refs.filter(r => r.piece === pid && r.kind === kind);
/** Is the cell above `pos` open (inside a carve, clear of every fill that
 *  stands up off the floor) for h cells? Props and refs go only where it is. */
const free = (pos, h) => {
  for (let dy = 1; dy <= (h || 6); dy++) {
    const q = [pos[0] + 0.5, pos[1] + dy + 0.5, pos[2] + 0.5];
    let inCarve = false, blocked = false;
    for (const pc of placed.values()) for (const p of pc.prims) {
      if (p.op === 'carve' && U.primContains(p, q)) inCarve = true;
      if (p.op === 'fill' && p.mat !== 'rubble' && p.mat !== 'spoil' && p.tag !== 'light' && U.primContains(p, q)) blocked = true;
    }
    if (!inCarve || blocked) return false;
  }
  return true;
};
/** `pos` if it is open, else the nearest open floor cell within 24 (same y). */
const need = (what, pos, h) => {
  for (let r = 0; r <= 24; r++) for (let dx = -r; dx <= r; dx++) for (let dz = -r; dz <= r; dz++) {
    if (Math.max(Math.abs(dx), Math.abs(dz)) !== r) continue;
    const q = [pos[0] + dx, pos[1], pos[2] + dz];
    if (free(q, h)) { if (r && flag('--plan')) console.log('  (' + what + ' moved ' + [dx, dz] + ' to open floor)'); return q; }
  }
  throw new Error(what + ' at ' + pos + ': no open floor within 24 cells');
};

// ---- authored props: the story the pieces do not tell by themselves ----------
d.props = [];
const prop = (piece, id, propId, params, pos, yaw, seed) => d.props.push({id, prop: propId, params: params || {}, seed: seed || 0, pos: pos.map(Math.round), yaw: ((yaw || 0) % 360 + 360) % 360, piece});
// A cart of waste left on the track inside the cut, as if the last shift
// pushed it out and walked away.
prop('p0', 'x_cut_cart', 'mines/ore_cart', {load: 'rubble'}, need('cut cart', [0, 0, 32], 9), 0, 11);
// The hub: a cart thrown over by the fall that sealed the north drift.
prop(p2, 'x_wreck', 'mines/ore_cart', {tipped: true, load: 'rubble'}, need('wreck', [44, 0, 484], 8), 90, 12);
// The camp: the lever that lifts the deep gate, by the back wall.
const leverPos = need('lever', L(p4, [-PC(p4).params.width * 5 + 6, 0, 70]), 18);
prop(p4, 'x_lever', 'mines/lever', {height: 1.3, pulled: false, post: true}, leverPos, Lyaw(p4, 90), 13);
// The incline foot: a cart of crystal that never went up.
const footZ = S(p7, 'out').pos[2];
prop(p8, 'x_crystal_cart', 'mines/ore_cart', {load: 'crystal'}, need('crystal cart', [0, -180, footZ + 22], 9), 0, 14);
// The Glimmer: crystal clusters grown out of the floor between the pillars.
// (the stope's one bench lifts the back half of its floor 0.8 m)
const benchAt = (pid, lz) => { const P = PC(pid).params; let y = 0; for (let k = 1; k <= P.benches; k++) if (lz >= 10 + Math.round(P.length * 10 * (1 - k / (P.benches + 1)))) y = k * 8; return y; };
const glim = (lx, lz, k, sz) => prop(p10, 'x_glim' + k, 'caves/crystal_cluster', {size: sz}, need('glimmer crystal ' + k, L(p10, [lx, benchAt(p10, lz), lz]), 6), Lyaw(p10, (k * 90) % 360), 20 + k);
glim(-80, 40, 0, 1.4); glim(85, 150, 1, 1.0); glim(-30, 230, 2, 1.8); glim(60, 60, 3, 0.8);
// The winze foot: what is left of the shift boss's mate, and his key.
const footSock = S(p14, 'foot');
const bonesPos = need('bones', [footSock.pos[0] + 12, -180, footSock.pos[2] + 8], 4);
prop(p14, 'x_bones', 'ruins/bones', {}, bonesPos, 90, 31);
// The worked-out stope: the assay chest, the one thing they meant to come back for.
const st12 = PC(p12).params;
const assayPos = need('assay chest', L(p12, [-Math.round(st12.width * 5) + 14, 0, 10 + Math.round(st12.length * 10 * 0.18)]), 12);
prop(p12, 'x_assay', 'mines/crate', {size: 1.1, stack: 1}, assayPos, Lyaw(p12, 90), 32);
// The drowned sump: a body on the ledge.
prop(p11, 'x_sump_bones', 'ruins/bones', {}, need('sump bones', L(p11, [-Math.round(PC(p11).params.width * 5) + 6, 0, 60]), 3), Lyaw(p11, 0), 33);

// =============================================================================
// 3. compile + refs
// =============================================================================
const dir = join(ASSETS, 'dungeons', ID);
mkdirSync(dir, {recursive: true});
d.refs = 'refs.json';
const text = G.dungeonJsonText(d);
const prog = G.compile(JSON.parse(text), {id: ID, sourceText: text, materials: MATS});
const wname = (n) => 'wn_' + n.replace(/\//g, '_');
const refs = [];
const ref = (name, kind, pos, yaw, props, base) => { const r = {name, kind, pos: pos.map(Math.round), yaw: yaw || 0, props: props || {}}; if (base) r.base = base; refs.push(r); return r; };

// Every generated waynode becomes a waynode ref (the tab's "Adopt waynodes"):
// the patrol route names them.
for (const w of prog.waynodes) {
  if (w.pit) continue;
  ref(wname(w.name), 'waynode', w.pos, 0, {links: w.links.filter(l => !(WN.get(l) || {}).pit).map(wname)});
}

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

// ---- the camp door --------------------------------------------------------------
{
  const s = S(p4, 'in');                                       // faces out of the camp, toward the north drift
  const yaw = ({'1,0,0': 90, '-1,0,0': 270, '0,0,1': 0, '0,0,-1': 180})[s.dir.join(',')];
  const fr = E.frameOf(yaw), w = 22;
  // 3 cells inside the doorway, centred: start half a leaf to the LEFT.
  const pos = [0, 1, 2].map(i => s.pos[i] - s.dir[i] * 3 - fr.right[i] * Math.round(w / 2));
  ref('camp_door', 'door', pos, yaw, {width: w, height: 21, thickness: 1, hinge: 'left'});
}

// ---- the cave-in: a loose roof that comes down on whoever digs at the toe ------
{
  const t = sugg(p5, 'trap')[0];
  const R = t.props.region;
  ref('roof_slab', 'region', R.min, 0, {size: [0, 1, 2].map(i => R.max[i] - R.min[i])});
  const inS = S(p5, 'in');                                     // the hub side of the collapse (faces -x)
  ref('roof_plate', 'plate', [inS.pos[0] + 2, 0, inS.pos[2] - 7], 0, {size: [Math.max(6, R.min[0] - inS.pos[0] - 4), 3, 15], latch: false, who: 'player'});
  ref('roof_fall', 'trap', R.min, 0, {type: 'collapse_floor', on: 'roof_plate', region: 'roof_slab', once: true});
}

// ---- containers ----------------------------------------------------------------------
{
  const nc = sugg(p1, 'container')[0];
  ref('haulage_niche', 'container', nc.pos, 0, {loot: 'mine_supplies', title: 'crate'});
  const campCrate = placed.get(p4).props.find(pr => pr.prop === 'mines/crate');
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
  ref('shift_boss', 'npc', wp('p2/hub'), 0, {name: 'the shift boss', behavior: 'zombie', schedule: 'shardwell_shift',
      outfit: ['smock#5a4b36', 'breeches#3b3127', 'iron_helm']}, 'zombie');
  // The watchman, an old boiler automaton, still guards the incline foot.
  const post = wp('p8/w0');
  ref('watch_post', 'marker', post, 180, {});
  ref('watchman', 'npc', post, 180, {name: 'the watchman', behavior: 'guardian', schedule: 'shardwell_watch', weapon: 'mace'}, 'boilerman');
  // The Glimmer's dead, asleep among the pillars until someone walks in.
  const heart = wp('p10/c');
  ref('glimmer_heart', 'marker', heart, 0, {});
  const stIn = S(p10, 'in');                                   // the stope's door (faces +x, toward the crossing)
  ref('glimmer', 'region', [stIn.pos[0] - 80, -180, stIn.pos[2] - 50], 0, {size: [70, 40, 100]});
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
for (const [name, j] of Object.entries(SCHED)) {
  const rows = j.rows.map(r => '    ' + JSON.stringify(r)).join(',\n');
  writeFileSync(join(ASSETS, 'schedules', name + '.json'), '{\n  "about": ' + JSON.stringify(j.about) + ',\n  "rows": [\n' + rows + '\n  ]\n}\n');
}
if (flag('--plan')) {
  for (const pc of prog.pieces) console.log(pc.id.padEnd(4), (pc.kit + '/' + pc.type).padEnd(22), 'bounds', JSON.stringify(pc.bounds.min), JSON.stringify(pc.bounds.max), pc.prims + ' prims');
  for (const s of prog.sockets) console.log('  sock', s.piece, s.name.padEnd(7), JSON.stringify(s.pos), JSON.stringify(s.dir), s.w + 'x' + s.h, s.linked ? 'linked' : 'OPEN');
  if (flag('--way')) for (const w of prog.waynodes) console.log('  way', w.name.padEnd(14), JSON.stringify(w.pos), w.links.join(' '));
  for (const r of prog.refs) console.log('  sugg', r.id.padEnd(18), r.kind.padEnd(9), JSON.stringify(r.pos), JSON.stringify(r.props || {}));
}

// =============================================================================
// 3. write
// =============================================================================
writeFileSync(join(dir, 'dungeon.json'), text);
writeFileSync(join(dir, 'shell.json'), G.emitShellJson(prog));
writeFileSync(join(dir, 'shell.bin'), G.emitShellBin(prog));
writeFileSync(join(dir, 'refs.json'), E.writeRefs(ID, ABOUT(), refs));
function ABOUT() {
  return 'The Shardwell, the abandoned crystal mine in the Shardspire crater (assets/worldmap/default/scripts/dungeon_shardwell.mjs). ' +
         'Lever camp_lever (miners\' camp) lifts incline_gate (head of the haulage incline). The winze links the deep level to the far side of the cave-in.';
}

// =============================================================================
// 4. validate (the tab's panel: dungeongen.validate + ui_entities.refProblems)
// =============================================================================
const cat = catalog();
const opts = {refs: E.forValidate(refs, cat), materials: MATS};
const hm = heightmap(prog.bounds);
if (hm) {
  const q = ((PLACE.yaw / 90) % 4 + 4) % 4;
  const R = (x, z) => [[x, z], [z, -x], [-x, -z], [-z, x]][q];
  const g0 = hm.at(PLACE.at[0], PLACE.at[1]);
  const W = (x, z) => { const [wx, wz] = R(x, z); return [PLACE.at[0] + wx, PLACE.at[1] + wz]; };
  opts.groundAt = (x, z) => { const [wx, wz] = W(x + 0.5, z + 0.5); const r = hm.at(Math.floor(wx), Math.floor(wz)); return r === null ? Infinity : r - g0; };
}
const issues = G.validate(JSON.parse(text), opts).concat(E.refProblems(refs, cat, new Set(prog.waynodes.map(w => w.name))));
const nErr = issues.filter(v => v.severity === 'error').length, nWarn = issues.filter(v => v.severity === 'warn').length;
for (const v of issues) if (v.severity !== 'info' || flag('--info')) console.log('  ' + v.severity.padEnd(5) + ' ' + (v.code || '').padEnd(11) + ' ' + (v.piece || v.ref || '-') + ': ' + v.msg);
const ext = [0, 1, 2].map(i => prog.bounds.max[i] - prog.bounds.min[i] + 1);
console.log(ID + ': ' + d.pieces.length + ' pieces, ' + prog.prims.length + ' prims, ' + prog.props.instances.length + ' prop instances, ' + refs.length + ' refs; extent ' +
            ext.map(v => (v / 10).toFixed(1) + ' m').join(' x ') + '; worst 64^3 cell ' + prog.limits.primsPerCell + ' prims / ' + prog.limits.instancesPerCell + ' instances; ' +
            (hm ? 'ground-checked' : 'NO ground check (run with --ground)') + '; ' + nErr + ' errors, ' + nWarn + ' warnings');
process.exit(nErr ? 1 : 0);

// =============================================================================
// helpers
// =============================================================================
/** The tab's pickers (tuner_server.py _dungeon_catalog), from disk. */
function catalog() {
  const stems = (p, ext) => existsSync(p) ? readdirSync(p).filter(n => n.endsWith(ext)).map(n => n.slice(0, -ext.length)) : [];
  const mdir = join(ASSETS, 'mobs');
  const subs = readdirSync(mdir).filter(s => !['effects', 'pool'].includes(s) && !s.startsWith('.') && statSync(join(mdir, s)).isDirectory());
  let mobs = stems(mdir, '.vox');
  for (const s of subs) mobs = mobs.concat(stems(join(mdir, s), '.vox'));
  for (const dd of [mdir].concat(subs.map(s => join(mdir, s)))) for (const st of stems(dd, '.json')) {
    try { if ('extends' in JSON.parse(readFileSync(join(dd, st + '.json'), 'utf8'))) mobs.push(st); } catch (e) {}
  }
  const beh = JSON.parse(readFileSync(join(mdir, 'behaviors.json'), 'utf8')).profiles.map(p => p.name).filter(Boolean);
  const ij = JSON.parse(readFileSync(join(ASSETS, 'items/items.json'), 'utf8'));
  const keys = (ij.items || ij).filter(it => it.kind === 'key').map(it => ({id: it.id, unlocks: it.unlocks || it.id}));
  return {loot: stems(join(ASSETS, 'loot'), '.json'), schedules: stems(join(ASSETS, 'schedules'), '.json'), mobs: [...new Set(mobs)].sort(), behaviors: beh, keys};
}
/** The engine's ground over the dungeon's footprint: `sandvox --heightmap`
 *  (GPU-free, the tab's /api/heightmap), cached in build/. */
function heightmap(bounds) {
  const file = join(ROOT, 'build', 'shardwell_ground.bin');
  if (flag('--ground')) {
    const exe = process.env.SANDVOX_EXE || join(ROOT, 'build', 'Release', 'sandvox.exe');
    const span = 4096, res = 1024;
    const r = spawnSync(exe, ['--heightmap', [PLACE.at[0] - 900, PLACE.at[1] + 300, span, res, 1337].join(','), '--heightmap-out', file],
                        {env: Object.assign({}, process.env, {SANDVOX_ASSET_DIR: ASSETS, SANDVOX_MAP: 'default', SANDVOX_NO_CRASH_DIALOG: '1'}), encoding: 'utf8'});
    if (r.status !== 0) { console.error('heightmap failed: ' + (r.stderr || r.stdout || r.error)); return null; }
  }
  if (!existsSync(file)) return null;
  void bounds;
  const b = readFileSync(file), dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const n = dv.getUint32(8, true), span = dv.getUint32(12, true), cx = dv.getInt32(16, true), cz = dv.getInt32(20, true);
  return {at(x, z) {
    const i = Math.floor((x - (cx - span / 2)) / span * n), j = Math.floor((z - (cz - span / 2)) / span * n);
    if (i < 0 || j < 0 || i >= n || j >= n) return null;
    return dv.getInt32(48 + (j * n + i) * 8, true);
  }};
}
