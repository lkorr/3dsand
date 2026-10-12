/* ui_entities.js — the L4 ENTITIES of a dungeon in the Dungeons tab
 * (docs/PLAN_dungeons.md §3.7 + the D3 amendment, DESIGN.md §16.D3,
 * src/world/refs_dungeon.h): the ref kinds the stitcher places, their props
 * (exact names, defaults as the engine's), refs.json in and out, the
 * inspector form of one ref, and the overlay -- markers, box handles, SIGNAL
 * WIRES from emitters (lever, plate) to receivers (door, gate, trap: props.on),
 * key -> lock threads, trap -> region links and PATROL ROUTES over waynodes.
 *
 * Frame: refs are in the ANCHOR frame (the compiled program's: entrance
 * socket at 0,0,0), which is refs.json's frame (D3: `anchorLocal` = the
 * shell's anchor, [0,0,0]). A box prop `size: [sx, sy, sz]` runs from pos
 * along RIGHT (heading yaw + 90) for sx, UP for sy, FORWARD (heading yaw)
 * for sz -- refs_dungeon.h "THE FRAME OF A BOX".
 */
'use strict';
import {h, sliderPanel} from './ui_core.js';

/* ---- the kinds ------------------------------------------------------------------- */
// f: field list. t = text | int | num | bool | enum (o: options) | pick (src:
// a catalog list) | size3 | mobs | links | region (a size or a ref name).
export const KINDS = {
  npc: {label: 'Mob (npc)', glyph: 'M', col: [1.0, 0.45, 0.45, 1], base: true, d: 'One placed creature: its mob def (base), behaviour profile, schedule (guard / patrol / lair / ambush rows).',
        f: [['behavior', 'pick', 'behaviors'], ['schedule', 'pick', 'schedules'], ['outfit', 'text'], ['weapon', 'text'], ['dialogue', 'text']],
        def: () => ({behavior: 'zombie'})},
  spawner: {label: 'Spawner', glyph: 'S', col: [1.0, 0.3, 0.6, 1], d: 'Keeps `count` creatures alive, drawn from mobs[] by weight -- deterministic by its id. trigger onEnter(<region>) waits for a player in a region.',
            f: [['mobs', 'mobs'], ['count', 'int', 1, 16], ['radius', 'num', 0, 20], ['respawn', 'text'], ['behavior', 'pick', 'behaviors'], ['schedule', 'pick', 'schedules'],
                ['trigger', 'text'], ['outfit', 'text'], ['weapon', 'text']],
            def: () => ({mobs: [{base: 'human', weight: 1}], count: 2, radius: 1.5, respawn: 'never', behavior: 'zombie'})},
  door: {label: 'Door', glyph: 'D', col: [0.95, 0.7, 0.35, 1], d: 'A hinged leaf of door voxels. lock = a keyId (a key item unlocks it); on = a signal works it (then not by hand).',
         f: [['width', 'int', 1, 64], ['height', 'int', 1, 64], ['thickness', 'int', 1, 8], ['hinge', 'enum', ['left', 'right']], ['lock', 'pick', 'keyIds'], ['on', 'pick', 'signals'], ['invert', 'bool'], ['locked', 'bool']],
         def: () => ({width: 9, height: 20})},
  gate: {label: 'Gate (portcullis)', glyph: 'G', col: [0.75, 0.8, 0.9, 1], d: 'Bars that slide up while its signal is on. Hang it from a lintel or the ceiling (lifted bars over air fall as debris).',
         f: [['width', 'int', 1, 128], ['height', 'int', 1, 128], ['thickness', 'int', 1, 8], ['material', 'text'], ['pattern', 'enum', ['bars', 'solid']], ['spacing', 'int', 1, 16], ['speed', 'int', 1, 30],
             ['on', 'pick', 'signals'], ['invert', 'bool']],
         def: () => ({width: 20, height: 30, thickness: 1, material: 'iron', pattern: 'bars', spacing: 3, speed: 2, on: ''})},
  lever: {label: 'Lever', glyph: 'L', col: [1.0, 0.85, 0.3, 1], d: 'Use toggles its signal (default: its own name); pulse > 0 springs back after that many seconds.',
          f: [['signal', 'text'], ['startOn', 'bool'], ['pulse', 'num', 0, 60], ['label', 'text']], def: () => ({})},
  plate: {label: 'Pressure plate', glyph: 'P', col: [1.0, 0.85, 0.3, 1], box: 'size', d: 'ON while a body’s feet are in its box. latch = stays on once pressed (saved).',
          f: [['signal', 'text'], ['size', 'size3'], ['latch', 'bool'], ['who', 'enum', ['any', 'player', 'mob']]], def: () => ({size: [8, 2, 8]})},
  trap: {label: 'Trap', glyph: 'T', col: [1.0, 0.35, 0.2, 1], box: 'region', d: 'Fires on the rising edge of `on` (a signal or a plate name). collapse_floor turns the region’s floor to its powder twin; flood fills it once; fire_vent, spikes, dart.',
         f: [['type', 'enum', ['collapse_floor', 'spikes', 'dart', 'flood', 'fire_vent']], ['on', 'pick', 'signals'], ['region', 'region'], ['once', 'bool'], ['cooldown', 'num', 0, 600],
             ['damage', 'num', 0, 500], ['duration', 'num', 0, 60], ['range', 'int', 1, 1000], ['material', 'text'], ['liquid', 'text'], ['from', 'text'], ['to', 'text']],
         def: () => ({type: 'collapse_floor', on: '', region: [16, 4, 16]})},
  key: {label: 'Key', glyph: 'K', col: [1.0, 0.9, 0.2, 1], d: 'A use takes the key item into the kit. Not drawn: put it on a prop (a pedestal, a corpse) or in a chest’s loot instead.',
        f: [['item', 'pick', 'keyItems'], ['keyId', 'text']], def: () => ({item: 'iron_key'})},
  container: {label: 'Container', glyph: 'C', col: [0.6, 0.85, 0.5, 1], d: 'A chest / bag. loot = a table in assets/loot/ (rolled once on first open, by the ref id).',
              f: [['loot', 'pick', 'loot'], ['title', 'text'], ['lock', 'pick', 'keyIds']], def: () => ({loot: 'cave_cache', title: 'chest'})},
  region: {label: 'Region', glyph: 'R', col: [0.55, 0.75, 1.0, 1], box: 'size', d: 'A named box. Traps and spawners (onEnter) name it.',
           f: [['size', 'size3']], def: () => ({size: [24, 16, 24]})},
  waynode: {label: 'Waynode', glyph: 'w', col: [0.5, 0.8, 1.0, 1], d: 'A point on the walking graph. Patrol routes are lists of these names.',
            f: [['links', 'links']], def: () => ({links: []})},
  marker: {label: 'Marker', glyph: '*', col: [0.8, 0.8, 0.8, 1], d: 'A tagged place a schedule row can name (`at`).', f: [['tags', 'text']], def: () => ({tags: ['post']})}
};
export const KIND_ORDER = ['npc', 'spawner', 'door', 'gate', 'lever', 'plate', 'trap', 'key', 'container', 'region', 'waynode', 'marker'];

