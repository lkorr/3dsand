/* dungeongen.js — the dungeon core (PLAN_dungeons §3.3): pieces placed by
 * quarter turns, sockets matched (with an auto TAPER where two openings
 * differ), every piece unioned into ONE primitive program (§3.4), the
 * validator (§5.2's panel) and the seeded layout assist ("grow a branch").
 *
 * PURE MODULE — no DOM, no fetch, no Math.random. The tuner's Dungeons tab,
 * scripts/compile_dungeon.mjs and scripts/test_dungeongen.mjs call the same
 * functions, so the preview is the asset.
 *
 * FRAMES.
 *   piece-local : what a kit generator draws in (dgutil.js: origin socket at
 *                 (0,0,0), body to +Z, Y up, voxels).
 *   dungeon     : dungeon.json's frame. A piece's `pos` is where its local
 *                 origin lands, `yaw` (multiple of 90) turns it about +Y
 *                 (heading convention: yaw 90 sends local +Z to +X).
 *   anchor      : the compiled program's frame = the dungeon frame translated
 *                 so the ENTRANCE socket sits at (0,0,0). The engine puts that
 *                 point at the map anchor (PLAN §2.1) and applies the map yaw.
 */
'use strict';
import {
  hashN, frand, strHash, fnvHex, quarter, rotV, xformP, xformCell, xformPrim, invXformP, dirName,
  primContains, shapeBounds, primReach, boxUnion, boxOverlap, normalizeParams, defaultsOf
} from './dgutil.js';
import CAVES from './kit_caves.js';
import MINES from './kit_mines.js';
import RUINS from './kit_ruins.js';
import { PROPS, PROP_IDS } from '../../dungeons/props/index.js';

export const DUNGEON_VERSION = 1;
/** Bumped when the same dungeon.json would compile to a different program. */
export const COMPILER_VERSION = 1;
export const SHELL_FORMAT = 'sandvox-dungeon-shell';
export const SHELL_VERSION = 1;
export const KITS = {caves: CAVES, mines: MINES, ruins: RUINS};
export const STYLES = ['caves', 'mines', 'ruins'];
export { PROPS, PROP_IDS };

/** Socket size difference (voxels) joined without a taper. */
export const SOCKET_DIRECT_TOL = 2;
/** Beyond these a join is refused: no taper can make it read as one space. */
export const SOCKET_TAPER_MAX_RATIO = 2.6;
export const SOCKET_TAPER_MAX_DIFF = 60;

// =============================================================================
// kits, types, pieces
// =============================================================================
export function getKit(kit) {
  const K = KITS[kit];
  if (!K) throw new Error('unknown kit "' + kit + '" (have ' + STYLES.join(', ') + ')');
  return K;
}
export function getType(kit, type) {
  const T = getKit(kit).TYPES[type];
  if (!T) throw new Error('kit ' + kit + ' has no piece type "' + type + '"');
  return T;
}
/** [{type, label, version, surface, presets:[names]}] in the kit's order. */
export function listTypes(kit) {
  const K = getKit(kit);
  return K.TYPE_ORDER.map(t => ({type: t, label: K.TYPES[t].label, version: K.TYPES[t].version,
                                 surface: !!K.TYPES[t].surface, presets: Object.keys(K.TYPES[t].PRESETS)}));
}
export function pieceSchema(kit, type) { return getType(kit, type).SCHEMA; }
/** Full normalized params of a preset (or the defaults). */
export function presetParams(kit, type, preset) {
  const T = getType(kit, type);
  const pr = preset ? T.PRESETS[preset] : null;
  if (preset && !pr) throw new Error(kit + '/' + type + ' has no preset "' + preset + '"');
  return T.normalize(pr ? pr.params : {});
}

export function newDungeon(o) {
  o = o || {};
  return {version: DUNGEON_VERSION, name: o.name || 'Untitled', style: o.style || 'caves', seed: (o.seed >>> 0) || 1,
          palette: {}, pieces: [], entrance: null, props: []};
}
export function nextPieceId(d) {
  let n = 0;
  for (const p of d.pieces || []) { const m = /^p(\d+)$/.exec(p.id); if (m) n = Math.max(n, Number(m[1]) + 1); }
  return 'p' + n;
}

/** Bring a piece up to its type's version, then fill + clamp its params.
 *  The migration hook: a type may export migrate(params, fromVersion). */
export function migratePiece(piece) {
  const T = getType(piece.kit, piece.type);
  let params = piece.params || {};
  const from = piece.version === undefined ? T.version : piece.version;
  if (from < T.version && typeof T.migrate === 'function') params = T.migrate(JSON.parse(JSON.stringify(params)), from);
  return Object.assign({}, piece, {version: T.version, params});
}
export function normalizePiece(piece) {
  const p = migratePiece(piece);
  const T = getType(p.kit, p.type);
  const out = {id: String(p.id), kit: p.kit, type: p.type, version: T.version, params: T.normalize(p.params),
               seed: (p.seed >>> 0) || 0, pos: (p.pos || [0, 0, 0]).map(v => Math.round(v)), yaw: quarter(p.yaw) * 90};
  if (p.links && Object.keys(p.links).length) out.links = JSON.parse(JSON.stringify(p.links));
  if (p.autoProps === false) out.autoProps = false;
  if (p.label) out.label = p.label;
  return out;
}
export function normalizeDungeon(d) {
  const o = Object.assign(newDungeon(), JSON.parse(JSON.stringify(d || {})));
  o.version = DUNGEON_VERSION;
  o.seed = (o.seed >>> 0) || 1;
  o.pieces = (o.pieces || []).map(normalizePiece);
  o.props = o.props || [];
  return o;
}
/** A piece's effective seed: explicit, or derived from the dungeon seed + id. */
export function pieceSeed(d, piece) { return piece.seed ? piece.seed >>> 0 : hashN(d.seed >>> 0, 'piece', String(piece.id)); }

export function newPiece(d, spec) {
  const T = getType(spec.kit, spec.type);
  const params = T.normalize(Object.assign({}, spec.preset ? presetParams(spec.kit, spec.type, spec.preset) : {}, spec.params || {}));
  return normalizePiece({id: spec.id || nextPieceId(d), kit: spec.kit, type: spec.type, params,
                         seed: spec.seed || 0, pos: spec.pos || [0, 0, 0], yaw: spec.yaw || 0});
}

// =============================================================================
// palettes + materials
// =============================================================================
/** The resolved slot table for a kit in this dungeon: kit variant, then the
 *  dungeon's overrides. dungeon.palette = {variant?, <slot>: name, kits: {<kit>:
 *  {variant?, <slot>: name}}}; top-level slots apply to the dungeon's style. */
export function resolvePalette(d, kit) {
  const K = getKit(kit), pal = (d && d.palette) || {};
  const kp = (pal.kits && pal.kits[kit]) || {};
  const variant = kp.variant || (kit === d.style ? pal.variant : undefined) || K.DEFAULT_PALETTE;
  const out = Object.assign({}, K.PALETTES[variant] || K.PALETTES[K.DEFAULT_PALETTE]);
  if (kit === d.style) for (const k in pal) if (k !== 'variant' && k !== 'kits' && typeof pal[k] === 'string') out[k] = pal[k];
  for (const k in kp) if (k !== 'variant' && typeof kp[k] === 'string') out[k] = kp[k];
  return out;
}
/** A prim/prop material spec -> a material name. '@slot' and bare slot names
 *  resolve through the kit's palette (falling back to the style palette, then
 *  the wall slot); anything else is already a material name. */
function matResolver(d) {
  const cache = {};
  const pal = (k) => cache[k] || (cache[k] = resolvePalette(d, k));
  return (kit, spec) => {
    if (!spec) return {mat: pal(kit).wall, slot: 'wall'};
    const explicit = spec[0] === '@';
    const slot = explicit ? spec.slice(1) : spec;
    const P = pal(kit);
    if (P[slot]) return {mat: P[slot], slot};
    if (explicit) {
      const S = pal(d.style);
      return S[slot] ? {mat: S[slot], slot} : {mat: P.wall, slot: 'wall'};
    }
    return {mat: spec, slot: null};
  };
}

// =============================================================================
// placement
// =============================================================================
/** Generate one piece in its LOCAL frame (the kit generator's result). */
export function generatePiece(d, piece) {
  const p = normalizePiece(piece);
  return getType(p.kit, p.type).generate(p.params, pieceSeed(d, p), {vpm: 10});
}

/** One piece placed in the DUNGEON frame: prims, sockets, props, refs and
 *  waynodes transformed by its pos + yaw. */
