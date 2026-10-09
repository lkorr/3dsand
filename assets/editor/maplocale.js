/* maplocale.js — LOCALES on the World map page (docs/PLAN_map_v2.md E4).
 *
 * A LOCALE (L1, DESIGN.md §9g) is a hand-made place: map.json `locales[]`
 * {id, name, icon, bounds: {min: [x, z], max: [x, z]} (world voxels,
 * INCLUSIVE), anchor: {at: [x, z], y}, editLayer?}, its ref group
 * refs/<id>.json (pos.y stored RELATIVE to the anchor; the engine adds the
 * ground at anchor.at), and its own edit layer locales/<id>/ground.svedit (v2,
 * ground-relative). This module is both halves of its editor:
 *
 *   WORLD MODE (the map as a whole): every locale's bounds at low opacity and
 *     its pixel-art marker (the E3 icon set, mapvector.js ICONS = worldmap.h
 *     kMarkerIcons) at the anchor; the Locale tool drags the bounds of a NEW
 *     one with the icon picked beside it; a click selects (the Locales panel
 *     edits id / name / icon / bounds / anchor); a DOUBLE-CLICK opens it.
 *
 *   LOCALE MODE (one place, opened): the view frames the bounds, the engine's
 *     own heights of the box are the backdrop (--heightmap of the SAVED files,
 *     at full resolution), and the page's tools are cut down to what a place
 *     is made of -- the Sculpt brush (the map-wide sculpt layer, CLIPPED: no
 *     sample whose bilinear reach leaves the bounds is touched), the
 *     clearing / soften / tree / water site tools (clipped: a box is clamped,
 *     feather included; a lake or a tree outside is refused), the Structure
 *     tool (place from the Structures library, drag to move, turn, delete) and
 *     the Link tool (waynodes: click two to join them, Shift to part them,
 *     click open ground for a new one). Refs are NOT written here: every
 *     change is a list of F8 editor commands (ref.place / ref.move /
 *     ref.delete / ref.link / ref.unlink) POSTed to /api/refs/commands, which
 *     runs the engine's --edit-script -- one authoring surface, and the engine
 *     does the anchor-relative y. Each batch is one page undo step (its
 *     inverse commands). The 3D view (a streaming WorldView of the locale's
 *     MAP, every layer applied as the game applies them) edits voxels inside
 *     the bounds only and saves them to the LOCALE's layer through
 *     /api/worldedit?locale=&map= (stored ground-relative by the engine).
 *     A breadcrumb leads back to the world.
 *
 * map.js owns the page (canvas, view, undo, save, the sculpt layer, the site
 * tools); it hands this module a context and asks it to clip. */

import {ICON_NAMES, iconCanvas} from './mapvector.js';

/* ---- pure helpers (Node-testable: scripts/test_maplocale.mjs) ------------------- */

/** A locale id is its folder and its ref group (worldmap.cpp LocaleIdOk). */
export const LOCALE_ID_RE = /^[a-z0-9_-]+$/;
/** The smallest bounds the Locale tool draws, voxels a side. */
export const LOCALE_MIN_SIDE = 32;
/** The sculpt layer's sample spacing (worldmap.h, map.js SC_SP). */
export const SCULPT_SP = 8;

/** [x0, z0, x1, z1], inclusive, of a locale (or null if it has none). */
export function boundsOf(L) {
  const b = L && L.bounds;
  if (!b || !Array.isArray(b.min) || !Array.isArray(b.max)) return null;
  return [b.min[0], b.min[1], b.max[0], b.max[1]];
}
export function inBounds(L, x, z) {
  const b = boundsOf(L);
  return !!b && x >= b[0] && x <= b[2] && z >= b[1] && z <= b[3];
}
/** May the sculpt brush touch sample (sx, sz)? worldgen.wgsl sculptOctave
 *  reads, for column x, samples x >> 3 and (x >> 3) + 1 -- the offset AND its
 *  gradient (o.gx uses s10 even at fx 0) -- so sample s reaches columns
 *  s*sp - sp .. s*sp + sp - 1 on each axis. Only a sample whose whole reach
 *  is inside the bounds is editable: that is what makes "nothing outside the
 *  bounds changes" exact (every outside column reads only untouched samples),
 *  not approximately true. */
export function sculptSampleInside(L, sx, sz, sp = SCULPT_SP) {
  const b = boundsOf(L);
  if (!b) return false;
  const x = sx * sp, z = sz * sp;
  return x - sp >= b[0] && x + sp - 1 <= b[2] && z - sp >= b[1] && z + sp - 1 <= b[3];
}
/** The sculpt samples column (x, z) reads (offset + gradient), as [sx, sz] pairs. */
export function sculptSamplesOf(x, z, sp = SCULPT_SP) {
  const sx = Math.floor(x / sp), sz = Math.floor(z / sp);
  return [[sx, sz], [sx + 1, sz], [sx, sz + 1], [sx + 1, sz + 1]];
}
/** Clamp a box site ({min, max, feather}) into the bounds, feather included
 *  (the feather eases the ground / thins the trees past the box). Returns true
 *  if anything moved. A box with no room left keeps a 1-voxel minimum. */
export function clampBoxSite(L, s, defFeather) {
  const b = boundsOf(L);
  if (!b || !Array.isArray(s.min) || !Array.isArray(s.max)) return false;
  const before = JSON.stringify([s.min, s.max, s.feather]);
  for (let i = 0; i < 2; i++) {
    const lo = b[i], hi = b[i + 2];
    s.min[i] = Math.max(lo, Math.min(hi, s.min[i]));
    s.max[i] = Math.max(s.min[i], Math.min(hi, s.max[i]));
  }
  const f = s.feather == null ? defFeather : s.feather;
  const room = Math.min(s.min[0] - b[0], s.min[1] - b[1], b[2] - s.max[0], b[3] - s.max[1]);
  if (f > room) s.feather = Math.max(0, room);
  return JSON.stringify([s.min, s.max, s.feather]) !== before;
}
/** Is a disc of radius r round (x, z) inside the bounds? */
export function circleInside(L, x, z, r) {
  const b = boundsOf(L);
  return !!b && x - r >= b[0] && x + r <= b[2] && z - r >= b[1] && z + r <= b[3];
}
/** A local (x, z) offset turned by a structure yaw (quarter turns; 0 faces
 *  +Z, 90 faces +X): world/structures.cpp YawToRot + Rot, as offsets. */
export function rotXZ(yaw, lx, lz) {
  const q = ((Math.round(yaw / 90) % 4) + 4) % 4;
  return q === 0 ? [lx, lz] : q === 1 ? [lz, -lx] : q === 2 ? [-lx, -lz] : [-lz, lx];
}
/** The four world (x, z) corners of a structure's box: struct.json `origin`
 *  is the anchor cell (the ref's pos), `size` the box. Corner 2 -> 3 is the
 *  FRONT edge (local +z, where the door is). */
export function footprint(sj, pos, yaw) {
  const o = sj.origin || [0, 0, 0], s = sj.size || [1, 1, 1];
  return [[-o[0], -o[2]], [s[0] - o[0], -o[2]], [s[0] - o[0], s[2] - o[2]], [-o[0], s[2] - o[2]]]
    .map(([lx, lz]) => { const d = rotXZ(yaw, lx, lz); return [pos[0] + d[0], pos[2] + d[1]]; });
}
/** A slot's world position from a placed copy. */
export function slotWorld(sj, pos, yaw, slot) {
  const o = sj.origin || [0, 0, 0], p = slot.pos || [0, 0, 0];
  const d = rotXZ(yaw, p[0] - o[0], p[2] - o[2]);
  return [pos[0] + d[0], pos[1] + p[1] - o[1], pos[2] + d[1]];
}
/** Even-odd point in polygon (fractional; for hit tests only). */
export function insidePoly(pts, x, z) {
  let inside = false;
  for (let i = 0, k = pts.length - 1; i < pts.length; k = i++) {
    const [ax, az] = pts[k], [bx, bz] = pts[i];
    if ((az > z) === (bz > z)) continue;
    if (x < ax + (bx - ax) * (z - az) / (bz - az)) inside = !inside;
  }
  return inside;
}
/** A ref link names an id, or a NAME in the ref's own group (refs.h). */
export function linkTarget(group, l) { return String(l).includes('/') ? String(l) : group + '/' + l; }
/** What the engine's loader would refuse in locales[], or null. */
export function localeProblems(json) {
  const out = [], seen = new Set();
  for (const l of json.locales || []) {
    if (!l || typeof l.id !== 'string' || !LOCALE_ID_RE.test(l.id)) { out.push(`a locale id must be [a-z0-9_-]+ (it is a folder and a ref group): "${l && l.id}"`); continue; }
    if (seen.has(l.id)) out.push(`two locales are called "${l.id}"`);
    seen.add(l.id);
    const b = boundsOf(l);
    if (!b || b[2] < b[0] || b[3] < b[1]) out.push(`locale "${l.id}" needs bounds with min <= max`);
    const a = l.anchor;
    if (!a || !Array.isArray(a.at) || a.at.length !== 2 || !Number.isInteger(a.y)) out.push(`locale "${l.id}" needs anchor {at: [x, z], y}`);
    if (l.editLayer != null && (typeof l.editLayer !== 'string' || !l.editLayer || /[\/\\:.]/.test(l.editLayer.replace(/\.svedit$/, ''))))
      out.push(`locale "${l.id}": editLayer is a file name under locales/${l.id}/, not a path`);
  }
  return out.length ? out : null;
}
/** A fresh locale id `<base>_<n>`. */
export function freshLocaleId(locales, base = 'locale') {
  let n = (locales || []).length, id = base + '_' + n;
  while ((locales || []).some(l => l.id === id)) id = base + '_' + (++n);
  return id;
}
/** The fields of a locale the engine reads (what a save must have written
 *  before refs or voxels can be authored against it). */