/* ---- refs.json -------------------------------------------------------------------- */
export function freshName(refs, base) {
  const have = new Set(refs.map(r => r.name));
  for (let i = 1; ; i++) { const n = base + '_' + i; if (!have.has(n)) return n; }
}
export function newRef(refs, kind, pos, yaw) {
  const K = KINDS[kind];
  const r = {name: freshName(refs, kind), kind, pos: pos.map(Math.round), yaw: ((Math.round((yaw || 0) / 90) * 90) % 360 + 360) % 360, props: K.def()};
  if (K.base) r.base = 'human';
  return r;
}
/** refs.json text -> editor refs (names without the dungeon prefix). */
export function readRefs(text) {
  if (!text) return {about: '', refs: []};
  const j = JSON.parse(text);
  const refs = (j.refs || []).map(r => {
    const id = String(r.id || '');
    const parts = id.split('/');
    const name = parts[0] === 'dungeon' && parts.length >= 3 ? parts.slice(2).join('/') : id;
    const o = {name, kind: r.kind, pos: (r.pos || [0, 0, 0]).slice(), yaw: r.yaw || 0, props: JSON.parse(JSON.stringify(r.props || {}))};
    if (r.base) o.base = r.base;
    return o;
  });
  return {about: j.about || '', refs};
}
/** Editor refs -> refs.json text (D3's group file; one ref a line like the map files). */
export function writeRefs(id, about, refs) {
  const J = JSON.stringify;
  const L = ['{', '  "group": ' + J('dungeon/' + id) + ','];
  if (about) L.push('  "about": ' + J(about) + ',');
  L.push('  "refs": [');
  refs.forEach((r, i) => {
    const o = {id: 'dungeon/' + id + '/' + r.name, kind: r.kind};
    if (r.base) o.base = r.base;
    o.pos = r.pos.map(Math.round); o.yaw = r.yaw || 0;
    const props = clean(r.props);
    if (Object.keys(props).length) o.props = props;
    L.push('    ' + J(o).replace(/,"/g, ', "').replace(/":/g, '": ').replace(/^\{/, '{ ').replace(/\}$/, ' }') + (i + 1 < refs.length ? ',' : ''));
  });
  L.push('  ]', '}');
  return L.join('\n') + '\n';
}
function clean(p) {
  const o = {};
  for (const [k, v] of Object.entries(p || {})) {
    if (v === '' || v === null || v === undefined) continue;
    if (Array.isArray(v) && !v.length && k !== 'links') continue;
    o[k] = v;
  }
  return o;
}

/* ---- semantics the overlay and the validator share ----------------------------------- */
export const signalOf = (r) => (r.kind === 'lever' || r.kind === 'plate') ? (r.props.signal || r.name) : null;
export const listensTo = (r) => (r.kind === 'door' || r.kind === 'gate' || r.kind === 'trap') && r.props.on ? r.props.on : null;
/** The keyId a key ref grants (props.keyId, else its item's `unlocks`). */
export function keyIdOf(r, cat) {
  if (r.props.keyId) return r.props.keyId;
  const k = cat && (cat.keys || []).find(x => x.id === r.props.item);
  return k ? k.unlocks : r.props.item || '';
}
/** Refs as dungeongen.validate reads them (anchor frame, props.keyId / signal). */
export function forValidate(refs, cat) {
  return refs.map(r => {
    const p = Object.assign({}, r.props);
    if (r.kind === 'key') p.keyId = keyIdOf(r, cat);
    const sig = signalOf(r);
    if (sig) p.signal = sig;
    return {id: r.name, kind: r.kind, pos: r.pos, yaw: r.yaw, props: p};
  });
}
/** Problems the core validator does not know (refs_dungeon.h rules). */
export function refProblems(refs, cat, waynodeNames) {
  const out = [];
  const names = new Set(refs.map(r => r.name));
  const seen = new Set();
  for (const r of refs) {
    if (seen.has(r.name)) out.push({severity: 'error', code: 'ref', ref: r.name, msg: 'two refs are named ' + r.name});
    seen.add(r.name);
    if (!/^[A-Za-z0-9_.-]+$/.test(r.name)) out.push({severity: 'error', code: 'ref', ref: r.name, msg: 'ref name "' + r.name + '" (letters, digits, _ . - only)'});
    if (r.kind === 'trap' && typeof r.props.region === 'string' && !names.has(r.props.region)) out.push({severity: 'error', code: 'trap', ref: r.name, msg: 'trap ' + r.name + ' names region "' + r.props.region + '", which is not a ref here'});
    if (r.kind === 'trap' && !r.props.on) out.push({severity: 'warn', code: 'trap', ref: r.name, msg: 'trap ' + r.name + ' has no `on` (a signal or a plate name): it never fires'});
    if (r.kind === 'spawner') {
      const m = /^onEnter\((.*)\)$/.exec(r.props.trigger || '');
      if (m && !names.has(m[1])) out.push({severity: 'error', code: 'spawner', ref: r.name, msg: 'spawner ' + r.name + ' waits on region "' + m[1] + '", which is not a ref here'});
      if (cat && cat.mobs) for (const mb of r.props.mobs || []) if (mb.base && !cat.mobs.includes(mb.base)) out.push({severity: 'error', code: 'spawner', ref: r.name, msg: 'spawner ' + r.name + ': no mob def "' + mb.base + '"'});
    }
    if (r.kind === 'npc' && cat && cat.mobs && r.base && !cat.mobs.includes(r.base)) out.push({severity: 'error', code: 'npc', ref: r.name, msg: r.name + ': no mob def "' + r.base + '"'});
    if ((r.kind === 'npc' || r.kind === 'spawner') && r.props.schedule && cat && cat.schedules && !cat.schedules.includes(r.props.schedule))
      out.push({severity: 'error', code: 'schedule', ref: r.name, msg: r.name + ': no schedule "' + r.props.schedule + '" in assets/schedules/'});
    if (r.kind === 'container' && r.props.loot && cat && cat.loot && !cat.loot.includes(r.props.loot))
      out.push({severity: 'error', code: 'loot', ref: r.name, msg: r.name + ': no loot table "' + r.props.loot + '" in assets/loot/'});
    if (r.kind === 'waynode') for (const l of r.props.links || []) if (!names.has(l) && !(waynodeNames && waynodeNames.has(l)))
      out.push({severity: 'warn', code: 'waynode', ref: r.name, msg: 'waynode ' + r.name + ' links "' + l + '", which is not a waynode here'});
  }
  return out;
}

/* ---- box geometry (refs_dungeon.h BoxOfRef) ------------------------------------------ */
export function frameOf(yaw) {
  const q = ((Math.round((yaw || 0) / 90) % 4) + 4) % 4;
  const F = [[0, 0, 1], [1, 0, 0], [0, 0, -1], [-1, 0, 0]][q];
  const Rt = [[1, 0, 0], [0, 0, -1], [-1, 0, 0], [0, 0, 1]][q];
  return {fwd: F, right: Rt};
}
/** Inclusive cell box of `size` from r.pos in r's frame -> {min, max}. */
export function boxOf(r, size) {
  const {fwd, right} = frameOf(r.yaw);
  const ends = [];
  for (const a of [0, size[0] - 1]) for (const b of [0, size[1] - 1]) for (const c of [0, size[2] - 1])
    ends.push([0, 1, 2].map(i => r.pos[i] + right[i] * a + (i === 1 ? b : 0) + fwd[i] * c));
  return {min: [0, 1, 2].map(i => Math.min(...ends.map(e => e[i]))), max: [0, 1, 2].map(i => Math.max(...ends.map(e => e[i])))};
}
/** The box a ref shows (region / plate size, trap region size, door / gate leaf), or null. */
export function refBox(r, refs) {
  if (r.kind === 'region' || r.kind === 'plate') return boxOf(r, r.props.size || (r.kind === 'plate' ? [8, 2, 8] : [24, 16, 24]));
  if (r.kind === 'trap' && Array.isArray(r.props.region)) return boxOf(r, r.props.region);
  if (r.kind === 'trap' && typeof r.props.region === 'string') { const o = refs.find(x => x.name === r.props.region); return o && o !== r ? refBox(o, refs) : null; }
  if (r.kind === 'door') return boxOf(r, [r.props.width || 9, r.props.height || 20, r.props.thickness || 1]);
  if (r.kind === 'gate') return boxOf(r, [r.props.width || 20, r.props.height || 30, r.props.thickness || 1]);
  return null;
}
/** Which size prop the box handle of r resizes, or null. */
export function sizeKey(r) {
  if (r.kind === 'region' || r.kind === 'plate') return 'size';
  if (r.kind === 'trap' && Array.isArray(r.props.region)) return 'region';
  return null;
}

/* ---- the overlay -------------------------------------------------------------------- */
const C3 = (p, dy) => [p[0] + 0.5, p[1] + (dy || 0.5), p[2] + 0.5];
/** Draw refs: markers, boxes, wires, threads, routes. st: {sel, schedules: name -> parsed, cat, showWires, showRoutes}. */
export function drawRefs(D, refs, st) {
  const byName = new Map(refs.map(r => [r.name, r]));
  for (const r of refs) {
    const K = KINDS[r.kind] || {glyph: '?', col: [1, 1, 1, 1]};
    const sel = st.sel === r.name;
    const b = refBox(r, refs);
    if (b) D.box(b.min, b.max.map(v => v + 1), sel ? [0.4, 0.85, 1, 1] : [K.col[0], K.col[1], K.col[2], 0.55]);
    const {fwd} = frameOf(r.yaw);
    const c = C3(r.pos, 1);
    D.line(c, [c[0] + fwd[0] * 8, c[1], c[2] + fwd[2] * 8], [K.col[0], K.col[1], K.col[2], 0.8]);
    D.marker(C3(r.pos, 2), K.glyph, K.col, {kind: 'ref', name: r.name}, {sel, size: r.kind === 'waynode' ? 4 : 6});
    if (sel || st.labels) D.label(C3(r.pos, 2), r.name, sel ? [0.4, 0.85, 1, 1] : [0.8, 0.8, 0.8, 0.9], {dx: 9});
    if (r.kind === 'waynode') for (const l of r.props.links || []) {
      const o = byName.get(l);
      if (o && o.name > r.name) D.line(C3(r.pos, 1), C3(o.pos, 1), [0.5, 0.8, 1, 0.6]);
    }
  }
  if (st.showWires !== false) {
    // SIGNAL WIRES: emitter -> every receiver of its signal.
    const emit = new Map();
    for (const r of refs) { const s = signalOf(r); if (s) { if (!emit.has(s)) emit.set(s, []); emit.get(s).push(r); } }
    for (const r of refs) {
      const s = listensTo(r);
      if (!s) continue;
      const src = emit.get(s);
      if (!src) { D.label(C3(r.pos, 6), '? ' + s, [1, 0.35, 0.35, 1], {dx: 9}); continue; }
      for (const e of src) wire(D, C3(e.pos, 2), C3(r.pos, 2), [1.0, 0.8, 0.25, 0.95], s);
    }
    for (const [s, list] of emit) if (!refs.some(r => listensTo(r) === s)) for (const e of list) D.label(C3(e.pos, 6), s + ' (nobody listens)', [1, 0.7, 0.3, 0.9], {dx: 9});
    // key -> lock threads
    for (const k of refs.filter(r => r.kind === 'key')) {
      const id = keyIdOf(k, st.cat);
      for (const l of refs.filter(r => (r.kind === 'door' || r.kind === 'container') && r.props.lock && r.props.lock === id))
        wire(D, C3(k.pos, 2), C3(l.pos, 2), [1, 0.92, 0.3, 0.8], 'key ' + id);
    }
    // trap / spawner -> named region
    for (const r of refs) {
      let n = r.kind === 'trap' && typeof r.props.region === 'string' ? r.props.region : null;
      const m = r.kind === 'spawner' && /^onEnter\((.*)\)$/.exec(r.props.trigger || '');
      if (m) n = m[1];
      const o = n && byName.get(n);
      if (o) D.line(C3(r.pos, 2), C3(o.pos, 1), [0.55, 0.75, 1, 0.7]);
    }
  }
  if (st.showRoutes !== false && st.schedules) {
    for (const r of refs) {
      if (r.kind !== 'npc' && r.kind !== 'spawner') continue;
      const sc = r.props.schedule && st.schedules[r.props.schedule];
      if (!sc) continue;
      for (const row of sc.rows || []) {
        const ex = row.extra || {};
        if (row.do !== 'patrol' || !Array.isArray(ex.route)) continue;
        const pts = ex.route.map(n => byName.get(n) || (st.waynodes && st.waynodes.get(n))).filter(Boolean).map(o => C3(o.pos, 1.5));
        for (let i = 0; i + 1 < pts.length; i++) wire(D, pts[i], pts[i + 1], [0.3, 1.0, 0.85, 0.95], i === 0 ? r.name + ' patrol' : null, true);
        if (pts.length > 2 && (ex.mode || 'loop') === 'loop') wire(D, pts[pts.length - 1], pts[0], [0.3, 1.0, 0.85, 0.6], null, true);
        if (pts.length) D.line(C3(r.pos, 2), pts[0], [0.3, 1.0, 0.85, 0.4]);
      }
    }
  }
}
/** A wire: a dashed line (+ an arrowhead + a label at its middle). */
function wire(D, a, b, col, label, arrow) {
  const n = 10;
  for (let i = 0; i < n; i += 2) {
    const t0 = i / n, t1 = (i + 1) / n;
    D.line([0, 1, 2].map(k => a[k] + (b[k] - a[k]) * t0), [0, 1, 2].map(k => a[k] + (b[k] - a[k]) * t1), col);
  }
  if (arrow) {
    const d = [b[0] - a[0], b[2] - a[2]], l = Math.hypot(d[0], d[1]) || 1, u = [d[0] / l * 4, d[1] / l * 4];
    D.line(b, [b[0] - u[0] - u[1] * 0.6, b[1], b[2] - u[1] + u[0] * 0.6], col);
    D.line(b, [b[0] - u[0] + u[1] * 0.6, b[1], b[2] - u[1] - u[0] * 0.6], col);
  }
  if (label) D.label([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + 2, (a[2] + b[2]) / 2], label, col, {dx: 4});
}

/* ---- the inspector form ------------------------------------------------------------- */
/**
 * refForm(r, ctx) -> element. ctx: {cat, refs, signals(), regions(), waynodes(),
 * change(label, fn, key), onRoutePick(row), schedules (cache), loadSchedule(name),
 * saveSchedule(name, doc)}.
 */
export function refForm(r, ctx) {
  const K = KINDS[r.kind];
  const box = h('div', {});
  const field = (label, input, title) => h('div', {class: 'dg-field', title: title || ''}, h('label', {}, label), input);
  const set = (k, v, key) => ctx.change('edit ' + r.name + '.' + k, () => {
    if (v === '' || v === undefined || (typeof v === 'number' && !isFinite(v))) delete r.props[k]; else r.props[k] = v;
  }, key || ('f:' + r.name + ':' + k));
  box.append(h('div', {class: 'dg-hint'}, K ? K.d : 'unknown kind ' + r.kind));
  const nameIn = h('input', {type: 'text', value: r.name, onchange: () => {
    const n = nameIn.value.trim();
    if (!n || n === r.name) return;
    if (ctx.refs.some(x => x.name === n)) { nameIn.value = r.name; return; }
    ctx.change('rename ' + r.name, () => {
      const old = r.name;
      r.name = n;
      for (const o of ctx.refs) {
        for (const k of ['on', 'region', 'signal']) if (o.props[k] === old) o.props[k] = n;
        if (o.kind === 'waynode') o.props.links = (o.props.links || []).map(l => l === old ? n : l);
        if (o.props.trigger === 'onEnter(' + old + ')') o.props.trigger = 'onEnter(' + n + ')';
      }
      ctx.select(n);
    });
  }});
  box.append(field('name', nameIn, 'the ref id is dungeon/<dungeon>/<name>'));
  if (K && K.base) {
    box.append(field('mob (base)', pickSel(ctx.cat.mobs || [], r.base || '', v => ctx.change('base ' + r.name, () => { r.base = v; }), 'any mob def')));
  }
  const yawSel = h('select', {onchange: () => ctx.change('turn ' + r.name, () => { r.yaw = +yawSel.value; })}, [0, 90, 180, 270].map(y => h('option', {value: y}, y + '°')));
  yawSel.value = String(r.yaw || 0);
  const posIn = h('input', {type: 'text', value: r.pos.join(', '), onchange: () => {
    const v = posIn.value.split(/[ ,]+/).map(Number);
    if (v.length === 3 && v.every(isFinite)) ctx.change('move ' + r.name, () => { r.pos = v.map(Math.round); });
  }});
  box.append(field('pos (anchor)', posIn), field('yaw', yawSel, 'heading: 0 = +Z, 90 = +X'));
  for (const [k, t, a, b] of (K ? K.f : [])) {
    const v = r.props[k];
    let inp;
    if (t === 'text') { inp = h('input', {type: 'text', value: Array.isArray(v) ? v.join(', ') : (v ?? '')}); inp.addEventListener('change', () => set(k, k === 'tags' ? inp.value.split(/[ ,]+/).filter(Boolean) : inp.value.trim())); }
    else if (t === 'int' || t === 'num') { inp = h('input', {type: 'number', value: v ?? '', min: a, max: b, step: t === 'int' ? 1 : 0.1}); inp.addEventListener('change', () => set(k, inp.value === '' ? '' : (t === 'int' ? Math.round(+inp.value) : +inp.value))); }
    else if (t === 'bool') { inp = h('input', {type: 'checkbox', checked: !!v}); inp.addEventListener('change', () => set(k, inp.checked)); }
    else if (t === 'enum') { inp = h('select', {}, h('option', {value: ''}, '(default)'), a.map(o => h('option', {value: o}, o))); inp.value = v ?? ''; inp.addEventListener('change', () => set(k, inp.value)); }
    else if (t === 'pick') {
      const list = a === 'signals' ? ctx.signals() : a === 'keyIds' ? keyIds(ctx) : a === 'keyItems' ? (ctx.cat.keys || []).map(x => x.id) : (ctx.cat[a] || []);
      inp = pickSel(list, v ?? '', nv => set(k, nv), a === 'signals' ? 'a signal some lever / plate emits (type a new one in the box)' : '');
      if (a === 'schedules') {
        const row = h('div', {}, inp, h('button', {class: 'dg-mini', title: 'a new schedule file for this creature', onclick: () => ctx.newSchedule(r)}, 'new'));
        inp = row;
      }
    }
    else if (t === 'size3') inp = vec3In(v || [8, 2, 8], nv => set(k, nv));
    else if (t === 'region') {
      const isName = typeof v === 'string';
      const nm = pickSel(ctx.regions(), isName ? v : '', nv => set(k, nv || [16, 4, 16]), 'a region / plate ref, or blank for a size box');
      const sz = vec3In(Array.isArray(v) ? v : [16, 4, 16], nv => set(k, nv));
      sz.style.display = isName ? 'none' : '';
      inp = h('div', {}, nm, sz);
    }
    else if (t === 'mobs') inp = mobsEditor(r, ctx);
    else if (t === 'links') inp = linksEditor(r, ctx);
    box.append(field(k, inp));
  }
  if ((r.kind === 'npc' || r.kind === 'spawner') && r.props.schedule) box.append(scheduleBox(r, ctx));
  return box;
}
function keyIds(ctx) {
  const s = new Set((ctx.cat.keys || []).map(k => k.unlocks));
  for (const r of ctx.refs) if (r.kind === 'key') s.add(keyIdOf(r, ctx.cat));
  return [...s].filter(Boolean).sort();
}
function pickSel(list, cur, on, title) {
  const id = 'dl' + Math.random().toString(36).slice(2);
  const inp = h('input', {type: 'text', value: cur, list: id, title: title || ''});
  const dl = h('datalist', {id}, list.map(o => h('option', {value: o})));
  inp.addEventListener('change', () => on(inp.value.trim()));
  return h('span', {style: 'display:contents'}, inp, dl);
}
function vec3In(v, on) {
  const ins = [0, 1, 2].map(i => h('input', {type: 'number', value: v[i], min: 1, max: 512, step: 1, style: 'width:52px'}));
  ins.forEach(i => i.addEventListener('change', () => on(ins.map(x => Math.max(1, Math.round(+x.value || 1))))));
  return h('span', {style: 'display:flex;gap:3px'}, ins);
}
function mobsEditor(r, ctx) {
  const box = h('div', {});
  const list = r.props.mobs || [];
  list.forEach((m, i) => {
    const b = pickSel(ctx.cat.mobs || [], m.base || '', v => ctx.change('spawner mob', () => { r.props.mobs[i].base = v; }));
    const w = h('input', {type: 'number', value: m.weight ?? 1, min: 0, step: 1, style: 'width:44px', title: 'weight',
                          onchange: () => ctx.change('spawner weight', () => { r.props.mobs[i].weight = Math.max(0, +w.value || 0); })});
    box.append(h('div', {style: 'display:flex;gap:3px;margin:1px 0'}, b, w, h('button', {class: 'dg-mini', onclick: () => ctx.change('spawner mob -', () => { r.props.mobs.splice(i, 1); })}, '×')));
  });
  box.append(h('button', {class: 'dg-mini', onclick: () => ctx.change('spawner mob +', () => { (r.props.mobs = r.props.mobs || []).push({base: 'human', weight: 1}); })}, '+ mob'));
  return box;
}
function linksEditor(r, ctx) {
  const box = h('div', {});
  for (const l of r.props.links || []) box.append(h('span', {class: 'dg-chip'}, l, ' ', h('a', {href: '#', onclick: (e) => { e.preventDefault(); ctx.change('unlink', () => { r.props.links = r.props.links.filter(x => x !== l); }); }}, '×')));
  box.append(pickSel(ctx.waynodes(), '', v => { if (v && v !== r.name) ctx.change('link ' + r.name, () => { r.props.links = [...new Set([...(r.props.links || []), v])]; }); }, 'link to a waynode'));
  return box;
}
/** The creature's schedule, rows editable in place (monster rows: guard / patrol / lair / ambush). */
function scheduleBox(r, ctx) {
  const name = r.props.schedule;
  const box = h('div', {class: 'dg-sec'}, h('div', {class: 'dg-sechead'}, 'schedule ' + name));
  const body = h('div', {class: 'dg-secbody'});
  box.append(body);
  const doc = ctx.schedules[name];
  if (!doc) { body.append(h('div', {class: 'dg-hint'}, 'loading...')); ctx.loadSchedule(name); return box; }
  const ACTS = ['guard', 'patrol', 'lair', 'ambush', 'wander', 'goto', 'sleep', 'work'];
  doc.rows.forEach((row, i) => {
    const act = h('select', {}, ACTS.map(a => h('option', {value: a}, a)));
    act.value = row.do;
    act.addEventListener('change', () => { row.do = act.value; ctx.scheduleEdited(name); });
    const from = h('input', {type: 'text', value: row.from, style: 'width:46px'}), to = h('input', {type: 'text', value: row.to, style: 'width:46px'});
    from.addEventListener('change', () => { row.from = from.value; ctx.scheduleEdited(name); });
    to.addEventListener('change', () => { row.to = to.value; ctx.scheduleEdited(name); });
    const at = h('input', {type: 'text', value: row.at || '', placeholder: 'at', style: 'width:70px', title: 'a waynode / marker tag / ref the row anchors on'});
    at.addEventListener('change', () => { row.at = at.value.trim(); ctx.scheduleEdited(name); });
    const line = h('div', {style: 'display:flex;gap:3px;flex-wrap:wrap;margin:2px 0'}, from, to, act, at);
    if (row.do === 'patrol') {
      row.extra = row.extra || {};
      const route = h('input', {type: 'text', value: (row.extra.route || []).join(', '), style: 'width:100%', title: 'waynode names in order'});
      route.addEventListener('change', () => { row.extra.route = route.value.split(/[ ,]+/).filter(Boolean); ctx.scheduleEdited(name); });
      const mode = h('select', {}, h('option', {value: 'loop'}, 'loop'), h('option', {value: 'pingpong'}, 'pingpong'));
      mode.value = row.extra.mode || 'loop';
      mode.addEventListener('change', () => { row.extra.mode = mode.value; ctx.scheduleEdited(name); });
      line.append(route, mode, h('button', {class: 'dg-mini', title: 'click waynodes in the view, in order; Enter / Esc ends', onclick: () => ctx.onRoutePick(name, i)}, 'pick in view'));
    } else if (row.do === 'lair' || row.do === 'ambush' || row.do === 'guard') {
      const rad = h('input', {type: 'number', value: row.radius ?? '', placeholder: 'radius m', style: 'width:64px', step: 0.5});
      rad.addEventListener('change', () => { row.radius = rad.value === '' ? undefined : +rad.value; ctx.scheduleEdited(name); });
      line.append(rad);
    }
    line.append(h('button', {class: 'dg-mini', onclick: () => { doc.rows.splice(i, 1); ctx.scheduleEdited(name, true); }}, '×'));
    body.append(line);
  });
  body.append(h('div', {class: 'dg-btns'},
    h('button', {onclick: () => { doc.rows.push({from: '00:00', to: '00:00', do: 'guard', at: '', extra: {}}); ctx.scheduleEdited(name, true); }}, '+ row'),
    h('button', {class: ctx.scheduleDirty(name) ? 'dg-pri' : '', onclick: () => ctx.saveSchedule(name)}, 'Save schedule')),
    h('div', {class: 'dg-hint'}, '00:00 → 00:00 is all day. A schedule file is shared by every creature that names it.'));
  return box;
}
export {sliderPanel};