export function placePiece(d, piece, local) {
  const p = normalizePiece(piece);
  const res = local || generatePiece(d, p);
  const q = quarter(p.yaw), t = p.pos;
  const X = (v) => xformP(q, t, v);          // continuous points (sockets)
  const XC = (v) => xformCell(q, t, v);      // cells (props, refs, waynodes, light mounts)
  const R = (v) => rotV(q, v).map(z => z === 0 ? 0 : z);
  const yawOf = (y) => (((y || 0) + q * 90) % 360 + 360) % 360;
  const out = {
    id: p.id, kit: p.kit, type: p.type, version: p.version, seed: pieceSeed(d, p),
    surface: !!res.surface, flooded: !!res.flooded, notes: res.notes || [],
    prims: res.prims.map(pr => xformPrim(q, t, pr)),
    scatter: res.scatter.map(s => Object.assign({}, s)),
    sockets: res.sockets.map(s => Object.assign({}, s, {pos: X(s.pos), dir: R(s.dir), piece: p.id})),
    props: res.props.map(pr => Object.assign({}, pr, {pos: XC(pr.pos), yaw: yawOf(pr.yaw)})),
    refs: res.refs.map(r => Object.assign({}, r, {pos: XC(r.pos), yaw: yawOf(r.yaw)},
                                           r.props && r.props.region ? {props: Object.assign({}, r.props, {region: xformRegion(q, t, r.props.region)})} : {})),
    waynodes: res.waynodes.map(w => Object.assign({}, w, {pos: XC(w.pos)})),
    lightSpots: res.lightSpots.map(l => Object.assign({}, l, {pos: XC(l.pos)}))
  };
  let bb = null;
  for (const pr of out.prims) bb = boxUnion(bb, primReach(pr));
  out.bounds = bb || {min: t.slice(), max: t.map(v => v + 1)};
  return out;
}
function xformRegion(q, t, r) {
  const a = xformP(q, t, r.min), b = xformP(q, t, r.max);
  return {min: a.map((v, i) => Math.min(v, b[i])), max: a.map((v, i) => Math.max(v, b[i]))};
}

/** Every socket of every piece, in the dungeon frame. */
export function socketsOf(d, placed) {
  const P = placed || d.pieces.map(p => placePiece(d, p));
  const out = [];
  for (const pc of P) for (const s of pc.sockets) out.push(s);
  return out;
}

const eq3 = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
const opposite = (a, b) => a[0] === -b[0] && a[1] === -b[1] && a[2] === -b[2];
const isVert = (s) => s.dir[1] !== 0;

/** Can sockets a and b join (positions aside)?
 *  -> {ok, taper, reason}. Size within SOCKET_DIRECT_TOL and the same profile
 *  family joins directly; within the taper limits an auto taper bridges them;
 *  beyond, refused with the reason. */
export function socketCompat(a, b) {
  if (isVert(a) !== isVert(b)) return {ok: false, taper: false, reason: 'a vertical socket only joins a vertical one'};
  const fam = (s) => (s.profile === 'rect' || s.profile === 'arch') ? 'built' : s.profile === 'shaft' ? 'shaft' : 'natural';
  const dw = Math.abs(a.w - b.w), dh = Math.abs(a.h - b.h);
  const ratio = Math.max(a.w / Math.max(1, b.w), b.w / Math.max(1, a.w), a.h / Math.max(1, b.h), b.h / Math.max(1, a.h));
  if (ratio > SOCKET_TAPER_MAX_RATIO || Math.max(dw, dh) > SOCKET_TAPER_MAX_DIFF)
    return {ok: false, taper: false, reason: 'openings too different (' + a.w + 'x' + a.h + ' vs ' + b.w + 'x' + b.h + ' voxels; a taper bridges up to x' + SOCKET_TAPER_MAX_RATIO + ' and ' + SOCKET_TAPER_MAX_DIFF + ' voxels): resize one or put a piece between'};
  const famDiff = !isVert(a) && fam(a) !== fam(b);
  if (dw <= SOCKET_DIRECT_TOL && dh <= SOCKET_DIRECT_TOL && !famDiff) return {ok: true, taper: false, reason: ''};
  return {ok: true, taper: true, reason: famDiff ? 'profile ' + a.profile + ' meets ' + b.profile : 'size ' + a.w + 'x' + a.h + ' meets ' + b.w + 'x' + b.h};
}

/** Where must a piece go so its LOCAL socket `ls` meets the world socket
 *  `target`? -> {pos, yaw} (or null if no quarter turn faces them). */
export function snapTransform(target, ls, preferYaw) {
  const want = target.dir.map(v => -v);
  const order = [quarter(preferYaw || 0), 0, 1, 2, 3];
  for (const q of order) {
    const r = rotV(q, ls.dir);
    if (r[0] === want[0] && r[1] === want[1] && r[2] === want[2]) {
      const rp = rotV(q, ls.pos);
      return {pos: [target.pos[0] - rp[0], target.pos[1] - rp[1], target.pos[2] - rp[2]].map(v => v === 0 ? 0 : v), yaw: q * 90};
    }
  }
  return null;
}

/** All joins: explicit `links` (either side) plus coincident, opposed socket
 *  pairs (auto). -> {links:[{a, b, auto, compat}], problems:[{piece, msg}]}. */
export function resolveLinks(d, placed) {
  const socks = socketsOf(d, placed);
  const byKey = new Map(socks.map(s => [s.piece + '#' + s.name, s]));
  const links = [], problems = [], seen = new Set();
  const add = (a, b, auto) => {
    const k = [a.piece + '#' + a.name, b.piece + '#' + b.name].sort().join('|');
    if (seen.has(k)) return;
    seen.add(k);
    links.push({a, b, auto, compat: socketCompat(a, b)});
  };
  for (const pc of d.pieces) for (const sn of Object.keys(pc.links || {})) {
    const L = pc.links[sn];
    const a = byKey.get(pc.id + '#' + sn);
    const b = L && byKey.get(L.piece + '#' + L.socket);
    if (!a) { problems.push({piece: pc.id, msg: 'link names socket "' + sn + '" which ' + pc.type + ' does not have'}); continue; }
    if (!b) { problems.push({piece: pc.id, msg: 'socket ' + sn + ' links to ' + (L ? L.piece + '.' + L.socket : '?') + ', which does not exist'}); continue; }
    if (!eq3(a.pos, b.pos) || !opposite(a.dir, b.dir)) {
      problems.push({piece: pc.id, msg: 'socket ' + sn + ' is linked to ' + b.piece + '.' + b.name + ' but they do not meet (at ' + a.pos + ' facing ' + dirName(a.dir) + ' vs ' + b.pos + ' facing ' + dirName(b.dir) + '): re-snap the piece'});
      continue;
    }
    add(a, b, false);
  }
  const at = new Map();
  for (const s of socks) { const k = s.pos.join(','); if (!at.has(k)) at.set(k, []); at.get(k).push(s); }
  for (const list of at.values()) for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++)
    if (list[i].piece !== list[j].piece && opposite(list[i].dir, list[j].dir)) add(list[i], list[j], true);
  return {links, problems};
}

/** The auto taper for a join that differs within tolerance: rounded boxes
 *  stepping from the bigger opening down to the smaller one, cut into the
 *  SMALLER piece (whose end it widens), so the join reads as one space. */
export function taperPrims(a, b, blend) {
  const big = a.w * a.h >= b.w * b.h ? a : b, small = big === a ? b : a;
  const L = Math.max(8, Math.min(48, 2 * Math.max(Math.abs(big.w - small.w), Math.abs(big.h - small.h))));
  const n = Math.max(2, Math.ceil(L / 4));
  const into = small.dir.map(v => -v);            // from the join into the small piece
  const prims = [];
  const natural = (s) => s.profile === 'round' || s.profile === 'rough';
  for (let k = 0; k < n; k++) {
    const t0 = k / n, t1 = (k + 1) / n;
    const w = Math.round(big.w + (small.w - big.w) * t0), h = Math.round(big.h + (small.h - big.h) * t0);
    const s0 = Math.round(L * t0) - (k === 0 ? 1 : 0), s1 = Math.round(L * t1) + 1;
    let min, max;
    if (isVert(small)) {
      const y0 = small.pos[1] + into[1] * s0, y1 = small.pos[1] + into[1] * s1;
      min = [small.pos[0] - Math.round(w / 2), Math.min(y0, y1), small.pos[2] - Math.round(h / 2)];
      max = [small.pos[0] + Math.round(w / 2), Math.max(y0, y1), small.pos[2] + Math.round(h / 2)];
    } else {
      const along = into[0] !== 0 ? 0 : 2, across = along === 0 ? 2 : 0;
      const c0 = small.pos[along] + into[along] * s0, c1 = small.pos[along] + into[along] * s1;
      min = [0, small.pos[1], 0]; max = [0, small.pos[1] + h, 0];
      min[along] = Math.min(c0, c1); max[along] = Math.max(c0, c1);
      min[across] = small.pos[across] - Math.round(w / 2); max[across] = small.pos[across] + Math.round(w / 2);
    }
    const r = natural(big) || natural(small) || big.profile === 'arch' ? Math.max(1, Math.round(Math.min(w, h) / 4)) : 0;
    prims.push({op: 'carve', shape: r ? 'rbox' : 'box', min, max, ...(r ? {r} : {}), blend: Math.max(4, blend || 0), tag: 'taper'});
  }
  return prims;
}

// =============================================================================
// assembly (shared by compile, validate and grow)
// =============================================================================
/** Place every piece and resolve the joins. */
export function assemble(dIn) {
  const d = normalizeDungeon(dIn);
  const placed = d.pieces.map(p => placePiece(d, p));
  const {links, problems} = resolveLinks(d, placed);
  return {d, placed, links, problems};
}

/** The entrance socket (anchor): dungeon.entrance, else the first socket
 *  tagged 'surface', else the first piece's origin. */