export function localeKey(L) {
  return L ? JSON.stringify([L.id, L.bounds && L.bounds.min, L.bounds && L.bounds.max, L.anchor && L.anchor.at, L.anchor && L.anchor.y, L.editLayer || '']) : '';
}

/* ---- the layer ---------------------------------------------------------------------- */
/**
 * The locale layer of one map page. `C` is the page:
 *   map(), view, SX(x), SZ(z), worldAtF(ev), fitBounds(x0, z0, x1, z1)
 *   pushUndo(e), snapshot(), markDirty(), paint(), status(t), toast(t, bad), el(...)
 *   vpm(), check(), savedJson(), tool(), setTool(t), hover(), groundAt(x, z),
 *   planeAt(x, z) (the authored plane), refreshHeights(), previewSeed(),
 *   modeChanged() (the page re-syncs its tools / panels), isVisible()
 */
export function createLocaleLayer(C) {
  const st = {
    sel: null,          // world mode: the selected locale's id
    open: null,         // the opened locale's id (locale mode), or null
    newIcon: 'village',
    drag: null,         // world: {x0, z0, x1, z1} a new locale's bounds; locale: {id, sx, sz, dx, dz}
    refs: [],           // the open locale's group, as stored (y relative)
    refsFor: '',        // map/id the refs were read for
    refsErr: '',
    structs: new Map(), // base -> struct.json | null (missing) | 'loading'
    library: [],        // structure names (assets/structures, samples/)
    place: {base: '', yaw: 0, name: ''},
    selRef: null,       // the selected ref id (structure / waynode)
    linkFrom: null,     // Link tool: the first waynode
    busy: false,
    lastResult: '',
    saved: null,        // the 3D view's last voxel save {bytes, deleted}
    prevView: null,     // the world view to return to
    prevTool: null,
  };
  const els = {};
  const J = () => C.map().json;
  const locales = () => (C.map() ? (J().locales || []) : []);
  const find = id => locales().find(l => l.id === id) || null;
  const openL = () => (st.open ? find(st.open) : null);
  const px = d => d * C.view.scale;
  const fmtM = m => (Math.abs(m) >= 1000 ? (m / 1000).toFixed(2) + ' km' : (Math.abs(m) >= 100 ? m.toFixed(0) : m.toFixed(1)) + ' m');
  const v = () => C.vpm();
  function edit(fn) { C.pushUndo(C.snapshot()); fn(); C.markDirty(true); sync(); C.paint(); }

  /* ---- clipping (asked by map.js; true everywhere in world mode) -------------- */
  const clipping = () => !!openL();
  function sampleAllowed(sx, sz) { const L = openL(); return !L || sculptSampleInside(L, sx, sz); }
  function pointAllowed(x, z) { const L = openL(); return !L || inBounds(L, x, z); }
  function circleAllowed(x, z, r) { const L = openL(); return !L || circleInside(L, x, z, r); }
  function clipBox(s, defFeather) { const L = openL(); return L ? clampBoxSite(L, s, defFeather) : false; }
  /** A box site wholly inside the open locale (or any site, in world mode). */
  function boxEditable(s) {
    const L = openL();
    return !L || (inBounds(L, s.min[0], s.min[1]) && inBounds(L, s.max[0], s.max[1]));
  }

  /* ---- the engine's view of the saved locale (--mapcheck) ---------------------- */
  function checkEntry(id) {
    const ck = C.check && C.check();
    return ck && ck.ok && Array.isArray(ck.locales) ? ck.locales.find(e => e.id === id) || null : null;
  }
  /** What a ref of the locale's group adds to its FILE y (LocaleDef::RefYOffset). */
  function refYOffset(L) {
    const e = checkEntry(L.id);
    return e ? e.groundY : L.anchor.y;
  }
  /** Is the open locale in the map.json on disk exactly as the page has it?
   *  The engine converts refs and voxels through the SAVED one. */
  function savedAsShown(L) {
    const sj = C.savedJson && C.savedJson();
    const s = sj && (sj.locales || []).find(x => x.id === L.id);
    return !!s && localeKey(s) === localeKey(L);
  }
  function needSaved(L, what) {
    if (savedAsShown(L)) return true;
    C.toast(`save the map first: ${what} of locale "${L.id}" go through the SAVED map.json (refs are stored relative to its anchor, the voxel layer is applied by its id)`, true);
    return false;
  }

  /* ---- refs: read through /api/refs, written through the F8 command layer ---------- */
  async function loadRefs() {
    const L = openL();
    if (!L) return;
    const name = C.map().name, key = name + '/' + L.id;
    try {
      const r = await fetch('/api/refs?name=' + encodeURIComponent(name), {cache: 'no-store'});
      const j = await r.json();
      if (!j.ok) throw new Error(j.error || 'HTTP ' + r.status);
      const g = (j.groups || {})[L.id];
      st.refs = g && Array.isArray(g.refs) ? g.refs : [];
      st.refsErr = g && g.error ? g.error : '';
    } catch (e) { st.refs = []; st.refsErr = e.message; }
    st.refsFor = key;
    for (const r of st.refs) if (r.kind === 'structure' && r.base) loadStruct(r.base);
    if (st.selRef && !st.refs.some(r => r.id === st.selRef)) st.selRef = null;
    sync(); C.paint();
  }
  async function loadStruct(base) {
    if (st.structs.has(base)) return st.structs.get(base);
    st.structs.set(base, 'loading');
    try {
      const r = await fetch('/api/model?path=structures/' + base.split('/').map(encodeURIComponent).join('/') + '.struct.json', {cache: 'no-store'});
      st.structs.set(base, r.ok ? await r.json() : null);
    } catch (e) { st.structs.set(base, null); }
    C.paint();
    return st.structs.get(base);
  }
  async function loadLibrary() {
    try {
      const r = await fetch('/api/models', {cache: 'no-store'});
      const j = await r.json();
      st.library = [...new Set((j.files || [])
        .filter(f => (f.dir === 'structures' || f.dir === 'structures/samples') && f.name.endsWith('.struct.json'))
        .map(f => (f.dir === 'structures' ? '' : 'samples/') + f.name.slice(0, -'.struct.json'.length)))].sort();
    } catch (e) { st.library = []; }
    if (!st.library.includes(st.place.base)) st.place.base = st.library[0] || '';
    if (st.place.base) loadStruct(st.place.base);
    sync();
  }
  /** Run editor commands on the map (all or nothing, the engine writes the
   *  files); `inverse` makes it one page undo step. */
  async function run(cmds, label, inverse) {
    const L = openL();
    if (!L || st.busy) return false;
    if (!needSaved(L, 'refs')) return false;
    st.busy = true;
    C.status(label + '…');
    let j;
    try {
      const r = await fetch('/api/refs/commands?name=' + encodeURIComponent(C.map().name), {method: 'POST', body: JSON.stringify({commands: cmds})});
      j = await r.json();
    } catch (e) { j = {ok: false, error: e.message}; }
    st.lastResult = j.ok ? String(j.result || 'ok') : String(j.error || '?');
    if (!j.ok) C.toast(label + ' refused by the engine: ' + st.lastResult.slice(0, 300), true);
    else {
      if (inverse) C.pushUndo({refs: {undo: inverse, redo: cmds, label}});
      C.status(label + ' — ' + st.lastResult);
    }
    await loadRefs();
    st.busy = false;   // only now: a caller waiting on it sees the refs as they are
    sync();
    return !!j.ok;
  }
  /** Replay an undo / redo entry's commands (no new undo step). */
  function replay(cmds, label) {
    if (!openL()) { C.toast('open the locale to undo its refs (' + label + ')', true); return; }
    st.busy = false;
    run(cmds, (label || 'refs') + ' (undo/redo)', null);
  }
  /** A ref back as the command that places it, world y (for undoing a delete). */
  function placeCmdOf(ref, L) {
    const a = {id: ref.id, kind: ref.kind, pos: [ref.pos[0], ref.pos[1] + refYOffset(L), ref.pos[2]]};
    if (ref.base) a.base = ref.base;
    if (ref.yaw != null) a.yaw = ref.yaw;
    if (ref.props) a.props = ref.props;
    return {cmd: 'ref.place', args: a};
  }
  function refById(id) { return st.refs.find(r => r.id === id) || null; }
  function structRefs() { return st.refs.filter(r => r.kind === 'structure'); }
  /** Every waynode the Link tool can click: the group's own and every placed
   *  house's slot waynodes ({id, x, z, slot}). */
  function waynodes() {
    const out = [];
    for (const r of st.refs) if (r.kind === 'waynode' && Array.isArray(r.pos)) out.push({id: r.id, x: r.pos[0], z: r.pos[2], ref: r});
    for (const r of structRefs()) {
      const sj = st.structs.get(r.base);
      if (!sj || sj === 'loading') continue;
      for (const s of sj.slots || []) if (s.kind === 'waynode') {
        const w = slotWorld(sj, r.pos, r.yaw || 0, s);
        out.push({id: r.id + '/' + s.name, x: w[0], z: w[2], slot: true, links: ((s.props || {}).links || []).map(l => r.id + '/' + l)});
      }
    }
    return out;
  }
  function waynodeAt(wx, wz) {
    let best = null, bd = 8;
    for (const w of waynodes()) { const d = px(Math.hypot(w.x - wx, w.z - wz)); if (d <= bd) { bd = d; best = w; } }
    return best;
  }
  function structureAt(wx, wz) {
    const rs = structRefs();
    for (let i = rs.length - 1; i >= 0; i--) {
      const sj = st.structs.get(rs[i].base);
      if (sj && sj !== 'loading' && insidePoly(footprint(sj, rs[i].pos, rs[i].yaw || 0), wx, wz)) return rs[i];
    }
    return null;
  }
  function freshRefId(L, stem) {
    const clean = String(stem || 'ref').toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '') || 'ref';
    let id = L.id + '/' + clean, n = 1;
    while (st.refs.some(r => r.id === id)) id = L.id + '/' + clean + '_' + (n++);
    return id;
  }
  function floorAt(x, z) { const g = C.groundAt(x, z); return g == null ? null : Math.round(g) + 1; }

  /* ---- the actions -------------------------------------------------------------- */
  async function placeStructure(wx, wz) {
    const L = openL();
    if (!st.place.base) { C.toast('pick a blueprint from the Structures library first', true); return; }
    const sj = await loadStruct(st.place.base);
    if (!sj) { C.toast('structures/' + st.place.base + '.struct.json did not load', true); return; }
    const pos = [Math.round(wx), 0, Math.round(wz)];
    if (!footprint(sj, pos, st.place.yaw).every(p => inBounds(L, p[0], p[1]))) { C.toast('the house would stand outside the locale\'s bounds: place it further in', true); return; }
    const y = floorAt(pos[0], pos[2]);
    if (y == null) { C.toast('the floor height comes from the engine\'s heights of this locale: they are still loading — click again in a moment', true); C.refreshHeights(); return; }
    pos[1] = y;
    const id = freshRefId(L, st.place.name || st.place.base.split('/').pop());
    const cmd = {cmd: 'ref.place', args: {id, kind: 'structure', base: st.place.base, pos, yaw: st.place.yaw}};
    if (await run([cmd], 'placed ' + id, [{cmd: 'ref.delete', args: {id}}])) st.selRef = id;
  }
  function moveStructure(ref, dx, dz) {
    const L = openL(), sj = st.structs.get(ref.base);
    const pos = [ref.pos[0] + dx, ref.pos[1], ref.pos[2] + dz];
    if (sj && sj !== 'loading' && !footprint(sj, pos, ref.yaw || 0).every(p => inBounds(L, p[0], p[1]))) {
      C.toast('the house would leave the locale\'s bounds: not moved', true); C.paint(); return;
    }
    // the floor follows the ground: the change of the engine's ground between the two spots
    const g0 = C.groundAt(ref.pos[0], ref.pos[2]), g1 = C.groundAt(pos[0], pos[2]);
    const dy = g0 != null && g1 != null ? Math.round(g1) - Math.round(g0) : 0;
    run([{cmd: 'ref.move', args: {id: ref.id, by: [dx, dy, dz]}}], 'moved ' + ref.id,
        [{cmd: 'ref.move', args: {id: ref.id, by: [-dx, -dy, -dz]}}]);
  }
  function turnStructure(ref, by) {
    const yaw = (((ref.yaw || 0) + by) % 360 + 360) % 360;
    const L = openL(), sj = st.structs.get(ref.base);
    if (sj && sj !== 'loading' && !footprint(sj, ref.pos, yaw).every(p => inBounds(L, p[0], p[1]))) {
      C.toast('turned, the house would leave the locale\'s bounds', true); return;
    }
    run([{cmd: 'ref.move', args: {id: ref.id, yaw}}], 'turned ' + ref.id + ' to ' + yaw + '°',
        [{cmd: 'ref.move', args: {id: ref.id, yaw: ref.yaw || 0}}]);
  }
  function deleteRef(ref) {
    run([{cmd: 'ref.delete', args: {id: ref.id}}], 'deleted ' + ref.id, [placeCmdOf(ref, openL())]);
    if (st.selRef === ref.id) st.selRef = null;
  }
  function linked(a, b) {
    const has = (from, to) => {
      const r = refById(from);
      const g = from.split('/')[0];
      return !!r && ((r.props || {}).links || []).some(l => linkTarget(g, l) === to);
    };
    return has(a, b) || has(b, a);
  }
  function linkPair(a, b, unlink) {
    if (unlink) run([{cmd: 'ref.unlink', args: {a, b}}], 'parted ' + a + ' / ' + b, [{cmd: 'ref.link', args: {a, b}}]);
    else run([{cmd: 'ref.link', args: {a, b}}], 'linked ' + a + ' — ' + b, [{cmd: 'ref.unlink', args: {a, b}}]);
  }
  async function newWaynode(wx, wz) {
    const L = openL();
    const y = floorAt(wx, wz);
    if (y == null) { C.toast('a waynode stands on the engine\'s ground: the heights are still loading', true); C.refreshHeights(); return; }
    const id = freshRefId(L, 'way');
    const cmds = [{cmd: 'ref.place', args: {id, kind: 'waynode', pos: [Math.round(wx), y, Math.round(wz)]}}];
    const inv = [{cmd: 'ref.delete', args: {id}}];
    if (st.linkFrom) {
      // the link is written on the FIRST waynode: undo parts it before the delete
      cmds.push({cmd: 'ref.link', args: {a: st.linkFrom, b: id}});
      inv.unshift({cmd: 'ref.unlink', args: {a: st.linkFrom, b: id}});
    }
    if (await run(cmds, 'new waynode ' + id + (st.linkFrom ? ' linked to ' + st.linkFrom : ''), inv)) st.linkFrom = id;
  }

  /* ---- world mode: open / leave ------------------------------------------------- */
  function enter(id, opts) {
    const L = find(id);
    if (!L) return false;
    st.prevView = {cx: C.view.cx, cz: C.view.cz, scale: C.view.scale};
    st.prevTool = C.tool();
    st.open = id; st.sel = id;
    st.refs = []; st.refsFor = ''; st.selRef = null; st.linkFrom = null; st.drag = null;
    const b = boundsOf(L);
    C.fitBounds(b[0], b[1], b[2] + 1, b[3] + 1);
    C.setTool('sculpt');
    C.modeChanged();
    loadRefs();
    if (!st.library.length) loadLibrary();
    C.refreshHeights();
    if (!opts || opts.view3d !== false) view3d.open(L);   // (a test seam can skip the 3D view)
    sync(); C.paint();
    C.status(`locale “${L.name || L.id}”: ${(b[2] - b[0] + 1) / v()} × ${(b[3] - b[1] + 1) / v()} m — sculpt and site tools are clipped to its bounds; Structure places houses, Link joins waynodes; the 3D view edits its own voxel layer`);
    return true;
  }
  function leave() {
    if (!st.open) return;
    view3d.close();
    st.open = null; st.drag = null; st.linkFrom = null; st.selRef = null;
    if (st.prevView) Object.assign(C.view, st.prevView);
    C.setTool(st.prevTool && !['structure', 'waylink'].includes(st.prevTool) ? st.prevTool : 'locale');
    C.modeChanged();
    sync(); C.paint();
  }

  /* ---- hit tests (world mode) --------------------------------------------------- */
  function markerAt(fx, fz) {
    const ls = locales();
    for (let i = ls.length - 1; i >= 0; i--) {
      const a = ls[i].anchor && ls[i].anchor.at;
      if (!Array.isArray(a)) continue;
      const dx = C.SX(fx) - C.SX(a[0]), dz = C.SZ(fz) - C.SZ(a[1]);
      if (Math.abs(dx) <= 14 && dz >= -31 && dz <= 4) return ls[i];
    }
    return null;
  }
  function localeAt(fx, fz) {
    const m = markerAt(fx, fz);
    if (m) return m;
    const ls = locales();
    for (let i = ls.length - 1; i >= 0; i--) if (inBounds(ls[i], fx, fz)) return ls[i];
    return null;
  }
  function newLocale(x0, z0, x1, z1) {
    const ls = J().locales || (J().locales = []);
    const id = freshLocaleId(ls, 'locale');
    const ax = Math.round((x0 + x1) / 2), az = Math.round((z0 + z1) / 2);
    // anchor.y: the engine's ground there when the backdrop has it, else the
    // authored plane; --mapcheck reports the real one after the save (and the
    // panel offers to take it: the delta then reads 0).
    const g = C.groundAt(ax, az);
    const y = Math.round(g != null ? g : C.planeAt(ax, az));
    const L = {id, name: 'Locale ' + (ls.length + 1), icon: st.newIcon, bounds: {min: [x0, z0], max: [x1, z1]}, anchor: {at: [ax, az], y}};
    edit(() => { ls.push(L); st.sel = id; });
    return L;
  }

  /* ---- events from map.js ------------------------------------------------------------ */
  function pointerDown(ev) {
    if (!C.map()) return false;
    const t = C.tool();
    const [fx, fz] = C.worldAtF(ev), wx = Math.round(fx), wz = Math.round(fz);
    if (!st.open) {
      // a locale's badge is a button whatever the tool: select (a double-click opens)
      const badgeHit = markerAt(fx, fz);
      if (badgeHit && !ev.ctrlKey) { st.sel = badgeHit.id; sync(); C.paint(); return true; }
      if (t !== 'locale') return false;
      // Ctrl+click draws a new one even on top of another
      const hit = ev.ctrlKey || ev.metaKey ? null : localeAt(fx, fz);
      if (hit) { st.sel = hit.id; sync(); C.paint(); return true; }
      st.drag = {kind: 'new', x0: wx, z0: wz, x1: wx, z1: wz};
      return true;
    }
    const L = openL();
    if (t === 'structure') {
      const hit = structureAt(fx, fz);
      if (hit && ev.shiftKey) { deleteRef(hit); return true; }
      if (hit) { st.selRef = hit.id; st.drag = {kind: 'move', id: hit.id, sx: wx, sz: wz, dx: 0, dz: 0}; sync(); C.paint(); return true; }
      if (!inBounds(L, wx, wz)) { C.status('outside the locale: nothing is placed here'); return true; }
      placeStructure(fx, fz);
      return true;
    }
    if (t === 'waylink') {
      const w = waynodeAt(fx, fz);
      if (w && ev.shiftKey && !st.linkFrom && w.ref) { deleteRef(w.ref); return true; }
      if (w) {
        if (!st.linkFrom) { st.linkFrom = w.id; st.selRef = w.ref ? w.id : st.selRef; }
        else if (st.linkFrom === w.id) st.linkFrom = null;
        else {
          const a = st.linkFrom, b = w.id;
          const unlink = ev.shiftKey || linked(a, b);
          linkPair(a, b, unlink);
          st.linkFrom = unlink ? null : b;
        }
        sync(); C.paint();
        return true;
      }
      if (!inBounds(L, wx, wz)) { C.status('outside the locale'); return true; }
      newWaynode(fx, fz);
      return true;
    }
    return false;
  }
  function pointerMove(ev) {
    const d = st.drag;
    if (!d || !(ev.buttons & 1)) return false;
    const [fx, fz] = C.worldAtF(ev), wx = Math.round(fx), wz = Math.round(fz);
    if (d.kind === 'new') { d.x1 = wx; d.z1 = wz; }
    else { d.dx = wx - d.sx; d.dz = wz - d.sz; }
    C.paint();
    return true;
  }
  function pointerUp() {
    const d = st.drag;
    if (!d) return false;
    st.drag = null;
    if (d.kind === 'new') {
      const x0 = Math.min(d.x0, d.x1), x1 = Math.max(d.x0, d.x1), z0 = Math.min(d.z0, d.z1), z1 = Math.max(d.z0, d.z1);
      if (x1 - x0 + 1 < LOCALE_MIN_SIDE || z1 - z0 + 1 < LOCALE_MIN_SIDE) {
        C.toast(`drag out the locale's bounds (at least ${LOCALE_MIN_SIDE} voxels = ${LOCALE_MIN_SIDE / v()} m a side); click a locale to select it, double-click to open it`);
        C.paint();
        return true;
      }
      const L = newLocale(x0, z0, x1, z1);
      C.status(`new locale “${L.name}” (${L.id}): ${(x1 - x0 + 1) / v()} × ${(z1 - z0 + 1) / v()} m — name it in the Locales panel, save the map, double-click it to open`);
    } else if (d.kind === 'move' && (d.dx || d.dz)) {
      const ref = refById(d.id);
      if (ref) moveStructure(ref, d.dx, d.dz);
    }
    C.paint();
    return true;
  }
  function dblclick(ev) {
    if (st.open || !C.map()) return false;
    const [fx, fz] = C.worldAtF(ev);
    const hit = markerAt(fx, fz) || (['locale', 'site'].includes(C.tool()) ? localeAt(fx, fz) : null);
    if (!hit) return false;
    enter(hit.id);
    return true;
  }
  function key(ev) {
    if (!st.open) return false;
    if (ev.key === 'Escape' && st.linkFrom) { st.linkFrom = null; C.paint(); return true; }
    const ref = st.selRef && refById(st.selRef);
    if (!ref) return false;
    if ((ev.key === 'r' || ev.key === 'R') && ref.kind === 'structure') { turnStructure(ref, ev.shiftKey ? -90 : 90); return true; }
    if (ev.key === 'Delete') { deleteRef(ref); return true; }
    return false;
  }
  function afterRestore() {
    if (st.open && !find(st.open)) leave();
    if (st.sel && !find(st.sel)) st.sel = null;
    st.drag = null;
    sync();
  }
  function reset() {
    if (st.open) { view3d.close(); st.open = null; C.modeChanged(); }
    st.sel = null; st.drag = null; st.refs = []; st.refsFor = ''; st.selRef = null; st.linkFrom = null;
    sync();
  }

  /* ---- painting ------------------------------------------------------------------------ */
  function badge(g, L, sel) {
    const a = L.anchor && L.anchor.at;
    if (!Array.isArray(a)) return;
    const x = Math.round(C.SX(a[0])), z = Math.round(C.SZ(a[1]));
    const ring = sel ? '#ffd866' : '#7fd4ff';
    g.fillStyle = ring; g.fillRect(x - 14, z - 30, 28, 28);
    g.fillStyle = '#0e1219'; g.fillRect(x - 13, z - 29, 26, 26);
    g.fillStyle = ring; g.fillRect(x - 1, z - 2, 3, 3); g.fillRect(x, z - 3, 1, 3);
    g.drawImage(iconCanvas(L.icon), x - 12, z - 28, 24, 24);
    g.fillStyle = sel ? '#ffd866' : '#cfeeff';
    g.fillText(L.name || L.id, x + 17, z - 12);
  }
  function paintWorld(g) {
    for (const L of locales()) {
      const b = boundsOf(L);
      if (!b) continue;
      const sel = st.sel === L.id;
      const x0 = C.SX(b[0]), z0 = C.SZ(b[1]), w = (b[2] - b[0] + 1) * C.view.scale, h = (b[3] - b[1] + 1) * C.view.scale;
      g.globalAlpha = sel ? 0.16 : 0.08; g.fillStyle = '#7fd4ff'; g.fillRect(x0, z0, w, h);
      g.globalAlpha = sel ? 0.9 : 0.45; g.strokeStyle = sel ? '#ffd866' : '#7fd4ff'; g.lineWidth = 1;
      g.setLineDash([5, 3]); g.strokeRect(Math.round(x0) + 0.5, Math.round(z0) + 0.5, w, h); g.setLineDash([]);
      g.globalAlpha = 1;
    }
    for (const L of locales()) badge(g, L, st.sel === L.id);
    const d = st.drag;
    if (d && d.kind === 'new') {
      const x0 = Math.min(d.x0, d.x1), z0 = Math.min(d.z0, d.z1);
      g.strokeStyle = '#ffd866'; g.setLineDash([4, 3]); g.lineWidth = 1;
      g.strokeRect(C.SX(x0) + 0.5, C.SZ(z0) + 0.5, (Math.abs(d.x1 - d.x0) + 1) * C.view.scale, (Math.abs(d.z1 - d.z0) + 1) * C.view.scale);
      g.setLineDash([]);
      g.fillStyle = '#ffd866';
      g.fillText(((Math.abs(d.x1 - d.x0) + 1) / v()).toFixed(1) + ' × ' + ((Math.abs(d.z1 - d.z0) + 1) / v()).toFixed(1) + ' m', C.SX(x0) + 3, C.SZ(z0) - 4);
    }
  }
  function polyPath(g, pts) {
    g.beginPath();
    pts.forEach((p, k) => (k ? g.lineTo(C.SX(p[0]), C.SZ(p[1])) : g.moveTo(C.SX(p[0]), C.SZ(p[1]))));
    g.closePath();
  }
  function paintStructure(g, ref, sj, pos, yaw, mode) {
    const fp = footprint(sj, pos, yaw);
    polyPath(g, fp);
    g.globalAlpha = mode === 'ghost' ? 0.12 : 0.22; g.fillStyle = '#ffd866'; g.fill();
    g.globalAlpha = 1; g.lineWidth = mode === 'sel' ? 2 : 1;
    g.strokeStyle = mode === 'sel' ? '#ffffff' : '#ffd866';
    if (mode === 'ghost') g.setLineDash([4, 3]);
    g.stroke(); g.setLineDash([]);
    // the front edge (the door side) in red
    g.strokeStyle = '#ff5a4a'; g.lineWidth = 2;
    g.beginPath(); g.moveTo(C.SX(fp[2][0]), C.SZ(fp[2][1])); g.lineTo(C.SX(fp[3][0]), C.SZ(fp[3][1])); g.stroke();
    if (ref && C.view.scale >= 0.25) {
      g.fillStyle = mode === 'sel' ? '#ffffff' : '#ffe9a8';
      g.fillText(ref.id.split('/').slice(1).join('/') + ' (' + (ref.base || '?') + ')', Math.round(C.SX(pos[0])) + 4, Math.round(C.SZ(pos[2])) - 4);
    }
  }
  function paintLocale(g) {
    const L = openL(), b = boundsOf(L);
    const W = g.canvas.width, Hh = g.canvas.height;
    const x0 = C.SX(b[0]), z0 = C.SZ(b[1]), w = (b[2] - b[0] + 1) * C.view.scale, h = (b[3] - b[1] + 1) * C.view.scale;
    // outside the bounds is dimmed: the place is the lit square
    g.save();
    g.beginPath(); g.rect(0, 0, W, Hh); g.rect(x0, z0, w, h);
    g.fillStyle = 'rgba(8,10,16,0.62)'; g.fill('evenodd');
    g.restore();
    g.strokeStyle = '#7fd4ff'; g.lineWidth = 2; g.strokeRect(Math.round(x0), Math.round(z0), w, h);
    const a = L.anchor.at;
    g.strokeStyle = '#7fd4ff'; g.lineWidth = 1;
    g.beginPath(); g.moveTo(C.SX(a[0]) - 6, C.SZ(a[1])); g.lineTo(C.SX(a[0]) + 6, C.SZ(a[1])); g.moveTo(C.SX(a[0]), C.SZ(a[1]) - 6); g.lineTo(C.SX(a[0]), C.SZ(a[1]) + 6); g.stroke();
    // structures
    for (const r of structRefs()) {
      const sj = st.structs.get(r.base);
      if (!sj || sj === 'loading') continue;
      const d = st.drag && st.drag.kind === 'move' && st.drag.id === r.id ? st.drag : null;
      paintStructure(g, r, sj, r.pos, r.yaw || 0, st.selRef === r.id ? 'sel' : '');
      if (d && (d.dx || d.dz)) paintStructure(g, null, sj, [r.pos[0] + d.dx, r.pos[1], r.pos[2] + d.dz], r.yaw || 0, 'ghost');
    }
    // waynodes and their links
    const ws = waynodes(), byId = new Map(ws.map(w => [w.id, w]));
    g.lineWidth = 1;
    for (const w of ws) {
      const links = w.slot ? w.links : ((w.ref.props || {}).links || []).map(l => linkTarget(w.id.split('/')[0], l));
      for (const t of links) {
        const o = byId.get(t);
        if (!o) continue;
        g.strokeStyle = w.slot ? 'rgba(141,255,154,0.35)' : 'rgba(141,255,154,0.85)';
        g.beginPath(); g.moveTo(C.SX(w.x), C.SZ(w.z)); g.lineTo(C.SX(o.x), C.SZ(o.z)); g.stroke();
      }
    }
    for (const w of ws) {
      const x = Math.round(C.SX(w.x)), z = Math.round(C.SZ(w.z)), on = st.linkFrom === w.id || st.selRef === w.id;
      g.fillStyle = on ? '#ffd866' : w.slot ? '#5f9f6a' : '#8dff9a';
      const r = w.slot ? 2 : 3;
      g.fillRect(x - r, z - r, 2 * r + 1, 2 * r + 1);
    }
    // everything else in the group (villagers, markers, doors...): a dot
    for (const r of st.refs) {
      if (r.kind === 'structure' || r.kind === 'waynode' || !Array.isArray(r.pos)) continue;
      const x = Math.round(C.SX(r.pos[0])), z = Math.round(C.SZ(r.pos[2]));
      g.fillStyle = '#e8dcc0'; g.beginPath(); g.arc(x, z, 3, 0, Math.PI * 2); g.fill();
      if (C.view.scale >= 0.5) g.fillText(r.id.split('/').slice(1).join('/'), x + 5, z + 4);
    }
    const hover = C.hover && C.hover();
    if (hover && st.linkFrom && C.tool() === 'waylink') {
      const f = byId.get(st.linkFrom);
      if (f) {
        g.strokeStyle = '#ffd866'; g.setLineDash([3, 3]);
        g.beginPath(); g.moveTo(C.SX(f.x), C.SZ(f.z)); g.lineTo(C.SX(hover[0]), C.SZ(hover[1])); g.stroke(); g.setLineDash([]);
      }
    }
    if (hover && C.tool() === 'structure' && !st.drag && st.place.base && !structureAt(hover[0], hover[1])) {
      const sj = st.structs.get(st.place.base);
      if (sj && sj !== 'loading') paintStructure(g, null, sj, [Math.round(hover[0]), 0, Math.round(hover[1])], st.place.yaw, 'ghost');
    }
  }
  function paint(g) {
    if (!C.map()) return;
    g.save();
    g.font = '11px monospace';
    if (st.open && openL()) paintLocale(g); else paintWorld(g);
    g.restore();
  }

  /* ---- the panel ------------------------------------------------------------------------ */
  function iconButtons(cur, onPick) {
    const el = C.el, box = el('span', {class: 'mapicons'});
    for (const n of ICON_NAMES) {
      const b = el('button', {class: 'mapicon' + (n === cur ? ' on' : ''), title: n, 'data-icon': n});
      const cv = el('canvas', {width: 24, height: 24});
      const g = cv.getContext('2d'); g.imageSmoothingEnabled = false; g.drawImage(iconCanvas(n), 0, 0, 24, 24);
      b.append(cv);
      b.addEventListener('click', () => onPick(n));
      box.append(b);
    }
    return box;
  }
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'maplocale', open: true});
    els.summary = el('summary', {}, 'Locales — hand-made places (map.json locales[])');
    els.body = el('div', {class: 'maptools-panel'});
    det.append(els.summary, els.body);
    return det;
  }
  /** The tool-bar option groups for map.js's els.opts. */
  function toolOpts() {
    const el = C.el;
    els.newIcons = iconButtons(st.newIcon, n => {
      st.newIcon = n;
      for (const x of els.newIcons.children) x.classList.toggle('on', x.dataset.icon === n);
      C.setTool('locale');
    });
    els.placeSel = el('select', {title: 'the blueprint a click places (Environment > Structures)'});
    els.placeSel.addEventListener('change', () => { st.place.base = els.placeSel.value; loadStruct(st.place.base); C.setTool('structure'); });
    els.placeYaw = el('select', {title: 'which way a NEW house faces (its door side): 0 = +Z (south), 90 = +X (east)'},
                      ...[0, 90, 180, 270].map(y => el('option', {value: String(y)}, y + '°')));
    els.placeYaw.addEventListener('change', () => { st.place.yaw = +els.placeYaw.value; C.paint(); });
    els.placeName = el('input', {type: 'text', class: 'mapid', placeholder: 'name (optional)', title: 'the ref id after "<locale>/" (blank = the blueprint\'s name)'});
    els.placeName.addEventListener('change', () => { st.place.name = els.placeName.value.trim(); });
    return {
      locale: el('span', {class: 'mapopts'}, 'icon ', els.newIcons,
                 el('span', {class: 'mapnote'}, ' · drag out a NEW locale\'s bounds · click one to select · double-click opens it')),
      structure: el('span', {class: 'mapopts'}, 'blueprint ', els.placeSel, ' ', els.placeYaw, ' ', els.placeName,
                    el('span', {class: 'mapnote'}, ' · click places · drag moves · R turns · shift+click / Delete removes')),
      waylink: el('span', {class: 'mapopts'}, el('span', {class: 'mapnote'},
                  'click a waynode, then another: joined (again: parted; Shift parts) · click open ground: a new waynode, joined to the last · shift+click a waynode deletes it · Esc')),
    };
  }
  function syncToolOpts() {
    if (!els.placeSel) return;
    const el = C.el;
    els.placeSel.replaceChildren(...(st.library.length ? st.library.map(n => el('option', {value: n}, n)) : [el('option', {value: ''}, '(no structures)')]));
    els.placeSel.value = st.place.base;
    els.placeYaw.value = String(st.place.yaw);
  }
  function numIn(val, on, title, w = 70) {
    const i = C.el('input', {type: 'number', step: 1, value: val, style: `width:${w}px`, title});
    i.addEventListener('change', () => on(Math.round(+i.value || 0)));
    return i;
  }
  function sync() {
    if (!els.body || !C.map()) return;
    syncToolOpts();
    if (els.bread) syncBreadcrumb();
    const el = C.el;
    els.body.replaceChildren();
    if (st.open && openL()) { syncLocalePanel(); return; }
    els.summary.textContent = 'Locales — hand-made places (map.json locales[]): ' + locales().length;
    for (const L of locales()) {
      const b = boundsOf(L);
      const row = el('div', {class: 'mapsite' + (st.sel === L.id ? ' on' : '')});
      const cv = el('canvas', {width: 12, height: 12, class: 'mapiconsm'});
      cv.getContext('2d').drawImage(iconCanvas(L.icon), 0, 0);
      const open = el('button', {class: 'mapgo', title: 'open this locale (or double-click it on the map)'}, 'Open');
      open.addEventListener('click', () => enter(L.id));
      const sel = el('button', {title: 'select and centre'}, st.sel === L.id ? 'selected' : 'select');
      sel.addEventListener('click', () => { st.sel = L.id; C.fitBounds(b[0], b[1], b[2] + 1, b[3] + 1, true); sync(); C.paint(); });
      const del = el('button', {title: 'delete this locale from map.json (its refs file and voxel layer stay on disk)'}, '×');
      del.addEventListener('click', () => {
        if (!confirm(`Delete locale "${L.id}" from map.json? Its refs/${L.id}.json and locales/${L.id}/ stay on disk (refs then read as ABSOLUTE y).`)) return;
        edit(() => { const ls = J().locales; ls.splice(ls.indexOf(L), 1); if (st.sel === L.id) st.sel = null; });
      });
      row.append(el('span', {class: 'mapkind mapkind-locale'}, cv, ' ' + (L.icon || '?')),
                 el('span', {class: 'mapwhere'}, `${L.name || L.id} · ${b ? ((b[2] - b[0] + 1) / v()).toFixed(0) + '×' + ((b[3] - b[1] + 1) / v()).toFixed(0) + ' m' : '?'}`), open, sel, del);
      els.body.append(row);
    }
    if (!locales().length) els.body.append(el('div', {class: 'mapnote'}, 'no locales — pick the Locale tool and drag out a place\'s bounds'));
    const L = st.sel && find(st.sel);
    if (L) els.body.append(inspector(L));
  }
  function inspector(L) {
    const el = C.el;
    const box = el('div', {class: 'maptools-panel'});
    const sj = C.savedJson && C.savedJson();
    const saved = !!(sj && (sj.locales || []).some(x => x.id === L.id));
    const idIn = el('input', {type: 'text', value: L.id, class: 'mapid', title: 'the id is its FOLDER (locales/<id>/) and its REF GROUP (refs/<id>.json): fixed once the locale is saved'});
    if (saved) idIn.disabled = true;
    idIn.addEventListener('change', () => {
      const id = idIn.value.trim();
      if (!LOCALE_ID_RE.test(id)) { C.toast('a locale id is [a-z0-9_-]+ (a folder and a ref group)', true); idIn.value = L.id; return; }
      if (locales().some(x => x !== L && x.id === id)) { C.toast('another locale is already "' + id + '"', true); idIn.value = L.id; return; }
      edit(() => { L.id = id; st.sel = id; });
    });
    const nameIn = el('input', {type: 'text', value: L.name || '', class: 'mapid', title: 'what the map shows'});
    nameIn.addEventListener('change', () => edit(() => { L.name = nameIn.value.trim() || L.id; }));
    const icons = iconButtons(L.icon, n => edit(() => { L.icon = n; }));
    const bset = (k, i) => n => edit(() => {
      L.bounds[k][i] = n;
      const lo = Math.min(L.bounds.min[i], L.bounds.max[i]), hi = Math.max(L.bounds.min[i], L.bounds.max[i]);
      L.bounds.min[i] = lo; L.bounds.max[i] = hi;
    });
    const e = checkEntry(L.id);
    const take = el('button', {title: 'anchor.y := the ground the engine measures at anchor.at (the delta reads 0); a ref\'s world y is its file y + that ground either way'}, 'anchor.y := engine ground');
    take.disabled = !e || e.groundY === L.anchor.y;
    take.addEventListener('click', () => { if (e) edit(() => { L.anchor.y = e.groundY; }); });
    box.append(el('b', {}, 'locale “' + (L.name || L.id) + '”'),
      el('label', {}, 'id ', idIn), el('label', {}, 'name ', nameIn), el('div', {}, 'icon ', icons),
      el('div', {class: 'mapgrid2'},
         el('label', {}, 'min x', numIn(L.bounds.min[0], bset('min', 0), 'west edge (inclusive, world voxels)')),
         el('label', {}, 'min z', numIn(L.bounds.min[1], bset('min', 1), 'north edge (inclusive)')),
         el('label', {}, 'max x', numIn(L.bounds.max[0], bset('max', 0), 'east edge (inclusive)')),
         el('label', {}, 'max z', numIn(L.bounds.max[1], bset('max', 1), 'south edge (inclusive)')),
         el('label', {}, 'anchor x', numIn(L.anchor.at[0], n => edit(() => { L.anchor.at[0] = n; }), 'the column whose ground the locale rides')),
         el('label', {}, 'anchor z', numIn(L.anchor.at[1], n => edit(() => { L.anchor.at[1] = n; }), '')),
         el('label', {}, 'anchor y', numIn(L.anchor.y, n => edit(() => { L.anchor.y = n; }), 'the ground at anchor.at when it was authored'))),
      el('span', {class: 'mapnote' + (e && e.delta ? ' bad' : '')}, e ? `engine (--mapcheck, the saved map): ground at the anchor y${e.groundY}, anchor.y ${e.anchorY}, delta ${e.delta >= 0 ? '+' : ''}${e.delta} — every ref of the locale stands at its file y + ${e.groundY}`
         : (saved ? 'the engine has not reported this locale yet (Load check)' : 'not saved yet: save the map, then the engine measures its anchor')),
      take,
      el('span', {class: 'mapnote'}, 'Its refs live in refs/' + L.id + '.json (y relative to the anchor), its own voxels in locales/' + L.id + '/' + ((L.editLayer || 'ground').replace(/\.svedit$/, '')) + '.svedit. Double-click it on the map (or Open) to edit the place.'));
    return box;
  }
  function syncLocalePanel() {
    const el = C.el, L = openL(), b = boundsOf(L);
    els.summary.textContent = 'Locale “' + (L.name || L.id) + '”';
    const back = el('button', {class: 'mapgo'}, '← World map');
    back.addEventListener('click', leave);
    const e = checkEntry(L.id);
    const savedOk = savedAsShown(L);
    els.body.append(el('div', {class: 'mapbar'}, back),
      el('span', {class: 'mapnote'}, `${((b[2] - b[0] + 1) / v()).toFixed(1)} × ${((b[3] - b[1] + 1) / v()).toFixed(1)} m, bounds (${b[0]},${b[1]})..(${b[2]},${b[3]}); anchor (${L.anchor.at.join(',')}) y${L.anchor.y}` +
         (e ? `, engine ground y${e.groundY} (delta ${e.delta >= 0 ? '+' : ''}${e.delta})` : '')),
      el('span', {class: 'mapnote' + (savedOk ? '' : ' bad')}, savedOk ? 'Sculpt, Clearing, Soften, Tree and Water are clipped to the bounds (Save map writes them); refs are written at once through the F8 command layer; the 3D view saves its own voxel layer.'
         : 'This locale differs from the SAVED map.json: save the map before placing structures, linking waynodes or saving voxels.'));
    // structures
    els.body.append(el('div', {class: 'mapsub'}, 'Structures (refs/' + L.id + '.json)'));
    const rs = structRefs();
    for (const r of rs) {
      const row = el('div', {class: 'mapsite' + (st.selRef === r.id ? ' on' : '')});
      const go = el('button', {title: 'select'}, st.selRef === r.id ? 'selected' : 'select');
      go.addEventListener('click', () => { st.selRef = r.id; C.setTool('structure'); sync(); C.paint(); });
      const ccw = el('button', {title: 'turn -90°'}, '⟲');
      ccw.addEventListener('click', () => turnStructure(r, -90));
      const cw = el('button', {title: 'turn +90° (R)'}, '⟳');
      cw.addEventListener('click', () => turnStructure(r, 90));
      const del = el('button', {title: 'delete (undo puts it back)'}, '×');
      del.addEventListener('click', () => deleteRef(r));
      row.append(el('span', {class: 'mapkind mapkind-stamp'}, 'house'),
                 el('span', {class: 'mapwhere'}, `${r.id.split('/').slice(1).join('/')} · ${r.base} · (${r.pos[0]},${r.pos[2]}) ${r.yaw || 0}°`), go, ccw, cw, del);
      els.body.append(row);
    }
    if (!rs.length) els.body.append(el('div', {class: 'mapnote'}, st.refsFor ? 'no structures — pick the Structure tool and a blueprint, then click inside the bounds' : 'reading refs…'));
    const nWay = st.refs.filter(r => r.kind === 'waynode').length, nOther = st.refs.length - rs.length - nWay;
    els.body.append(el('div', {class: 'mapsub'}, 'Waynodes and the rest'),
      el('span', {class: 'mapnote'}, `${nWay} waynode${nWay === 1 ? '' : 's'} of the group (bright) + the houses' slot waynodes (dim); ${nOther} other ref${nOther === 1 ? '' : 's'} (villagers, markers, doors: edit them in game, F8 / F1 → References). The Link tool joins waynodes.` +
         (st.refsErr ? ' REFS FILE: ' + st.refsErr : '') + (st.lastResult ? ' Last: ' + st.lastResult : '')));
    els.body.append(el('div', {class: 'mapsub'}, 'Voxels (locales/' + L.id + '/' + (L.editLayer || 'ground').replace(/\.svedit$/, '') + '.svedit)'),
      el('span', {class: 'mapnote'}, 'The 3D view under the map: brush / erase inside the bounds, Save voxels writes the locale\'s own layer (ground-relative, applied after the map\'s).'));
  }

  /* ---- the breadcrumb + the 3D view (main column) ------------------------------------- */
  function buildBreadcrumb() {
    const el = C.el;
    els.bread = el('div', {class: 'mapbar mapcrumb', style: 'display:none'});
    return els.bread;
  }
  function syncBreadcrumb() {
    const el = C.el, L = openL();
    els.bread.style.display = L ? '' : 'none';
    if (!L) return;
    const back = el('a', {href: '#', class: 'mapcrumb-link', title: 'back to the whole map'}, 'World map');
    back.addEventListener('click', ev => { ev.preventDefault(); leave(); });
    const cv = el('canvas', {width: 24, height: 24, class: 'mapiconsm'});
    const g = cv.getContext('2d'); g.imageSmoothingEnabled = false; g.drawImage(iconCanvas(L.icon), 0, 0, 24, 24);
    els.bread.replaceChildren(back, el('span', {}, ' › '), cv, el('b', {}, ' ' + (L.name || L.id)), el('span', {class: 'mapnote'}, '  locale “' + L.id + '” of worldmap/' + C.map().name));
  }
  const view3d = (() => {
    let wv = null, raf = 0, last = 0, L3 = null, palOk = false;
    const ui = {};
    function build() {
      const el = C.el;
      ui.root = el('div', {class: 'maplocale3d', style: 'display:none'});
      ui.canvas = el('canvas', {class: 'maplocale3d-canvas'});
      ui.tool = el('select', {title: 'none = orbit only (drag to turn, wheel to zoom); brush / erase edit voxels INSIDE the bounds; picker takes a material'},
                   ...['none', 'brush', 'erase', 'picker'].map(t => el('option', {value: t}, t)));
      ui.mat = el('select', {title: 'the brush material'});
      ui.radius = el('input', {type: 'number', min: 0, max: 12, step: 1, value: 2, style: 'width:44px', title: 'brush radius, voxels'});
      ui.undo = el('button', {}, 'Undo'); ui.redo = el('button', {}, 'Redo');
      ui.save = el('button', {class: 'mapgo', title: 'write the locale\'s own voxel layer (stored ground-relative by the engine)'}, 'Save voxels');
      ui.reload = el('button', {title: 'drop the regions and fetch them again'}, 'Reload');
      ui.frameBtn = el('button', {title: 'orbit the locale anchor again, the whole locale in view'}, 'Frame');
      ui.max = el('button', {title: 'fill the window with the 3D view (Esc or this button again to restore)'}, 'Maximize');
      ui.detail = el('select', {title: 'level of detail: how close a region must be before it is drawn at full resolution. Higher is sharper further out and costs more regions'},
                     ...[['0.6', 'detail low'], ['1', 'detail normal'], ['1.6', 'detail high']].map(([v, t]) => el('option', {value: v}, t)));
      ui.detail.value = '1';
      ui.status = el('span', {class: 'mapnote'}, '');
      ui.stream = el('span', {class: 'mapnote'}, '');
      ui.help = el('div', {class: 'maplocale3d-help'},
        'Drag: turn · Shift+drag or middle-drag: pan · Wheel: zoom · WASD/QE: move the orbit point · Double-click: orbit what you clicked · ' +
        'Brush / erase edit full-resolution cells inside the locale bounds (zoom in until the readout says editable).');
      ui.tool.addEventListener('change', () => { if (wv) { wv.tool.name = ui.tool.value; } });
      ui.mat.addEventListener('change', () => { if (wv) wv.tool.material = +ui.mat.value; });
      ui.radius.addEventListener('change', () => { if (wv) wv.tool.radius = Math.max(0, Math.min(12, Math.round(+ui.radius.value || 0))); });
      ui.undo.addEventListener('click', () => { if (wv) { wv.undo(); status(); } });
      ui.redo.addEventListener('click', () => { if (wv) { wv.redo(); status(); } });
      ui.save.addEventListener('click', () => save());
      ui.reload.addEventListener('click', () => { if (wv) { wv.invalidate(); status(); } });
      ui.frameBtn.addEventListener('click', () => { if (wv && L3) frameLocale(L3); });
      ui.max.addEventListener('click', () => maximize(!ui.root.classList.contains('max')));
      ui.detail.addEventListener('change', () => { if (wv) wv.view.detail = +ui.detail.value || 1; });
      ui.canvas.addEventListener('keydown', ev => { if (ev.key === 'Escape' && ui.root.classList.contains('max')) maximize(false); });
      ui.root.append(el('div', {class: 'mapbar'}, el('b', {}, '3D (voxels)'), ' tool ', ui.tool, ' ', ui.mat, ' r ', ui.radius, ui.undo, ui.redo, ui.save, ui.reload, ui.frameBtn, ui.detail, ui.max, ui.status),
                     ui.canvas, el('div', {class: 'mapbar'}, ui.stream), ui.help);
      return ui.root;
    }
    function maximize(on) {
      ui.root.classList.toggle('max', on);
      ui.max.textContent = on ? 'Restore' : 'Maximize';
      if (on) ui.canvas.focus();
    }
    function status(t) {
      if (!ui.status) return;
      if (t) { ui.status.textContent = t; return; }
      if (!wv) return;
      ui.status.textContent = `${wv.edit.count()} voxels in the locale layer${wv.edit.unsaved.size ? ' (' + wv.edit.unsaved.size + ' unsaved)' : ''}`;
    }
    // The streaming line under the canvas, plus what is under the cursor: an
    // empty view always says why (still loading, a slow server, an error).
    function streamLine() {
      if (!wv) return;
      const h = wv.hover, b = wv.editBounds;
      let s = wv.streamStatus();
      if (h) {
        const m = wv.pal && wv.pal.materials[h.mat];
        const inB = !b || (h.vox[0] >= b[0] && h.vox[2] >= b[1] && h.vox[0] <= b[2] && h.vox[2] <= b[3]);
        s += `  ·  ${m ? m.name : '#' + h.mat} at ${h.vox.join(', ')}` +
             (h.lod > 1 ? ` (1:${h.lod} preview, zoom in to edit)` : inB ? ' (editable)' : ' (outside the locale bounds)');
      }
      if (ui.stream.textContent !== s) ui.stream.textContent = s;
    }
    function frameLocale(L) {
      const e = checkEntry(L.id), gy = e ? e.groundY : L.anchor.y;
      const b = boundsOf(L);
      wv.cam.mode = 'orbit';
      wv.cam.target = [L.anchor.at[0], gy, L.anchor.at[1]];
      // the whole locale in view: its larger side, from a 35 degree look-down
      wv.cam.dist = Math.max(60, Math.min(900, 0.9 * Math.max(b[2] - b[0], b[3] - b[1])));
      wv.cam.yaw = 0.7; wv.cam.pitch = -0.6;
    }
    async function ensure() {
      if (wv) return wv;
      if (typeof window === 'undefined' || typeof window.WorldView !== 'function') { status('3D view unavailable (worldview.js not loaded)'); return null; }
      try {
        wv = new window.WorldView(ui.canvas, {seaY: C.seaY ? C.seaY() : 0});
      } catch (e) { status('3D view unavailable: ' + e.message); wv = null; return null; }
      wv.view.showFar = false;
      wv.view.showAxes = false;
      wv.view.detail = +ui.detail.value || 1;
      // the ground under each root column, so the octree sits on the terrain
      // however far the orbit camera is pulled back
      wv.groundAt = (x, z) => (C.groundAt ? C.groundAt(x, z) : null);
      wv.onError = () => streamLine();
      wv.onEdit = () => status();
      return wv;
    }
    async function loadPalette() {
      if (palOk) return true;
      const r = await fetch('/api/voxpalette?map=' + encodeURIComponent(C.map().name), {cache: 'no-store'});
      if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
      const pal = await r.json();
      wv.loadPalette(pal);
      ui.mat.replaceChildren(...pal.materials.filter(m => m.id).map(m => C.el('option', {value: String(m.id)}, m.name)));
      const pick = pal.materials.find(m => m.name === 'cobble') || pal.materials.find(m => m.id);
      if (pick) { ui.mat.value = String(pick.id); wv.tool.material = pick.id; }
      palOk = true;
      return true;
    }
    async function loadLayer() {
      const name = C.map().name;
      const r = await fetch('/api/worldedit?map=' + encodeURIComponent(name) + '&locale=' + encodeURIComponent(L3.id) + '&seed=' + wv.seed, {cache: 'no-store'});
      if (r.status === 404) { wv.edit.clear(); wv.edit.markSaved(); return 0; }
      if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
      wv.edit.deserialize(await r.arrayBuffer());
      return wv.edit.count();
    }
    async function open(L) {
      L3 = L;
      ui.root.style.display = '';
      if (!await ensure()) return;
      const b = boundsOf(L);
      wv.mapName = C.map().name;
      wv.layerSel = '@map';
      wv.seed = C.previewSeed ? C.previewSeed() : 1337;
      wv.editBounds = b.slice();
      wv.tool.name = ui.tool.value;
      wv.tool.radius = Math.max(0, Math.round(+ui.radius.value || 2));
      wv.edit.clear(); wv.edit.markSaved();
      frameLocale(L);
      wv.invalidate();
      if (!raf) { last = 0; raf = requestAnimationFrame(frame); }
      try {
        await loadPalette();
        const n = await loadLayer();
        status(`${n} voxels in the locale layer`);
      } catch (e2) { status('3D: ' + e2.message); }
    }
    function close() {
      if (ui.root) ui.root.style.display = 'none';
      if (wv && wv.edit.unsaved.size) C.toast(`left ${wv.edit.unsaved.size} unsaved voxel edit(s) of locale "${L3 && L3.id}" behind (not written)`, true);
      L3 = null;
      if (wv) { wv.edit.clear(); wv.edit.markSaved(); wv.invalidate(); }
      if (ui.root) maximize(false);
    }
    // Render only when the picture can have changed (the camera, an edit, a
    // region or mesh arriving) -- as tuner.html's wvFrame does. A 3D view
    // redrawn every frame is a GPU (or, headless, a SwiftShader core) spent on
    // a still image while the author works on the map beside it.
    let lastSig = '';
    function sig() {
      const c = wv.cam, v = wv.view;
      // drawRev covers everything that changes the picture without the camera
      // moving (a mesh landing, a region arriving or leaving, an edit)
      return [c.mode, c.target, c.dist, c.yaw, c.pitch, c.pos, wv.drawRev, v.detail,
              wv.tool.name, wv.tool.radius, wv.hover ? wv.hover.vox.join('.') : '-', ui.canvas.clientWidth, ui.canvas.clientHeight].join('|');
    }
    function frame(now) {
      raf = requestAnimationFrame(frame);
      if (!wv || !L3 || !C.isVisible() || ui.root.offsetParent === null) return;
      const dt = Math.min(0.1, (now - last) / 1000 || 0);
      last = now;
      wv.step(dt); wv.update();
      const s = sig();
      if (s !== lastSig) { lastSig = s; wv.render(); }
      streamLine();
    }
    async function save() {
      if (!wv || !L3) return false;
      if (!needSaved(L3, 'voxels')) return false;
      const name = C.map().name;
      const r = await fetch('/api/worldedit?map=' + encodeURIComponent(name) + '&locale=' + encodeURIComponent(L3.id) + '&seed=' + wv.seed,
                            {method: 'POST', body: wv.edit.serialize(wv.seed), headers: {'Content-Type': 'application/octet-stream'}});
      const j = await r.json().catch(() => ({ok: false, error: 'HTTP ' + r.status}));
      if (!j.ok) { C.toast('voxel save failed: ' + (j.error || '?'), true); return false; }
      st.saved = j;
      wv.edit.markSaved();
      wv.invalidate();   // the server applies the saved layer now
      status(j.deleted ? 'saved: the locale layer is empty (file removed)' : `saved ${wv.edit.count()} voxels (${j.bytes} B, ground-relative)`);
      if (C.saved) C.saved();
      return true;
    }
    return {build, open, close, save, view: () => wv, status, ui};
  })();

  return {
    st, buildPanel, buildBreadcrumb, build3d: () => view3d.build(), toolOpts, paint, pointerDown, pointerMove, pointerUp, dblclick, key,
    sync, afterRestore, reset, enter, leave, replay, loadRefs, loadLibrary,
    open: () => (st.open ? openL() : null), dragging: () => !!st.drag,
    clipping, sampleAllowed, pointAllowed, circleAllowed, clipBox, boxEditable,
    problems: () => (C.map() ? localeProblems(J()) : null),
    view3d, waynodes, structureAt, placeStructure, linkPair, newWaynode, deleteRef, turnStructure, moveStructure,
  };
}