export function entranceOf(d, placed) {
  const socks = socketsOf(d, placed);
  if (d.entrance && d.entrance.piece) {
    const s = socks.find(q => q.piece === d.entrance.piece && q.name === d.entrance.socket);
    if (s) return s;
  }
  const s = socks.find(q => q.tags && q.tags.includes('surface'));
  if (s) return s;
  return d.pieces.length ? {piece: d.pieces[0].id, name: '(origin)', pos: d.pieces[0].pos.slice(), dir: [0, 0, -1], w: 0, h: 0, profile: 'rect', tags: []}
                         : {piece: null, name: '(none)', pos: [0, 0, 0], dir: [0, 0, -1], w: 0, h: 0, profile: 'rect', tags: []};
}

// =============================================================================
// props -> templates (PLAN 3.4.1 "Props")
// =============================================================================
/** Cell rotation of a prop-local cell (x, z) by q quarter turns (3.4.1 Rot). */
function rotCell(q, x, z) {
  switch (q & 3) { case 0: return [x, z]; case 1: return [z, -1 - x]; case 2: return [-1 - x, -1 - z]; default: return [-1 - z, x]; }
}
/** Bake one prop to a 3.4.1 Template: {id, size, palette, voxels}, voxels in
 *  template-local cells 0..n-1 (the prop's cells minus their min corner);
 *  '@carve' is the carve code (the cell becomes air); a cell not listed is
 *  template-air. Also returns `min` (the prop-local cell of template 0,0,0)
 *  for instance placement. resolve(spec) maps '@slot' / names to names. */
export function bakeProp(propId, params, seed, resolve) {
  const P = PROPS[propId];
  if (!P) throw new Error('unknown prop "' + propId + '"');
  const r = P.generate(params || {}, seed >>> 0);
  const cells = r.cells.length ? r.cells : [{x: 0, y: 0, z: 0, mat: '#carve'}];
  const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
  for (const c of cells) { mn[0] = Math.min(mn[0], c.x); mn[1] = Math.min(mn[1], c.y); mn[2] = Math.min(mn[2], c.z); mx[0] = Math.max(mx[0], c.x); mx[1] = Math.max(mx[1], c.y); mx[2] = Math.max(mx[2], c.z); }
  const palette = [], idx = new Map();
  const code = (m) => {
    const name = m === '#carve' ? '@carve' : (resolve ? resolve(m) : m);
    if (!idx.has(name)) { idx.set(name, palette.length); palette.push(name); }
    return idx.get(name);
  };
  if (cells.some(c => c.mat === '#carve')) code('#carve');           // '@carve' first when used
  const voxels = cells.map(c => [c.x - mn[0], c.y - mn[1], c.z - mn[2], code(c.mat)]);
  const size = [mx[0] - mn[0] + 1, mx[1] - mn[1] + 1, mx[2] - mn[2] + 1];
  const key = hashN(strHash(propId), strHash(JSON.stringify(r.params)), r.seed, strHash(palette.join(','))).toString(16).padStart(8, '0');
  return {id: propId + '#' + key, size, palette, voxels, min: mn, prop: propId, params: r.params, seed: r.seed};
}
/** Where a template's (0,0,0) lands for an instance whose prop ORIGIN cell is
 *  `pos` with `yaw`: the rotated footprint's MIN CELL (3.4.1 Instance.pos). */
export function instancePos(tpl, pos, yaw) {
  const q = quarter(yaw);
  let mx = Infinity, mz = Infinity;
  for (const x of [tpl.min[0], tpl.min[0] + tpl.size[0] - 1]) for (const z of [tpl.min[2], tpl.min[2] + tpl.size[2] - 1]) {
    const r = rotCell(q, x, z);
    mx = Math.min(mx, r[0]); mz = Math.min(mz, r[1]);
  }
  return [pos[0] + mx, pos[1] + tpl.min[1], pos[2] + mz];
}

// =============================================================================
// compile: the whole dungeon -> one primitive program (PLAN 3.4 / 3.4.1)
// =============================================================================
const LAYER_OF = {carve: 0, skin: 1, fill: 2, keep: 3};
/** Engine per-cell limits (3.4.1): prims / prop instances whose inflated
 *  bounds touch one 64^3 cell. */
export const LIMIT_PRIMS_PER_CELL = 64, LIMIT_INSTANCES_PER_CELL = 32, MAX_SCATTER_PER_PRIM = 4;

/** Internal (continuous, half-open) prim -> the 3.4.1 record (inclusive cells). */
function toShellPrim(p, res, kit, seedOf) {
  const o = {op: p.op, shape: p.shape === 'spline_tube' ? 'tube' : p.shape};
  const inc = (v) => v.map(x => x - 1);
  if (p.shape === 'capsule') { o.a = p.a.slice(); o.b = p.b.slice(); o.r = p.r; }
  else if (p.shape === 'spline_tube') { o.pts = p.pts.map(q => q.slice()); o.r = p.radii.slice(); }
  else { o.min = p.min.slice(); o.max = inc(p.max); }
  if (p.shape === 'rbox') o.r = p.r || 0;
  if (p.shape === 'cylinder' || p.shape === 'vault') o.axis = p.axis;
  if (p.shape === 'vault') o.rise = p.rise;
  if (p.shape === 'ramp') { o.dir = p.dir; o.stepH = p.stepH; o.stepD = p.stepD; o.height = p.height || 0; o.below = p.below || 0; }
  const used = [];
  if (p.op === 'fill' || p.op === 'skin' || (p.op === 'carve' && p.mat)) {
    const m = res(kit, p.mat);
    o.mat = m.mat; used.push(m.mat);
  }
  if (p.op === 'carve' && p.mat && p.level !== undefined) o.level = p.level;
  if (p.noise) {
    const amp = Math.round(p.noise.amp);
    if (amp >= 1) o.noise = {amp: Math.min(64, amp), scale: Math.max(1, Math.min(10, Math.round(Math.log2(Math.max(2, p.noise.scale))))),
                             octaves: Math.max(1, Math.min(3, p.noise.octaves || 1)), seed: seedOf('noise', String(p.noise.seedKey))};
  }
  if (p.op === 'carve' && p.blend) o.blend = Math.min(256, p.blend);
  if (p.op === 'skin') o.skin = Math.max(1, Math.min(64, p.skin || 2));
  return {o, used};
}
function shellScatter(s, res, kit, seed) {
  const m = res(kit, s.mat);
  const lo = Math.max(1, Math.min(32, Math.round(s.size[0]))), hi = Math.max(lo, Math.min(32, Math.round(s.size[1])));
  const r = {kind: s.kind, density: Math.max(0, Math.min(1, Number((+s.density).toFixed(4)))), size: [lo, hi], surface: s.surface || 'floor', mat: m.mat, seed};
  if (s.spacing) r.spacing = s.spacing;
  return {r, mat: m.mat};
}
/** The inflated cell box a prim can touch (noise, blend, skin). */
export function shellPrimBounds(o) {
  let mn, mx;
  if (o.shape === 'capsule') { mn = [0, 1, 2].map(i => Math.min(o.a[i], o.b[i]) - o.r); mx = [0, 1, 2].map(i => Math.max(o.a[i], o.b[i]) + o.r); }
  else if (o.shape === 'tube') {
    mn = [Infinity, Infinity, Infinity]; mx = [-Infinity, -Infinity, -Infinity];
    o.pts.forEach((q, i) => { for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], q[k] - o.r[i]); mx[k] = Math.max(mx[k], q[k] + o.r[i]); } });
  } else { mn = o.min.slice(); mx = o.max.slice(); if (o.shape === 'ramp') mn[1] -= o.below || 0; }
  const pad = (o.noise ? o.noise.amp : 0) + (o.blend || 0) + (o.skin || 0);
  return {min: mn.map(v => v - pad), max: mx.map(v => v + pad)};
}
/** Worst count of boxes touching one 64^3 cell, over two grid alignments
 *  (the map placement is unknown here). -> {max, at}. */
function perCellMax(boxes) {
  let best = {max: 0, at: null};
  for (const off of [0, 32]) {
    const cnt = new Map();
    for (const b of boxes) {
      const lo = b.min.map(v => Math.floor((v + off) / 64)), hi = b.max.map(v => Math.floor((v + off) / 64));
      if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 4096) continue;
      for (let x = lo[0]; x <= hi[0]; x++) for (let y = lo[1]; y <= hi[1]; y++) for (let z = lo[2]; z <= hi[2]; z++) {
        const k = x + ',' + y + ',' + z, n = (cnt.get(k) || 0) + 1;
        cnt.set(k, n);
        if (n > best.max) best = {max: n, at: [x * 64 - off, y * 64 - off, z * 64 - off]};
      }
    }
  }
  return best;
}

/**
 * compile(dungeon, {id?, sourceText?, materials?}) -> program.
 * The program IS the 3.4.1 shell (format, version, dungeon, sourceHash,
 * anchor, keepOut, naturalCaves, materials, prims, props{templates,instances})
 * plus D2 extras the engine ignores and the tuner reads (name, style, seed,
 * generator, bounds, limits, pieces, waynodes, refs, sockets, errors, warnings).
 *   id         : the asset dir name (shell "dungeon")
 *   sourceText : dungeon.json as on disk; its FNV-1a (CRs dropped) is sourceHash
 *   materials  : names to check every resolved material against
 * Coordinates are the dungeon frame translated so the ENTRANCE socket is the
 * anchor cell (0,0,0).
 */
export function compile(dIn, opts) {
  opts = opts || {};
  const {d, placed, links, problems} = assemble(dIn);
  const res = matResolver(d);
  const errors = problems.map(p => ({piece: p.piece, msg: p.msg}));
  const warnings = [];
  const ent = entranceOf(d, placed);
  const A = ent.pos.slice();
  const T = (v) => [v[0] - A[0], v[1] - A[1], v[2] - A[2]];
  const shift = (p) => {
    const o = Object.assign({}, p);
    for (const k of ['min', 'max', 'a', 'b']) if (o[k]) o[k] = T(o[k]);
    if (o.pts) o.pts = o.pts.map(T);
    return o;
  };
  const used = new Set();
  // Collect prims with their sort key: (layer, piece order, prim order).
  const rows = [];
  placed.forEach((pc, pi) => pc.prims.forEach((pr, li) =>
    rows.push({pr, kit: pc.kit, piece: pc.id, pseed: pc.seed, layer: pr.layer !== undefined ? pr.layer : LAYER_OF[pr.op], pi, li})));
  links.forEach((L, k) => {
    if (!L.compat.ok) { errors.push({piece: L.a.piece, msg: 'socket ' + L.a.name + ' cannot join ' + L.b.piece + '.' + L.b.name + ': ' + L.compat.reason}); return; }
    if (!L.compat.taper) return;
    const small = L.a.w * L.a.h >= L.b.w * L.b.h ? L.b : L.a;
    const pcs = placed.find(p => p.id === small.piece);
    const blend = Math.max(...[L.a, L.b].map(s => d.pieces.find(p => p.id === s.piece).params.blend || 0));
    taperPrims(L.a, L.b, blend).forEach((pr, li) => rows.push({pr, kit: pcs.kit, piece: small.piece, pseed: hashN(d.seed, 'taper', k), layer: 0, pi: placed.length + k, li, taper: true}));
  });
  rows.sort((x, y) => x.layer - y.layer || x.pi - y.pi || x.li - y.li);
  // Scatter rules by (piece, local prim).
  const scatterOf = new Map();
  placed.forEach(pc => pc.scatter.forEach((s, si) => {
    const k = pc.id + '#' + s.prim;
    if (!scatterOf.has(k)) scatterOf.set(k, []);
    scatterOf.get(k).push(shellScatter(s, res, pc.kit, hashN(pc.seed, 'scatter', String(s.seedKey), si)));
  }));
  const prims = [];
  for (const row of rows) {
    const p = shift(row.pr);
    const {o, used: u} = toShellPrim(p, res, row.kit, (...k) => hashN(d.seed, ...k));
    u.forEach(m => used.add(m));
    o.piece = row.piece;
    if (p.tag) o.tag = p.tag;
    const sc = row.taper ? [] : (scatterOf.get(row.piece + '#' + row.li) || []);
    sc.forEach(x => used.add(x.mat));
    // At most 4 rules a prim: overflow rides on twins of the same carve (blend 0,
    // so the union and the re-cut are unchanged), placed right after it.
    for (let c = 0; c === 0 || c * MAX_SCATTER_PER_PRIM < sc.length; c++) {
      const part = sc.slice(c * MAX_SCATTER_PER_PRIM, (c + 1) * MAX_SCATTER_PER_PRIM).map(x => x.r);
      const q = c === 0 ? o : Object.assign({}, o, {tag: 'scatter'});
      if (c > 0) delete q.blend;
      if (part.length) q.scatter = part; else delete q.scatter;
      prims.push(q);
    }
  }
  // Props: the pieces' suggestions (unless autoProps:false) + the authored list.
  const templates = [], tIndex = new Map(), instances = [];
  const addProp = (id, propId, params, seed, pos, yaw, kit, piece) => {
    if (!PROPS[propId]) { errors.push({piece, msg: 'unknown prop "' + propId + '"'}); return; }
    const resolve = (m) => { const r = res(kit, m); used.add(r.mat); return r.mat; };
    const tpl = bakeProp(propId, params, seed, resolve);
    if (tpl.size.some(n => n > 256)) { errors.push({piece, msg: 'prop ' + id + ' (' + propId + ') is ' + tpl.size.join('x') + ' cells: a template is at most 256 on an axis'}); return; }
    const key = tpl.id + '|' + JSON.stringify(tpl.params) + '|' + tpl.seed + '|' + tpl.palette.join(',');
    if (!tIndex.has(key)) { tIndex.set(key, templates.length); templates.push(tpl); }
    const t = templates[tIndex.get(key)];
    instances.push({template: tIndex.get(key), pos: instancePos(t, T(pos), yaw), yaw: quarter(yaw) * 90, id, piece});
  };
  placed.forEach((pc, pi) => {
    if (d.pieces[pi].autoProps === false) return;
    pc.props.forEach((pr, k) => addProp(pc.id + '_s' + k, pr.prop, pr.params, hashN(pc.seed, 'prop', k), pr.pos, pr.yaw, pc.kit, pc.id));
  });
  for (const pr of d.props || []) {
    const pc = placed.find(p => p.id === pr.piece);
    addProp(pr.id, pr.prop, pr.params, pr.seed ? pr.seed >>> 0 : hashN(d.seed, 'prop', String(pr.id)), pr.pos || [0, 0, 0], pr.yaw || 0, pc ? pc.kit : d.style, pr.piece || null);
  }
  // Waynodes / refs / sockets (D2 extras for the tuner and the gameplay package).
  const waynodes = buildWaygraph(placed, links).map(w => Object.assign({}, w, {pos: T(w.pos)}));
  const refs = [];
  placed.forEach(pc => pc.refs.forEach(r => {
    const o = Object.assign({}, r, {id: pc.id + '_' + r.name, pos: T(r.pos), piece: pc.id, suggested: true});
    if (o.props && o.props.region) o.props = Object.assign({}, o.props, {region: {min: T(o.props.region.min), max: T(o.props.region.max)}});
    delete o.name;
    refs.push(o);
  }));
  for (const L of links) {
    const tagged = [L.a, L.b].find(s => s.tags && s.tags.includes('door'));
    if (!tagged || !L.compat.ok) continue;
    const yaw = (({'0,0,1': 0, '1,0,0': 90, '0,0,-1': 180, '-1,0,0': 270})[tagged.dir.join(',')]) || 0;
    refs.push({kind: 'door', id: tagged.piece + '_door_' + tagged.name, pos: T(tagged.pos), yaw, piece: tagged.piece, suggested: true,
               props: {width: Math.min(L.a.w, L.b.w), height: Math.min(L.a.h, L.b.h)}});
  }
  if (opts.materials) {
    const known = new Set(opts.materials);
    for (const m of [...used].sort()) if (!known.has(m)) errors.push({piece: null, msg: 'material "' + m + '" is not in materials.json'});
  }
  for (const pc of placed) for (const n of pc.notes) warnings.push({piece: pc.id, msg: n});
  // Limits the engine refuses (3.4.1).
  const pb = prims.map(shellPrimBounds);
  const ib = instances.map(ins => { const t = templates[ins.template], q = quarter(ins.yaw), sx = q & 1 ? t.size[2] : t.size[0], sz = q & 1 ? t.size[0] : t.size[2];
    return {min: ins.pos.slice(), max: [ins.pos[0] + sx - 1, ins.pos[1] + t.size[1] - 1, ins.pos[2] + sz - 1]}; });
  const pc1 = perCellMax(pb), pc2 = perCellMax(ib);
  if (pc1.max > LIMIT_PRIMS_PER_CELL) errors.push({piece: null, msg: pc1.max + ' prims touch one 64^3 cell near ' + pc1.at.join(',') + ' (the engine takes ' + LIMIT_PRIMS_PER_CELL + ')'});
  if (pc2.max > LIMIT_INSTANCES_PER_CELL) errors.push({piece: null, msg: pc2.max + ' prop instances touch one 64^3 cell near ' + pc2.at.join(',') + ' (the engine takes ' + LIMIT_INSTANCES_PER_CELL + ')'});
  pb.forEach((b, i) => { for (let k = 0; k < 3; k++) if (b.max[k] - b.min[k] + 1 > 2048 + 2 * 64) { errors.push({piece: prims[i].piece, msg: 'prim ' + i + ' is over 2048 cells on an axis'}); break; } });
  let bounds = null;
  for (const b of pb.concat(ib)) bounds = boxUnion(bounds, b);
  const kits = {};
  for (const pc of placed) kits[pc.kit] = 1;
  const typeVersions = {};
  for (const pc of placed) typeVersions[pc.kit + '/' + pc.type] = pc.version;
  return {
    format: SHELL_FORMAT, version: SHELL_VERSION, dungeon: opts.id || slugify(d.name),
    sourceHash: sourceHashOf(opts.sourceText !== undefined ? opts.sourceText : dungeonJsonText(dIn)),
    anchor: [0, 0, 0], keepOut: 24,
    naturalCaves: Object.keys(kits).length > 0 && Object.keys(kits).every(k => getKit(k).naturalCaves === 'allow'),
    materials: resolvePalette(d, d.style),
    prims,
    props: {templates: templates.map(t => ({id: t.id, size: t.size, palette: t.palette, voxels: t.voxels})),
            instances: instances.map(i => ({template: i.template, pos: i.pos, yaw: i.yaw, id: i.id, piece: i.piece}))},
    // ---- D2 extras (not read by the engine) ----
    name: d.name, style: d.style, seed: d.seed,
    generator: {tool: 'dungeongen', compiler: COMPILER_VERSION, types: sortObj(typeVersions)},
    entrance: {piece: ent.piece, socket: ent.name, dir: ent.dir.slice(), dungeonPos: A},
    bounds: bounds || {min: [0, 0, 0], max: [0, 0, 0]},
    limits: {primsPerCell: pc1.max, instancesPerCell: pc2.max},
    usedMaterials: [...used].sort(),
    pieces: placed.map(pc => ({id: pc.id, kit: pc.kit, type: pc.type, surface: pc.surface, flooded: pc.flooded,
                               bounds: {min: T(pc.bounds.min), max: T(pc.bounds.max).map(v => v - 1)}, prims: prims.filter(p => p.piece === pc.id).length})),
    waynodes, refs,
    sockets: socketsOf(d, placed).map(s => ({piece: s.piece, name: s.name, pos: T(s.pos), dir: s.dir, w: s.w, h: s.h, profile: s.profile,
                                             tags: s.tags, linked: links.some(L => (L.a === s || L.b === s) && L.compat.ok)})),
    errors, warnings
  };
}

function sortObj(o) { const r = {}; for (const k of Object.keys(o).sort()) r[k] = o[k]; return r; }
function slugify(s) { return String(s || 'dungeon').toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '') || 'dungeon'; }

function utf8(text) { return typeof TextEncoder !== 'undefined' ? new TextEncoder().encode(text) : Buffer.from(text, 'utf8'); }
/** 3.4.1 sourceHash: u32 FNV-1a of dungeon.json's bytes with every '\r' dropped. */
export function sourceHashOf(text) {
  const bytes = utf8(text);
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) { if (bytes[i] === 13) continue; h ^= bytes[i]; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}
/** The same hash as 8 hex digits (display). */
export function hashSource(text) { return sourceHashOf(text).toString(16).padStart(8, '0'); }
/** The canonical on-disk text of a dungeon.json (what the tuner writes). */
export function dungeonJsonText(d) {
  const n = normalizeDungeon(d);
  const J = (v) => JSON.stringify(v);
  const L = ['{'];
  const top = ['version', 'name', 'style', 'seed', 'palette'];
  for (const k of top) L.push('  ' + J(k) + ': ' + J(n[k]) + ',');
  L.push('  "pieces": [');
  n.pieces.forEach((p, i) => L.push('    ' + J(p) + (i + 1 < n.pieces.length ? ',' : '')));
  L.push('  ],');
  L.push('  "entrance": ' + J(n.entrance || null) + ',');
  L.push('  "props": [');
  (n.props || []).forEach((p, i) => L.push('    ' + J(p) + (i + 1 < n.props.length ? ',' : '')));
  L.push('  ]' + (n.detail || n.refs ? ',' : ''));
  const tail = [];
  if (n.detail) tail.push('  "detail": ' + J(n.detail));
  if (n.refs) tail.push('  "refs": ' + J(n.refs));
  if (tail.length) L.push(tail.join(',\n'));
  L.push('}');
  return L.join('\n') + '\n';
}

/** The waynode graph across pieces: names '<piece>/<node>', socket links
 *  ('socket:<name>') replaced by edges to the nodes on the far side of the join. */
export function buildWaygraph(placed, links) {
  const nodes = new Map();
  const sockNodes = new Map();        // piece#socket -> [global node names]
  for (const pc of placed) for (const w of pc.waynodes) {
    const name = pc.id + '/' + w.name;
    const extra = {};
    for (const k of Object.keys(w)) if (!['name', 'pos', 'links'].includes(k)) extra[k] = w[k];
    nodes.set(name, Object.assign({name, pos: w.pos.slice(), links: new Set(), piece: pc.id, sockets: []}, extra));
  }
  for (const pc of placed) for (const w of pc.waynodes) {
    const me = pc.id + '/' + w.name;
    for (const l of w.links) {
      if (l.startsWith('socket:')) {
        const k = pc.id + '#' + l.slice(7);
        if (!sockNodes.has(k)) sockNodes.set(k, []);
        sockNodes.get(k).push(me);
        nodes.get(me).sockets.push(l.slice(7));
      } else if (nodes.has(pc.id + '/' + l)) {
        nodes.get(me).links.add(pc.id + '/' + l);
        nodes.get(pc.id + '/' + l).links.add(me);
      }
    }
  }
  for (const L of links) {
    if (!L.compat.ok) continue;
    const a = sockNodes.get(L.a.piece + '#' + L.a.name) || [], b = sockNodes.get(L.b.piece + '#' + L.b.name) || [];
    for (const x of a) for (const y of b) { nodes.get(x).links.add(y); nodes.get(y).links.add(x); }
  }
  return [...nodes.values()].map(n => {
    const o = Object.assign({}, n, {links: [...n.links].sort()});
    if (!o.sockets.length) delete o.sockets; else o.sockets = [...new Set(o.sockets)].sort();
    return o;
  });
}

// =============================================================================
// the shell files
// =============================================================================
const SHELL_HEAD = ['format', 'version', 'dungeon', 'sourceHash', 'anchor', 'keepOut', 'naturalCaves', 'materials'];
const SHELL_EXTRA = ['name', 'style', 'seed', 'generator', 'entrance', 'bounds', 'limits', 'usedMaterials'];
const SHELL_LISTS = ['pieces', 'waynodes', 'refs', 'sockets', 'errors', 'warnings'];
/** shell.json text: the 3.4.1 shell first (one prim / template / instance a
 *  line, stable key order), then the D2 extras. The ONE place the program's
 *  on-disk text is decided. */
export function emitShellJson(program) {
  const J = (v) => JSON.stringify(v);
  const L = ['{'];
  const list = (k, arr, ind, last) => {
    if (!arr.length) { L.push(ind + J(k) + ': []' + (last ? '' : ',')); return; }
    L.push(ind + J(k) + ': [');
    arr.forEach((o, i) => L.push(ind + '  ' + J(o) + (i + 1 < arr.length ? ',' : '')));
    L.push(ind + ']' + (last ? '' : ','));
  };
  for (const k of SHELL_HEAD) L.push('  ' + J(k) + ': ' + J(program[k]) + ',');
  list('prims', program.prims, '  ', false);
  L.push('  "props": {');
  list('templates', program.props.templates, '    ', false);
  list('instances', program.props.instances, '    ', true);
  L.push('  },');
  for (const k of SHELL_EXTRA) L.push('  ' + J(k) + ': ' + J(program[k]) + ',');
  SHELL_LISTS.forEach((k, i) => list(k, program[k] || [], '  ', i + 1 === SHELL_LISTS.length));
  L.push('}');
  return L.join('\n') + '\n';
}

/** shell.bin per 3.4.1 (little-endian u32 words). -> Uint8Array. */
export function emitShellBin(program) {
  const words = [];
  const strings = [], sIdx = new Map();
  const S = (s) => { if (s === undefined || s === null) return 0xFFFFFFFF; if (!sIdx.has(s)) { sIdx.set(s, strings.length); strings.push(s); } return sIdx.get(s); };
  // Intern every string first so the table precedes its users.
  const mats = Object.keys(program.materials || {}).map(k => [S(k), S(program.materials[k])]);
  const OPS = {carve: 0, fill: 1, skin: 2, keep: 3}, SHAPES = {box: 0, rbox: 1, ellipsoid: 2, cylinder: 3, capsule: 4, ramp: 5, vault: 6, tube: 7};
  const KINDS = {rubble: 0, stalagmite: 1, stalactite: 2, crystal: 3, moss: 4, puddle: 5, bones: 6, ore_vein: 7};
  const SURF = {floor: 0, ceil: 1, wall: 2, any: 3}, AXES = {x: 0, y: 1, z: 2}, DIRS = {'+x': 0, '-x': 1, '+z': 2, '-z': 3};
  const tube = [], scat = [], primW = [];
  for (const p of program.prims) {
    const w = new Array(26).fill(0);
    w[0] = OPS[p.op]; w[1] = SHAPES[p.shape]; w[2] = p.mat !== undefined ? S(p.mat) : 0xFFFFFFFF;
    if (p.shape === 'capsule') { w[3] = p.a[0]; w[4] = p.a[1]; w[5] = p.a[2]; w[6] = p.b[0]; w[7] = p.b[1]; w[8] = p.b[2]; w[9] = p.r; }
    else if (p.shape !== 'tube') { w[3] = p.min[0]; w[4] = p.min[1]; w[5] = p.min[2]; w[6] = p.max[0]; w[7] = p.max[1]; w[8] = p.max[2]; }
    if (p.shape === 'rbox') w[9] = p.r || 0;
    if (p.shape === 'vault') { w[9] = p.rise; w[10] = AXES[p.axis || 'x']; }
    if (p.shape === 'cylinder') w[10] = AXES[p.axis || 'y'];
    if (p.shape === 'ramp') { w[9] = p.stepH; w[10] = DIRS[p.dir]; w[11] = p.stepD; w[12] = p.height || 0; w[13] = p.below || 0; }
    if (p.noise && p.noise.amp) { w[14] = p.noise.amp; w[15] = p.noise.scale; w[16] = p.noise.octaves; w[17] = p.noise.seed >>> 0; }
    w[18] = p.blend || 0; w[19] = p.skin || 0;
    w[20] = p.level !== undefined ? p.level : -2147483648;
    w[21] = p.firm !== undefined ? S(p.firm) : 0xFFFFFFFF;
    if (p.shape === 'tube') { w[22] = tube.length; w[23] = p.pts.length; p.pts.forEach((q, i) => tube.push([q[0], q[1], q[2], p.r[i]])); }
    const sc = p.scatter || [];
    w[24] = sc.length ? scat.length : 0; w[25] = sc.length;
    for (const r of sc) scat.push([KINDS[r.kind], Math.max(0, Math.min(65536, Math.floor(r.density * 65536 + 0.5))), r.size[0], r.size[1], SURF[r.surface], r.mat !== undefined ? S(r.mat) : 0xFFFFFFFF, r.seed >>> 0, r.spacing || 0]);
    primW.push(w);
  }
  const tpls = program.props.templates.map(t => ({id: S(t.id), size: t.size, pal: t.palette.map(S), vox: t.voxels}));
  const insts = program.props.instances.map(i => ({t: i.template, pos: i.pos, q: quarter(i.yaw), id: S(i.id)}));
  const pieceIds = program.prims.map(p => S(p.piece));
  // Write.
  const push = (v) => words.push(v >>> 0);
  push(0x47445653); push(1); push(program.sourceHash >>> 0); push(program.naturalCaves ? 1 : 0);
  program.anchor.forEach(push); push(program.keepOut);
  const bytes = [];
  const flushWords = () => { for (const v of words) bytes.push(v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255); words.length = 0; };
  push(strings.length); flushWords();
  for (const s of strings) {
    const b = utf8(s);
    push(b.length); flushWords();
    for (const x of b) bytes.push(x);
    while (bytes.length & 3) bytes.push(0);
  }
  push(mats.length); for (const [a, b] of mats) { push(a); push(b); }
  push(primW.length); for (const w of primW) w.forEach(push);
  push(tube.length); for (const t of tube) t.forEach(push);
  push(scat.length); for (const c of scat) c.forEach(push);
  push(tpls.length);
  for (const t of tpls) { push(t.id); t.size.forEach(push); push(t.pal.length); t.pal.forEach(push); push(t.vox.length); for (const v of t.vox) v.forEach(push); }
  push(insts.length); for (const i of insts) { push(i.t); i.pos.forEach(push); push(i.q); push(i.id); }
  push(pieceIds.length); pieceIds.forEach(push);
  push(0x21444E45);
  flushWords();
  return Uint8Array.from(bytes);
}

// =============================================================================
// validation (§5.2's panel)
// =============================================================================
/**
 * validate(dungeon, opts) -> [{severity: 'error'|'warn'|'info', code, piece, msg, at?}]
 * opts:
 *   refs      : the dungeon's refs.json entries (locks, keys, signals); also
 *               reads dungeon.refsInline if present.
 *   groundAt  : (x, z) -> ground y, in the ANCHOR frame (the compiled
 *               program's frame). Given, non-surface pieces that come within
 *   minCover  : (default 4) voxels of it are reported as surface breaches.
 *   waterAt   : (x, z) -> water surface y or null (anchor frame); a carve
 *               under standing water within minCover breaches unless the
 *               piece is `flooded`.
 *   materials : material names (unknown names are errors).
 */
export function validate(dIn, opts) {
  opts = opts || {};
  const out = [];
  const add = (severity, code, piece, msg, at) => out.push(Object.assign({severity, code, piece, msg}, at ? {at} : {}));
  let asm;
  try { asm = assemble(dIn); } catch (e) { add('error', 'load', null, e.message); return out; }
  const {d, placed, links, problems} = asm;
  for (const p of problems) add('error', 'link', p.piece, p.msg);
  for (const L of links) {
    if (!L.compat.ok) add('error', 'socket', L.a.piece, L.a.name + ' -> ' + L.b.piece + '.' + L.b.name + ': ' + L.compat.reason);
    else if (L.compat.taper) add('info', 'taper', L.a.piece, L.a.name + ' -> ' + L.b.piece + '.' + L.b.name + ': auto taper (' + L.compat.reason + ')');
    if (L.auto) add('info', 'autolink', L.a.piece, L.a.name + ' meets ' + L.b.piece + '.' + L.b.name + ' (not in links: joined because they coincide)');
  }
  // Duplicate ids.
  const ids = new Set();
  for (const p of d.pieces) { if (ids.has(p.id)) add('error', 'id', p.id, 'duplicate piece id'); ids.add(p.id); }
  // Open sockets.
  const linkedKey = new Set();
  for (const L of links) if (L.compat.ok) { linkedKey.add(L.a.piece + '#' + L.a.name); linkedKey.add(L.b.piece + '#' + L.b.name); }
  const ent = entranceOf(d, placed);
  for (const s of socketsOf(d, placed)) {
    if (linkedKey.has(s.piece + '#' + s.name)) continue;
    if (s.tags && s.tags.includes('surface')) continue;
    add('warn', 'open', s.piece, 'open socket ' + s.name + ' at ' + s.pos.join(',') + ' (facing ' + dirName(s.dir) + '): the passage ends in rock', s.pos);
  }
  if (!d.entrance) add(d.pieces.length ? 'warn' : 'info', 'entrance', null, 'no entrance set; using ' + ent.piece + '.' + ent.name);
  const entPiece = placed.find(p => p.id === ent.piece);
  if (entPiece && !entPiece.surface) add('warn', 'entrance', ent.piece, 'the entrance piece is not a surface piece (no terrain blend apron)');
  for (const pc of placed) for (const n of pc.notes) add('warn', 'piece', pc.id, n);
  // Overlap.
  for (let i = 0; i < placed.length; i++) for (let j = i + 1; j < placed.length; j++) {
    const linked = links.some(L => L.compat.ok && ((L.a.piece === placed[i].id && L.b.piece === placed[j].id) || (L.b.piece === placed[i].id && L.a.piece === placed[j].id)));
    const hit = piecesOverlap(placed[i], placed[j], linked, d);
    if (hit) add('error', 'overlap', placed[i].id, placed[i].id + ' and ' + placed[j].id + ' overlap near ' + hit.join(','), hit);
  }
  // Reachability over the waynode graph.
  const graph = buildWaygraph(placed, links);
  const G = new Map(graph.map(n => [n.name, n]));
  const roots = graph.filter(n => n.piece === ent.piece && !n.pit).map(n => n.name);
  const refs = (opts.refs || []).concat(d.refsInline || []);
  const nearest = (pos) => {
    let best = null, bd = Infinity;
    for (const n of graph) { if (n.pit) continue; const dd = Math.hypot(n.pos[0] - pos[0], n.pos[1] - pos[1], n.pos[2] - pos[2]); if (dd < bd) { bd = dd; best = n.name; } }
    return bd <= 80 ? best : null;
  };
  // Refs are in the ANCHOR frame (refs.json is authored on the compiled frame).
  const A = ent.pos;
  const toD = (p) => [p[0] + A[0], p[1] + A[1], p[2] + A[2]];
  const locks = refs.filter(r => r.kind === 'door' || r.kind === 'gate').filter(r => r.props && r.props.lock);
  const keys = refs.filter(r => r.kind === 'key' && r.props && r.props.keyId);
  const lockNode = new Map(locks.map(r => [r, nearest(toD(r.pos || [0, 0, 0]))]));
  const keyNode = new Map(keys.map(r => [r, nearest(toD(r.pos || [0, 0, 0]))]));
  const reach = (blocked) => {
    const seen = new Set(), q = roots.filter(r => !blocked.has(r));
    q.forEach(r => seen.add(r));
    while (q.length) {
      const n = G.get(q.shift());
      for (const m of n.links) if (!seen.has(m) && !blocked.has(m) && !G.get(m).pit) { seen.add(m); q.push(m); }
    }
    return seen;
  };
  const have = new Set();
  let seen;
  for (let it = 0; it < 32; it++) {
    const blocked = new Set();
    for (const [r, n] of lockNode) if (n && !have.has(r.props.lock)) blocked.add(n);
    seen = reach(blocked);
    let grew = false;
    for (const [r, n] of keyNode) if (n && seen.has(n) && !have.has(r.props.keyId)) { have.add(r.props.keyId); grew = true; }
    if (!grew) break;
  }
  const allOpen = reach(new Set());
  if (graph.length && roots.length) for (const pc of placed) {
    const mine = graph.filter(n => n.piece === pc.id && !n.pit);
    if (!mine.length) { add('warn', 'nav', pc.id, 'no waynodes'); continue; }
    if (!mine.some(n => allOpen.has(n.name))) add('error', 'unreachable', pc.id, pc.id + ' (' + pc.type + ') cannot be reached from the entrance on foot');
  }
  for (const [r, n] of lockNode) {
    const k = r.props.lock;
    if (!keys.some(kr => kr.props.keyId === k)) add('error', 'lock', r.id || null, 'lock "' + k + '" has no key ref');
    else if (!have.has(k)) add('error', 'lock', r.id || null, 'lock "' + k + '": its key cannot be reached without passing the lock');
    if (!n) add('warn', 'lock', r.id || null, 'lock "' + k + '" is not near any waynode');
  }
  for (const kr of keys) if (!locks.some(r => r.props.lock === kr.props.keyId)) add('info', 'key', kr.id || null, 'key "' + kr.props.keyId + '" opens nothing');
  // Signals.
  const sig = (r) => r.signal || (r.props && r.props.signal);
  const on = (r) => r.on || (r.props && r.props.on);
  const emit = new Set(refs.filter(r => (r.kind === 'lever' || r.kind === 'plate') && sig(r)).map(sig));
  const recv = refs.filter(r => on(r) && on(r) !== 'plate' && on(r) !== 'signal');
  for (const r of recv) if (!emit.has(on(r))) add('error', 'signal', r.id || null, r.kind + ' listens for signal "' + on(r) + '" which nothing emits');
  for (const s of emit) if (!recv.some(r => on(r) === s)) add('warn', 'signal', null, 'signal "' + s + '" is emitted but nothing listens');
  // Slopes.
  const done = new Set();
  for (const n of graph) for (const m of n.links) {
    const k = [n.name, m].sort().join('|');
    if (done.has(k)) continue;
    done.add(k);
    const o = G.get(m);
    if (n.climb || o.climb) continue;
    const h = Math.hypot(n.pos[0] - o.pos[0], n.pos[2] - o.pos[2]), dy = Math.abs(n.pos[1] - o.pos[1]);
    if (dy > h * 1.05 + 4) add('warn', 'slope', n.piece, 'slope ' + n.name + ' -> ' + m + ' is ' + Math.round(Math.atan2(dy, h) * 180 / Math.PI) + ' degrees (too steep to walk)', n.pos);
  }
  // Props inside rock: the cell above a prop's origin must be inside some carve.
  const carves = [];
  for (const pc of placed) for (const p of pc.prims) if (p.op === 'carve') carves.push(p);
  const progProps = [];
  placed.forEach((pc, pi) => { if (d.pieces[pi].autoProps !== false) pc.props.forEach((pr, k) => progProps.push({id: pc.id + '_s' + k, piece: pc.id, pos: pr.pos, prop: pr.prop})); });
  for (const pr of d.props || []) progProps.push({id: pr.id, piece: pr.piece, pos: pr.pos || [0, 0, 0], prop: pr.prop});
  for (const pr of progProps) {
    const q = [pr.pos[0] + 0.5, pr.pos[1] + 0.5, pr.pos[2] + 0.5];
    if (!carves.some(c => primContains(c, q))) add('warn', 'prop', pr.piece, 'prop ' + pr.id + ' (' + pr.prop + ') sits inside rock', pr.pos);
  }
  // Surface / water breach (anchor frame).
  if (opts.groundAt || opts.waterAt) {
    const cover = opts.minCover === undefined ? 4 : opts.minCover;
    for (const pc of placed) {
      if (pc.surface) continue;
      let hit = null;
      for (const p of pc.prims) {
        if (p.op !== 'carve' || p.tag === 'apron' || hit) continue;
        const b = shapeBounds(p);
        const amp = p.noise ? p.noise.amp : 0;
        const step = Math.max(4, Math.round(Math.max(b.max[0] - b.min[0], b.max[2] - b.min[2]) / 12));
        for (let x = b.min[0]; x < b.max[0] && !hit; x += step) for (let z = b.min[2]; z < b.max[2] && !hit; z += step) {
          const ax = x - A[0], az = z - A[2];
          const g = opts.groundAt ? opts.groundAt(ax, az) : Infinity;
          const w = opts.waterAt ? opts.waterAt(ax, az) : null;
          const lim = g - cover;
          if (b.max[1] - A[1] + amp < lim && !(w !== null && w > g)) continue;
          // Find the top carved cell of this column.
          let top = null;
          for (let y = b.max[1]; y >= b.min[1]; y--) if (primContains(p, [x + 0.5, y + 0.5, z + 0.5])) { top = y; break; }
          if (top === null) continue;
          const ty = top - A[1] + amp;
          if (ty >= lim) hit = {kind: 'surface', at: [ax, ty, az]};
          else if (w !== null && w > g && ty >= g - cover && !pc.flooded) hit = {kind: 'water', at: [ax, ty, az]};
        }
      }
      if (hit) add('error', hit.kind === 'surface' ? 'breach' : 'water', pc.id, hit.kind === 'surface'
        ? pc.id + ' (' + pc.type + ') breaks the ground surface near ' + hit.at.join(',') + ' (mark it surface: true, or go deeper)'
        : pc.id + ' (' + pc.type + ') comes within ' + cover + ' voxels of a lake / sea bed near ' + hit.at.join(',') + ' (not a flooded piece)', hit.at);
    }
  }
  // What the compiler refuses (materials, engine limits, props) belongs on the panel too.
  const prog = compile(dIn, {materials: opts.materials});
  for (const e of prog.errors) {
    if (/^socket /.test(e.msg) || problems.some(p => p.msg === e.msg)) continue;     // reported above
    add('error', /materials\.json/.test(e.msg) ? 'material' : /64\^3 cell|2048 cells/.test(e.msg) ? 'limit' : 'prop', e.piece, e.msg);
  }
  return out;
}

/** Do two placed pieces' carves intersect beyond tolerance? -> a point or null.
 *  Linked neighbours may overlap round their join (tubes have round ends,
 *  doorways reach past the socket plane) by up to half the opening + blend. */
export function piecesOverlap(A, B, linked, d) {
  const sel = (pc) => pc.prims.filter(p => p.op === 'carve' && p.tag !== 'apron' && p.tag !== 'detail' && p.tag !== 'taper');
  const pa = sel(A), pb = sel(B);
  const bbA = pa.reduce((acc, p) => boxUnion(acc, shapeBounds(p)), null), bbB = pb.reduce((acc, p) => boxUnion(acc, shapeBounds(p)), null);
  if (!bbA || !bbB) return null;
  if (boxOverlap(bbA, bbB).some(v => v <= 0)) return null;
  let joinPts = [];
  // The join zone: a ball round the centre of the shared opening.
  if (linked) for (const s of A.sockets) for (const t of B.sockets) if (eq3(s.pos, t.pos)) {
    const c = isVert(s) ? s.pos : [s.pos[0], s.pos[1] + Math.max(s.h, t.h) / 2, s.pos[2]];
    joinPts.push({pos: c, r: Math.max(s.w, s.h, t.w, t.h) * 0.75 + 10});
  }
  void d;
  for (const p of pa) for (const q of pb) {
    const ov = boxOverlap(shapeBounds(p), shapeBounds(q));
    if (ov.some(v => v <= 1)) continue;
    const lo = [0, 1, 2].map(i => Math.max(shapeBounds(p).min[i], shapeBounds(q).min[i]));
    const hi = [0, 1, 2].map(i => Math.min(shapeBounds(p).max[i], shapeBounds(q).max[i]));
    const st = [0, 1, 2].map(i => Math.max(1, (hi[i] - lo[i]) / 7));
    for (let x = lo[0] + st[0] / 2; x < hi[0]; x += st[0]) for (let y = lo[1] + st[1] / 2; y < hi[1]; y += st[1]) for (let z = lo[2] + st[2] / 2; z < hi[2]; z += st[2]) {
      const pt = [x, y, z];
      if (!primContains(p, pt) || !primContains(q, pt)) continue;
      if (joinPts.some(j => Math.hypot(pt[0] - j.pos[0], pt[1] - j.pos[1], pt[2] - j.pos[2]) <= j.r)) continue;
      return pt.map(Math.round);
    }
  }
  return null;
}

// =============================================================================
// layout assist: grow a branch from a socket
// =============================================================================
function weightedPick(weights, r) {
  const keys = Object.keys(weights).sort();
  let tot = 0;
  for (const k of keys) tot += weights[k];
  let x = r * tot;
  for (const k of keys) { x -= weights[k]; if (x < 0) return k; }
  return keys[keys.length - 1];
}

/** Try one candidate piece onto a world socket. -> {piece, placed, socket} or {why}. */
function tryAttach(d, placed, target, kit, type, params, seed, id, fit) {
  const T = getType(kit, type);
  let piece = normalizePiece({id, kit, type, params, seed, pos: [0, 0, 0], yaw: 0});
  let local = T.generate(piece.params, seed, {vpm: 10});
  const wantVert = target.dir[1] !== 0;
  // Attach by the ORIGIN socket when it can face the target, else the first that can.
  const pickSock = (res) => {
    const can = res.sockets.filter(s => (s.dir[1] !== 0) === wantVert && (!wantVert || s.dir[1] === -target.dir[1]) && !(s.tags || []).includes('surface'));
    return can.find(s => s.pos[0] === 0 && s.pos[1] === 0 && s.pos[2] === 0) || can[0];
  };
  let ls = pickSock(local);
  if (!ls) return {why: type + ' has no socket that can face ' + dirName(target.dir)};
  // Size the piece to the socket when its opening follows width / height.
  if (fit && !wantVert && (Math.abs(ls.w - target.w) > SOCKET_DIRECT_TOL || Math.abs(ls.h - target.h) > SOCKET_DIRECT_TOL)) {
    const p2 = Object.assign({}, piece.params);
    const sch = T.SCHEMA;
    const has = (k) => sch.some(r => r.k === k);
    if (has('doorWidth')) { p2.doorWidth = p2.doorWidth * target.w / Math.max(1, ls.w); p2.doorHeight = p2.doorHeight * target.h / Math.max(1, ls.h); }
    else if (has('mouth')) p2.mouth = p2.mouth * target.w / Math.max(1, ls.w);
    else { if (has('width')) p2.width = p2.width * target.w / Math.max(1, ls.w); if (has('height')) p2.height = p2.height * target.h / Math.max(1, ls.h); }
    const piece2 = normalizePiece(Object.assign({}, piece, {params: T.normalize(p2)}));
    const local2 = T.generate(piece2.params, seed, {vpm: 10});
    const ls2 = pickSock(local2);
    if (ls2) { piece = piece2; local = local2; ls = ls2; }
  }
  const sn = snapTransform(target, ls);
  if (!sn) return {why: 'no quarter turn faces ' + type + '.' + ls.name + ' to the socket'};
  piece.pos = sn.pos; piece.yaw = sn.yaw;
  const pl = placePiece(d, piece, local);
  const ws = pl.sockets.find(s => s.name === ls.name);
  const c = socketCompat(target, ws);
  if (!c.ok) return {why: c.reason};
  for (const other of placed) {
    const hit = piecesOverlap(pl, other, other.id === target.piece, d);
    if (hit) return {why: type + ' would overlap ' + other.id + ' near ' + hit.join(',')};
  }
  piece.links = {[ls.name]: {piece: target.piece, socket: target.name}};
  return {piece, placed: pl, socket: ls.name};
}

/**
 * grow(dungeon, {piece, socket}, {count, kit, branch, seed, types?, close?})
 *   -> proposal {pieces, links, open, notes, seed}
 * Reproducible: the same dungeon + options give the same proposal. Every
 * choice is keyed (seed, step, try), never by call order elsewhere.
 *   count  : pieces to add along the branch (caps excluded)
 *   branch : 0 = a single chain; 1 = every new socket stays a growth front
 *   types  : restrict to these piece types (else the kit's GROW weights)
 *   close  : cap the fronts left open with a dead-end piece (default true)
 */
export function grow(dIn, ref, opts) {
  opts = opts || {};
  const d = normalizeDungeon(dIn);
  const seed = (opts.seed >>> 0) || 1;
  const count = Math.max(0, Math.min(64, opts.count === undefined ? 3 : opts.count | 0));
  const branch = Math.max(0, Math.min(1, opts.branch === undefined ? 0.3 : opts.branch));
  const placed = d.pieces.map(p => placePiece(d, p));
  const start = socketsOf(d, placed).find(s => s.piece === ref.piece && s.name === ref.socket);
  if (!start) return {pieces: [], links: [], open: [], notes: ['no socket ' + ref.piece + '.' + ref.socket], seed};
  const startPiece = d.pieces.find(p => p.id === ref.piece);
  const kit = opts.kit || startPiece.kit || d.style;
  const K = getKit(kit);
  let weights = Object.assign({}, K.GROW);
  if (opts.types && opts.types.length) { weights = {}; for (const t of opts.types) if (K.TYPES[t]) weights[t] = K.GROW[t] || 1; }
  const added = [], notes = [];
  const work = Object.assign({}, d, {pieces: d.pieces.slice()});
  let frontier = [start];
  const toClose = [];
  const idFor = () => nextPieceId(work);
  for (let step = 0; step < count && frontier.length; step++) {
    const pickRand = frand(seed, 'pick', step) < branch;
    const fi = pickRand ? Math.floor(frand(seed, 'pickidx', step) * frontier.length) : frontier.length - 1;
    const target = frontier[fi];
    let ok = null;
    for (let t = 0; t < 10 && !ok; t++) {
      const type = weightedPick(weights, frand(seed, 'type', step, t));
      const T = K.TYPES[type];
      if (!T || T.surface) continue;
      const pnames = Object.keys(T.PRESETS);
      const preset = pnames[Math.floor(frand(seed, 'preset', step, t) * pnames.length)];
      const pseed = hashN(seed, 'pseed', step, t) || 1;
      const r = tryAttach(work, placed, target, kit, type, T.normalize(T.PRESETS[preset].params), pseed, idFor(), true);
      // A dead end mid-branch (no further horizontal socket) would stop the
      // growth early: only the last step may end it.
      const onward = r.piece ? r.placed.sockets.filter(s => s.name !== r.socket && s.dir[1] === 0 && !(s.tags || []).includes('surface')).length : 0;
      if (r.piece && step < count - 1 && frontier.length <= 1 && onward === 0 && t < 9) continue;
      if (r.piece) ok = r; else if (t === 9) notes.push('step ' + step + ': ' + r.why);
    }
    frontier.splice(fi, 1);
    if (!ok) { notes.push('nothing fits at ' + target.piece + '.' + target.name + ' (left open)'); continue; }
    work.pieces.push(ok.piece);
    placed.push(ok.placed);
    added.push(ok.piece);
    const fresh = ok.placed.sockets.filter(s => s.name !== ok.socket && !(s.tags || []).includes('surface'));
    // Keep growing from one new socket; keep the others as fronts with
    // probability `branch`, else they only get closed at the end.
    // Vertical sockets only get closed (nothing in a kit hangs below a socket).
    const horiz = fresh.filter(s => s.dir[1] === 0);
    fresh.filter(s => s.dir[1] !== 0).forEach(s => toClose.push(s));
    horiz.forEach((s, i) => { if (i === horiz.length - 1 || frand(seed, 'keep', step, s.name) < branch) frontier.push(s); else toClose.push(s); });
    // Ran dry early: take a set-aside horizontal socket back as a front.
    if (!frontier.length) { const k = toClose.findIndex(s => s.dir[1] === 0); if (k >= 0) frontier.push(toClose.splice(k, 1)[0]); }
  }
  // Close the fronts that are left.
  const open = [];
  const rest = toClose.concat(frontier);
  if (opts.close !== false) {
    const caps = K.CAPS || [];
    for (let i = 0; i < rest.length; i++) {
      const target = rest[i];
      let ok = null;
      for (let c = 0; c < caps.length && !ok; c++) {
        const T = K.TYPES[caps[c].type];
        const r = tryAttach(work, placed, target, kit, caps[c].type, T.normalize(Object.assign({}, T.PRESETS[Object.keys(T.PRESETS)[0]].params, caps[c].params)), hashN(seed, 'cap', i, c) || 1, idFor(), true);
        if (r.piece) ok = r;
      }
      if (ok) { work.pieces.push(ok.piece); placed.push(ok.placed); added.push(ok.piece); }
      else open.push({piece: target.piece, socket: target.name});
    }
  } else for (const t of rest) open.push({piece: t.piece, socket: t.name});
  return {pieces: added, links: [{piece: ref.piece, socket: ref.socket, to: added.length ? {piece: added[0].id, socket: Object.keys(added[0].links)[0]} : null}].filter(l => l.to),
          open, notes, seed};
}

/** Apply a grow() proposal: append its pieces, add the back-link on the start piece. */
export function applyProposal(dIn, proposal) {
  const d = normalizeDungeon(dIn);
  for (const p of proposal.pieces) d.pieces.push(normalizePiece(p));
  for (const L of proposal.links) {
    const pc = d.pieces.find(p => p.id === L.piece);
    if (pc) pc.links = Object.assign({}, pc.links || {}, {[L.socket]: L.to});
  }
  return d;
}

/** Attach a new piece of (kit, type, preset/params) onto an existing socket. */
export function attachPiece(dIn, ref, spec) {
  const d = normalizeDungeon(dIn);
  const placed = d.pieces.map(p => placePiece(d, p));
  const target = socketsOf(d, placed).find(s => s.piece === ref.piece && s.name === ref.socket);
  if (!target) throw new Error('no socket ' + ref.piece + '.' + ref.socket);
  const T = getType(spec.kit, spec.type);
  const params = T.normalize(Object.assign({}, spec.preset ? presetParams(spec.kit, spec.type, spec.preset) : {}, spec.params || {}));
  const id = spec.id || nextPieceId(d);
  // Generate with the seed the piece will really have (explicit, or derived from its id).
  const eff = spec.seed ? spec.seed >>> 0 : pieceSeed(d, {id, seed: 0});
  const r = tryAttach(d, spec.checkOverlap ? placed : [], target, spec.kit, spec.type, params, eff, id, !!spec.fit);
  if (!r.piece) throw new Error(r.why);
  if (!spec.seed) r.piece.seed = 0;
  d.pieces.push(r.piece);
  const pc = d.pieces.find(p => p.id === ref.piece);
  pc.links = Object.assign({}, pc.links || {}, {[ref.socket]: {piece: r.piece.id, socket: r.socket}});
  return {dungeon: d, piece: r.piece};
}

export { invXformP, strHash, defaultsOf, normalizeParams, fnvHex };
