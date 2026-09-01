/* ============================================================================
   attacks.js — the ATTACKS lane of the Models tab.

   Authors assets/mobs/attack_styles.json: the stroke programs both the NPCs
   and the player's discrete strikes replay (src/game/strokes.h is the schema
   of record, and its header comment is the reasoning behind every field).

   WHY THIS IS A TAB AND NOT A ROW IN THE TUNING TAB. An attack style is FOUR
   NUMBERS PER SEGMENT and none of them is a pose. What a style actually does
   depends on the arm it is played on (the reach is a BAND POSITION, resolved
   against that rig's own bones), on the blade in the fist (the hand is the tip
   minus a measured blade), and on three clamps that can each move the result.
   The only honest way to author one is next to a rig that is swinging it, and
   that is what this lane is: the panel edits the JSON, rig.js runs the REAL
   driver (editor/melee.js, a line-cited port of melee.cpp) on the real
   skeleton, and the viewport draws the swept edge.

   ---------------------------------------------------------------------------
   THE LAYOUT RULES, because the first version broke all four.

   It rendered with `.rigf` and `.clipprops`, which are built for the 260px
   SIDE PANEL, and inherited from them: a 104px label column per field, every
   hint forced onto its own line indented 110px, and wrapping rows that each
   grew to the full window width. Twelve numbers came out as a thousand-pixel
   column of prose with the values clipped inside 52px boxes.

     1. FIELDS GO IN A GRID, not in a flex row of label+input pairs. The
        segments already ARE a table — windup/cut/recover x ticks/az/el/reach —
        and seeing them aligned is most of what makes a style readable.
     2. A BOX IS WIDE ENOUGH FOR ITS NUMBER. Spinners are off (nobody nudges a
        radian by 1) and the text is centred, so nothing hides under the arrows.
     3. ANGLES ARE AUTHORED IN DEGREES. The file stores radians and always
        will — strokes.h is in radians and a stored degree would be a second
        unit for one fact — but -132 is four characters and -2.30 is five with
        a decimal point everyone miscounts. The radian value rides under the
        box as a read-only sub-label.
     4. EXPLANATION LIVES IN `title=` AND ONE HELP FOLD, not between the
        controls. The reasoning is worth having; it is not worth 300px of
        vertical space every time you nudge a tick count.

   ---------------------------------------------------------------------------
   OWNERSHIP, because three documents are in play and they are not the same:

     assets/mobs/attack_styles.json   THE STYLES and the player's flick
       compass. Owned here; written through /api/model, the same route the
       mob/item sidecars use.
     the rig sidecar (<mob>.json)     PER-LIMB range of motion (`poseLimit`).
       Owned by rig.js; this panel edits the arm's entries in place, because
       "how far may this shoulder go" is the per-limb half of "how does this
       attack work on this limb".
     assets/materials/tuning.json     the `melee.*` block: the shared feel, and
       the torso/elbow/head knobs that decide how much of the BODY joins a
       swing. Owned by the Tuning tab; this panel edits the same live object.

   Every field says which file it writes, and EVERY EDIT IS UNDOABLE — through
   the editor's own Ctrl+Z, not a second stack of this lane's own (see
   `editDoc` and editor.js's `pushUndoEntry`).
   ========================================================================== */

import * as MELEE from './melee.js';

const STYLES_PATH = 'mobs/attack_styles.json';

let el = null, toast = () => {};
let host = null;            // the seam rig.js registers (see bind)

// ---- the document -------------------------------------------------------
let raw = null;             // the parsed JSON, MUTATED IN PLACE and saved whole
let lib = null;             // MELEE.parseStyleLibrary(raw)
let dirty = false;
let err = '';
let loading = false;

// ---- panel state --------------------------------------------------------
let selected = -1;          // index into lib.styles
let aim = { az: 0, el: 0 }; // the target's bearing about the shoulder, radians
let loop = true;
let swingNo = 0;
let trailOn = true;
let flick = { x: 1, y: 0 }; // the compass pad's test flick
const folds = { limbs: false, compass: false, help: false };

const num = (v, d = 0) => (Number.isFinite(+v) ? +v : d);
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const DEG = 180 / Math.PI;
const TICK_MS = 1000 / 30;   // the sim's own rate; `ticks` in the JSON are these
const fmt = (v, n = 2) => (Number.isFinite(v) ? v.toFixed(n) : '—');

/* ==========================================================================
   the seam rig.js binds
   ========================================================================== */

export function bind(h) {
  host = h;
  el = h.el;
  toast = h.toast || (() => {});
}

export const library = () => lib;
export const styleIndex = () => selected;
export const currentAim = () => aim;
export const looping = () => loop;
export const trailEnabled = () => trailOn;
export const swingNumber = () => swingNo;
export const isDirty = () => dirty;
export function nextSwing() { swingNo = (swingNo + 1) >>> 0; }
/** Select a style by index — for the test seam, so it can drive a real box. */
export function selectStyle(i) {
  if (!lib || i < 0 || i >= lib.styles.length || i === selected) return;
  selected = i;
  render();
}

/* ==========================================================================
   load / save
   ========================================================================== */

export async function load(force) {
  if (raw && !force) return lib;
  if (loading) return lib;
  loading = true;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent(STYLES_PATH),
                          { cache: 'no-store' });
    if (!r.ok) throw new Error(STYLES_PATH + ' ' + r.status);
    raw = JSON.parse(await r.text());
    lib = MELEE.parseStyleLibrary(raw);
    err = '';
    dirty = false;
    if (selected < 0 && lib.styles.length) selected = 0;
  } catch (e) {
    // file:// degradation, exactly as the item list and the audio tab do it.
    raw = null; lib = null;
    err = String(e.message || e);
  } finally {
    loading = false;
  }
  return lib;
}

export async function save() {
  if (!raw) return;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent(STYLES_PATH), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(raw, null, 2) + '\n',
    });
    if (!r.ok) throw new Error(await r.text() || ('HTTP ' + r.status));
    dirty = false;
    toast('attack_styles.json saved — press R in the game to hot-reload');
    render();
  } catch (e) {
    toast('save failed: ' + (e.message || e), true);
  }
}

/* ==========================================================================
   EDITS AND UNDO

   One helper for all three documents. It snapshots before, runs the mutation,
   snapshots after, and hands both restores to the editor's own undo stack —
   so Ctrl+Z on this tab means one thing whatever panel you were in.

   RESTORE REPLACES CONTENT, NEVER THE REFERENCE. The parsed style library
   holds `.raw` back-pointers into `raw.styles[i]`, rig.js's skeleton is built
   from the sidecar object, and the melee driver reads the tuning object every
   tick; swapping any of those for a fresh clone would leave live readers
   pointing at the old one. So the snapshot is poured back into the SAME
   container and the derived state is rebuilt after.
   ========================================================================== */

const snap = o => JSON.parse(JSON.stringify(o));

function pourInto(target, source) {
  for (const k of Object.keys(target)) delete target[k];
  Object.assign(target, snap(source));
}

/**
 * `which` is 'styles' | 'tuning' | 'rig'. `fn` mutates the live document.
 * Everything else — the snapshot pair, the undo entry, the dirty flag, the
 * re-parse and the re-render — happens here, so no call site can forget one.
 */
function editDoc(which, label, fn) {
  const doc = which === 'styles' ? raw
            : which === 'tuning' ? host?.tuning?.()
            : host?.sidecar?.();
  if (!doc) { fn(); afterEdit(which); return; }
  const before = snap(doc);
  fn();
  const after = snap(doc);
  const apply = s => { pourInto(doc, s); afterEdit(which); };
  host?.pushUndo?.(label, () => apply(before), () => apply(after));
  afterEdit(which);
}

/** Re-derive whatever `which` feeds, mark it dirty, and repaint. */
function afterEdit(which) {
  if (which === 'styles') {
    dirty = true;
    // The library holds RESOLVED INDICES for the player compass (strokes.h:107)
    // and `.raw` back-pointers, so a rename or a delete has to rebuild it — the
    // same reason the engine rebuilds the map with the library on every R.
    const keep = (selected >= 0 && lib) ? lib.styles[selected]?.name : null;
    lib = MELEE.parseStyleLibrary(raw);
    const i = keep ? lib.styles.findIndex(s => s.name === keep) : -1;
    selected = i >= 0 ? i : Math.min(Math.max(selected, 0), lib.styles.length - 1);
    host?.onStylesChanged?.();
  } else if (which === 'tuning') {
    host?.touchTuning?.();
    host?.onTuningChanged?.();
  } else {
    host?.touchSidecar?.();
    host?.rebuild?.();
  }
  render();
}

const editStyles = (label, fn) => editDoc('styles', label, fn);
const editTuning = (label, fn) => editDoc('tuning', label, fn);
const editRig = (label, fn) => editDoc('rig', label, fn);

/* ==========================================================================
   field helpers — see LAYOUT RULES 2 and 3 at the top
   ========================================================================== */

/**
 * A number box that fits its number, with an optional read-only sub-label
 * underneath (the radians behind a degrees box, the voxels behind a band
 * position). The pair is ONE grid cell, never a layout row of its own.
 *
 * `get`/`set` rather than (obj, key) so a degrees box can front a radians
 * field without either side knowing.
 */
function numCell(opts) {
  const cell = el('div', { class: 'atkcell' });
  const i = el('input', {
    class: 'atknum' + (opts.wide ? ' wide' : ''), type: 'number',
    step: opts.step === undefined ? 'any' : String(opts.step),
    title: opts.title || '',
  });
  const show = () => { i.value = String(opts.get()); };
  show();
  i.addEventListener('change', () => {
    let v = num(i.value, opts.dflt ?? 0);
    if (opts.int) v = Math.round(v);
    if (opts.min !== undefined) v = Math.max(opts.min, v);
    if (opts.max !== undefined) v = Math.min(opts.max, v);
    if (v === opts.get()) { show(); return; }   // no undo entry for a no-op
    opts.set(v);
  });
  cell.append(i);
  if (opts.sub !== undefined) cell.append(el('em', { title: opts.subTitle || '' }, opts.sub));
  return cell;
}

/** Degrees in, radians stored. See LAYOUT RULE 3. */
function degCell(obj, key, label, editLabel) {
  return numCell({
    step: 1, min: -200, max: 200,
    title: `${label} — authored in DEGREES here, stored as radians in the ` +
      'JSON (strokes.h is in radians; a stored degree would be a second unit ' +
      'for one fact)',
    get: () => Math.round(num(obj[key], 0) * DEG),
    sub: fmt(num(obj[key], 0), 3) + ' rad',
    subTitle: 'the value as the file stores it',
    set: v => editStyles(`${editLabel} ${label}`, () => { obj[key] = v / DEG; }),
  });
}

const chip = (label, on, onclick, title) => el('button', {
  class: 'small' + (on ? ' on' : ''), title: title || '', onclick,
}, label);

/* ==========================================================================
   the panel
   ========================================================================== */

let wrap = null;

export function render(container) {
  if (container) wrap = container;
  if (!wrap) return;
  wrap.innerHTML = '';

  if (err) {
    wrap.append(el('div', { class: 'rignote' },
      'attack_styles.json could not be read: ' + err +
      ' — the Attacks lane needs the tuner SERVER (./sandvox_tuner.exe or ' +
      'python scripts/tuner_server.py); a file:// page cannot read assets/.'));
    return;
  }
  if (!lib) {
    wrap.append(el('div', { class: 'rignote' }, 'loading attack_styles.json…'));
    load().then(() => render());
    return;
  }

  renderStyleBar();

  const grid = el('div', { class: 'atk' });
  const sty = lib.styles[selected];
  grid.append(sty ? programCard(sty) : el('div', { class: 'atkcard' },
    el('div', { class: 'rignote' },
      'No style selected. A style is a WINDUP that raises the blade into a ' +
      'stance, then a CUT that drives it through the target line.')));
  grid.append(previewCard(sty));
  wrap.append(grid);

  wrap.append(foldedLimbs());
  wrap.append(foldedCompass());
  wrap.append(foldedHelp());
  renderLoaderLog();
}

/* ---- the style list ---------------------------------------------------- */

function renderStyleBar() {
  const bar = el('div', { class: 'tagbar' });
  const groups = [
    ['player', lib.styles.map((s, i) => [s, i]).filter(([s]) => s.name.startsWith('player_'))],
    ['npc', lib.styles.map((s, i) => [s, i]).filter(([s]) => !s.name.startsWith('player_'))],
  ];
  for (const [label, list] of groups) {
    if (!list.length) continue;
    bar.append(el('span', { class: 'hint', title: label === 'player'
      ? 'the player\'s discrete strikes (melee.controlMode 0), picked by the flick at the press'
      : 'replayed by NPCs; a behaviour profile lists opaque ids and draws one per attack' },
      label));
    for (const [s, i] of list)
      bar.append(chip(s.name.replace(/^player_/, ''), i === selected,
        () => { selected = i; render(); }, s.label));
  }

  bar.append(el('span', { class: 'spacer' }));
  bar.append(
    chip('+', false, addStyle, 'a new stroke program, seeded from the selected one'),
    selected >= 0 ? chip('rename', false, renameStyle,
      'renames the id and every reference to it inside THIS file') : null,
    selected >= 0 ? chip('dup', false, dupStyle) : null,
    selected >= 0 ? el('button', { class: 'small danger', onclick: deleteStyle,
      title: 'delete this style and its compass references' }, '✕') : null,
    el('button', {
      class: 'small' + (dirty ? ' primary' : ''),
      title: 'write assets/mobs/attack_styles.json (Ctrl+Z undoes edits before ' +
        'you save; after a save, undo still works — it just leaves the file ahead)',
      onclick: () => save(),
    }, dirty ? 'Save •' : 'Save'),
    chip('↻', false, () => { load(true).then(() => render()); },
      're-read from disk (another session may have edited it)'));
  wrap.append(bar);
}

function addStyle() {
  const n = prompt('style id (lowercase, no spaces — this is what a behaviour ' +
    'profile names)', 'rising_cut');
  if (!n) return;
  if (lib.styles.some(s => s.name === n)) return toast('that id exists', true);
  // Seeded from a real style rather than from zeros: an all-zero program is a
  // stroke that never moves, which reads as "the editor is broken".
  const base = lib.styles[selected] || lib.styles[0];
  const seed = base ? snap(base.raw) : {
    windup: { ticks: 12, az: 0.3, el: 0.05, reach: -0.05 },
    cut: { ticks: 7, az: -2.3, el: 0, reach: 0.1 },
    recover: { ticks: 10 },
    jitter: { az: 0.1, el: 0.04, tempo: 0.25 },
  };
  seed.name = n; seed.label = n;
  editStyles('add attack "' + n + '"', () => {
    raw.styles = raw.styles || [];
    raw.styles.push(seed);
  });
  selected = lib.styles.findIndex(s => s.name === n);
  render();
}

function renameStyle() {
  const s = lib.styles[selected];
  const n = prompt('rename style id', s.name);
  if (!n || n === s.name) return;
  if (lib.styles.some(x => x.name === n)) return toast('that id exists', true);
  const old = s.name;
  // RENAME EVERY REFERENCE. A style id is named from three places — the player
  // compass, the neutral pair, and behaviors.json — and the engine skips an
  // unknown one LOUDLY rather than crashing, so a half-rename is a silent
  // behaviour change. The first two are fixed here; the third is another file
  // and is reported instead of silently edited.
  editStyles('rename "' + old + '"', () => {
    const target = raw.styles.find(x => x.name === old);
    if (target) target.name = n;
    for (const sec of (raw.player?.sectors || [])) if (sec.style === old) sec.style = n;
    const na = raw.player?.neutralAlternate;
    if (Array.isArray(na)) for (let k = 0; k < na.length; k++) if (na[k] === old) na[k] = n;
  });
  toast(`renamed — check assets/mobs/behaviors.json for "${old}"`);
}

function dupStyle() {
  const s = lib.styles[selected];
  let n = s.name + '_2';
  for (let k = 2; lib.styles.some(x => x.name === n); k++) n = s.name + '_' + k;
  const copy = snap(s.raw);
  copy.name = n;
  editStyles('duplicate "' + s.name + '"', () => {
    raw.styles.splice(raw.styles.findIndex(x => x.name === s.name) + 1, 0, copy);
  });
  selected = lib.styles.findIndex(x => x.name === n);
  render();
}

function deleteStyle() {
  const s = lib.styles[selected];
  const refs = (raw.player?.sectors || []).filter(x => x.style === s.name).length +
    (raw.player?.neutralAlternate || []).filter(x => x === s.name).length;
  if (!confirm(`Delete "${s.name}"?` +
    (refs ? `\n\n${refs} reference(s) in this file's player compass go with it.` : '') +
    '\n\nUndo (Ctrl+Z) brings it back. A behaviour profile that still names it ' +
    'gets a loud skip and its first available style, never a crash.')) return;
  editStyles('delete "' + s.name + '"', () => {
    raw.styles.splice(raw.styles.findIndex(x => x.name === s.name), 1);
    if (raw.player) {
      raw.player.sectors = (raw.player.sectors || []).filter(x => x.style !== s.name);
      raw.player.neutralAlternate =
        (raw.player.neutralAlternate || []).filter(x => x !== s.name);
    }
  });
}

/* ---- the program card -------------------------------------------------- */

function programCard(sty) {
  const r = sty.raw;
  const card = el('div', { class: 'atkcard' });
  card.append(el('h5', {}, 'program',
    el('span', { class: 'spacer' }),
    el('span', { class: 'hint', style: 'text-transform:none;letter-spacing:0' },
      sty.name)));

  // label
  const lbl = el('input', { class: 'atknum wide', type: 'text',
    title: 'human text for the dev readout' });
  lbl.value = sty.label || '';
  lbl.addEventListener('change', () => {
    if (lbl.value === (r.label || '')) return;
    editStyles('label', () => { r.label = lbl.value; });
  });
  card.append(el('div', { class: 'atkrow' },
    el('label', {}, 'label'), lbl));

  // ---- the segment table (LAYOUT RULE 1) ------------------------------
  const seg = el('div', { class: 'atkseg' });
  seg.append(el('div', {}),
    el('div', { class: 'hdr', title: '30 Hz sim ticks' }, 'ticks'),
    el('div', { class: 'hdr', title: 'azimuth: 0 straight ahead, + to the mob\'s right' }, 'az°'),
    el('div', { class: 'hdr', title: 'elevation: 0 level' }, 'el°'),
    el('div', { class: 'hdr', title: 'a POSITION in this arm\'s reach band, not voxels' }, 'reach'));

  for (const [key, name, tip] of [
    ['windup', 'windup',
     'A POSE, relative to the aim, driven closed-loop and deliberately SLOWLY ' +
     '(under melee.commitSpeed, so the driver stays in Guard and no cut fires). ' +
     'Its length IS the whole telegraph — there is no UI indicator by design.'],
    ['cut', 'cut',
     'A TRAVEL, not a pose: how far the point goes and how fast. The deltas are ' +
     'divided by the tick count and delivered per tick, which is what commits ' +
     'the driver\'s own Slash and gives the sweep the tip speed that scales the ' +
     'damage — SPEED IS THE DAMAGE.'],
  ]) {
    if (!r[key] || typeof r[key] !== 'object')
      r[key] = { ticks: 8, az: 0, el: 0, reach: 0 };
    const s = r[key];
    seg.append(el('div', { class: 'lbl', title: tip }, name));
    seg.append(numCell({
      int: true, step: 1, min: 1, max: 120, dflt: 8, title: tip,
      get: () => Math.max(1, Math.round(num(s.ticks, 8))),
      sub: fmt(Math.max(1, Math.round(num(s.ticks, 8))) * TICK_MS / 1000, 2) + ' s',
      set: v => editStyles(`${name} ticks`, () => { s.ticks = v; }),
    }));
    seg.append(degCell(s, 'az', 'azimuth', name));
    seg.append(degCell(s, 'el', 'elevation', name));
    seg.append(numCell({
      step: 0.01, min: -1, max: 1,
      title: 'A BAND POSITION offset, not voxels. 0 means the neutral 0.60 of ' +
        'this arm\'s own annulus — authored against the ARM instead, every ' +
        'chamber and lunge in the shipped library landed outside the band and ' +
        'was clamped (strokes.cpp:160).',
      get: () => num(s.reach, 0),
      sub: reachSub(s.reach),
      subTitle: 'where that lands on the arm currently previewing',
      set: v => editStyles(`${name} reach`, () => { s.reach = v; }),
    }));
  }

  if (!r.recover || typeof r.recover !== 'object') r.recover = { ticks: 10 };
  seg.append(el('div', { class: 'lbl',
    title: 'a hold with no input: the follow-through unwinds and the arm is ' +
           'handed back to the walk cycle' }, 'recover'));
  seg.append(numCell({
    int: true, step: 1, min: 1, max: 120, dflt: 10,
    get: () => Math.max(1, Math.round(num(r.recover.ticks, 10))),
    sub: fmt(Math.max(1, Math.round(num(r.recover.ticks, 10))) * TICK_MS / 1000, 2) + ' s',
    set: v => editStyles('recover ticks', () => { r.recover.ticks = v; }),
  }));
  seg.append(el('div', {}), el('div', {}), el('div', {}));

  if (!r.jitter || typeof r.jitter !== 'object') r.jitter = { az: 0, el: 0, tempo: 0 };
  const jTip = 'VARIATION IS DETERMINISTIC (CLAUDE.md rule 1): every draw is ' +
    'Hash3(mobId ^ salt, tick, index), so ten swings vary and the fight ' +
    'replays. 0 on the player_* styles is deliberate — a strike must go ' +
    'exactly where it was flicked.';
  seg.append(el('div', { class: 'lbl', title: jTip }, 'jitter'));
  seg.append(numCell({
    step: 0.01, min: 0, max: 1,
    title: 'tempo: scales BOTH tick counts, ± — what stops two duelists ' +
      'beating time together',
    get: () => num(r.jitter.tempo, 0), sub: 'tempo',
    set: v => editStyles('jitter tempo', () => { r.jitter.tempo = v; }),
  }));
  seg.append(degCell(r.jitter, 'az', 'start bow', 'jitter'));
  seg.append(degCell(r.jitter, 'el', 'start bow', 'jitter'));
  seg.append(el('div', {}));
  card.append(seg);

  card.append(derivedLine(sty));
  return card;
}

/** Where an authored reach offset lands on the arm currently previewing. */
function reachSub(offset) {
  const a = host?.armInfo?.();
  const at = MELEE.kNeutralReach + num(offset, 0);
  if (!a || a.bandHi === undefined || a.bandHi <= a.bandLo)
    return 'band ' + fmt(at);
  const v = a.bandLo + clamp(at, 0, 1) * (a.bandHi - a.bandLo);
  return (at < 0 || at > 1 ? '⚠ ' : '') + fmt(v) + ' vox';
}

/**
 * ONE derived line instead of a paragraph per field — and it is the line an
 * author actually wants: is the telegraph long enough to read, and does the
 * cut travel far enough fast enough.
 */
function derivedLine(sty) {
  const w = sty.windup.ticks, c = sty.cut.ticks, rc = sty.recoverTicks;
  const total = w + c + rc;
  const cutDeg = Math.round(Math.hypot(sty.cut.az, sty.cut.el) * DEG);
  const cutS = c * TICK_MS / 1000;
  const d = el('div', { class: 'atkderived' });
  d.append(el('span', {},
    el('b', {}, String(total)), ' ticks / ', el('b', {}, fmt(total * TICK_MS / 1000)),
    ' s  ·  telegraph ', el('b', {}, fmt(w * TICK_MS / 1000)),
    ' s  ·  cut sweeps ', el('b', {}, cutDeg + '°'), ' in ', el('b', {}, fmt(cutS)),
    ' s (', el('b', {}, String(Math.round(cutDeg / Math.max(cutS, 1e-3)))), '°/s)'));
  if (sty.jitter.tempo > 0) {
    const lo = Math.max(2, Math.round(w * (1 - sty.jitter.tempo)));
    const hi = Math.max(2, Math.round(w * (1 + sty.jitter.tempo)));
    d.append(el('span', {}, ` · windup varies ${lo}–${hi} ticks`));
  }
  return d;
}

/* ---- the preview card -------------------------------------------------- */

function previewCard(sty) {
  const card = el('div', { class: 'atkcard' });
  const a = host?.armInfo?.() || {};
  card.append(el('h5', {}, 'preview', el('span', { class: 'spacer' }),
    el('span', { class: 'hint', style: 'text-transform:none;letter-spacing:0' },
      a.armName || 'no arm')));

  // ---- transport
  const t1 = el('div', { class: 'atkrow' });
  t1.append(
    el('button', {
      class: 'small primary',
      title: 'run this stroke program through the REAL driver on the rig',
      onclick: () => {
        if (selected < 0) return toast('pick an attack first', true);
        const arm = host?.armInfo?.();
        if (!arm || arm.chain < 0)
          return toast('this rig has no arm chain tagged "arm" whose effector ' +
            'is the weapon socket\'s part — the driver has nothing to steer', true);
        nextSwing();
        host.begin(selected);
        render();
      },
    }, '▶ swing'),
    el('button', { class: 'small', onclick: () => { host?.stop?.(); render(); } }, '■'),
    chip('loop', loop, () => { loop = !loop; render(); },
      'replay the program forever — the authoring mode'),
    chip('trail', trailOn, () => {
      trailOn = !trailOn; host?.onTrailToggled?.(); render();
    }, 'draw the blade\'s swept edge, coloured by phase'),
    chip('reroll', false, () => {
      nextSwing();
      if (host?.state?.().live) host?.begin?.(selected);
      render();
    }, `jitter draw #${swingNo} — the next deterministic draw, the same ` +
       'sequence the game walks'));
  card.append(t1);

  // ---- THE SWORD BUTTON. A blade is not decoration here: the driver MEASURES
  // hand-to-point and solves the whole reach band against it, so an empty hand
  // is a different set of numbers rather than the same swing without a prop.
  const t2 = el('div', { class: 'atkrow' });
  const armed = host?.weaponEquipped?.();
  t2.append(el('button', {
    class: 'small' + (armed ? '' : ' primary'),
    title: armed
      ? 'swap the weapon in the rig\'s hand (Held item panel has the full list)'
      : 'put a sword in this rig\'s hand. The stroke driver MEASURES the blade ' +
        'and solves its reach band against it — with an empty fist the point IS ' +
        'the hand and the band is the bare arm, so the preview is a different ' +
        'swing, not the same one undressed.',
    onclick: async () => { await host?.equipWeapon?.(); render(); },
  }, armed ? '◆ ' + (host?.weaponName?.() || 'armed') : '+ sword'));
  if (armed)
    t2.append(el('button', {
      class: 'small', title: 'empty the hand',
      onclick: async () => { await host?.equipWeapon?.(null, true); render(); },
    }, 'disarm'));
  t2.append(el('span', { class: 'spacer' }),
    el('label', { title: 'the target\'s BEARING about the shoulder; the cut is ' +
      'CENTRED on it, so the windup lands half a cut short and the blade passes ' +
      'THROUGH where it was aimed' }, 'aim'),
    aimSlider('az'), aimSlider('el'),
    chip('centre', false, () => { aim = { az: 0, el: 0 }; render(); }));
  card.append(t2);

  if (sty) card.append(strokeBar(sty));
  card.append(readout());
  return card;
}

function aimSlider(key) {
  const g = el('span', { style: 'display:inline-flex;gap:3px;align-items:center' });
  const s = el('input', { type: 'range', min: key === 'az' ? '-1.4' : '-1.0',
    max: key === 'az' ? '1.4' : '1.0', step: '0.01',
    style: 'width:64px', title: key });
  s.value = String(aim[key]);
  const box = el('span', { class: 'hint',
    style: 'font-family:var(--mono);min-width:46px' },
    `${key} ${fmt(aim[key])}`);
  s.addEventListener('input', () => {
    aim[key] = num(s.value, 0);
    box.textContent = `${key} ${fmt(aim[key])}`;
  });
  g.append(s, box);
  return g;
}

/** windup | cut | recover as one proportional bar, with the live phase cursor. */
function strokeBar(sty) {
  const st = host?.state?.() || {};
  const w = sty.windup.ticks, c = sty.cut.ticks, rc = sty.recoverTicks;
  const total = Math.max(1, w + c + rc);
  const bar = el('div', {
    class: 'cliplane', id: 'atkStrokeBar',
    style: 'width:100%;height:18px;cursor:default;display:flex;overflow:hidden;margin:6px 0 4px',
  });
  bar.dataset.total = String(total);
  bar.dataset.w = String(w);
  bar.dataset.c = String(c);
  const s = (n, frac, colour, tip) => el('div', {
    style: `flex:${frac} 0 0;background:${colour};display:flex;align-items:center;` +
           'justify-content:center;font-size:9.5px;color:#0b0e13;font-weight:600;' +
           'overflow:hidden;white-space:nowrap',
    title: tip,
  }, n);
  bar.append(
    s(`windup ${w}`, w, '#6aa9ff', 'the telegraph: driven under commitSpeed, no cut fires'),
    s(`cut ${c}`, c, '#ffd08a', 'the travel: this is the part that damages'),
    s(`recover ${rc}`, rc, '#3d4756', 'hand-back: PoseWeight ramps down'));
  if (st.live) {
    const done = st.phase === 'windup' ? st.phaseTick
      : st.phase === 'cut' ? w + st.phaseTick
      : st.phase === 'recover' ? w + c + st.phaseTick : 0;
    bar.append(el('span', { class: 'ccursor',
      style: `left:${clamp(done / total, 0, 1) * 100}%` }));
  }
  return bar;
}

/**
 * WHAT THE DRIVER IS ACTUALLY DOING, in two dense columns. Every number here
 * separates causes a bare "the attack looks wrong" cannot — the same argument
 * NpcStroke's three tally words make in strokes.h.
 */
function readout() {
  const st = host?.state?.() || {};
  const a = host?.armInfo?.() || {};
  const box = el('div', { class: 'atkreadout', id: 'atkReadout' });
  if (a.chain === undefined || a.chain < 0) {
    return el('div', { class: 'rignote atkwarn' },
      '⚠ no weapon arm: the driver needs a chain tagged "arm" whose effector ' +
      'is the part the weapon socket names. Add one in the Parts panel.');
  }
  const line = (k, v, tip) => {
    const s = el('span', { title: tip || '' }, k, el('b', {}, v));
    s.dataset.k = k;
    return s;
  };
  box.append(
    line('phase', st.phase || 'idle', 'the STROKE PROGRAM\'s phase'),
    line('driver', st.meleePhase || 'idle',
      'MeleeState: guard / wind / slash / recover. A windup deliberately stays ' +
      'in GUARD — that is what "under commitSpeed" means.'),
    line('az/el', `${fmt(st.az)} / ${fmt(st.el)}`, 'the live stroke, radians'),
    line('radius', fmt(st.radius), 'commanded tip radius, voxels'),
    line('tip', fmt(st.tipSpeed, 1) + ' v/s',
      'SPEED IS THE DAMAGE. Below melee.minSpeedMps the sweep does nothing.'),
    line('edge', fmt(st.edgeAlign),
      'MeleeEdgeAlign: 1 is edge-on, melee.edgeFloor is a slap with the flat'),
    line('weight', fmt(st.weight), 'PoseWeight: how much of the arm the stroke claims'),
    line('reach', fmt(a.reach), 'L1 + L2 off the LIVE bone lengths'),
    line('blade', fmt(a.bladeLen),
      'MEASURED hand-to-point distance of whatever is in the fist — never authored'),
    line('band', `${fmt(a.bandLo)}–${fmt(a.bandHi)}`,
      'the annulus of tip radii this arm can serve with that blade'));
  return box;
}

/**
 * Per-frame refresh of the LIVE values only. Deliberately NOT a re-render:
 * rebuilding the panel ten times a second would tear focus out of every box
 * the author is typing in. Same split the clip lane makes with
 * updateClipCursorUI().
 */
export function tickUI() {
  if (!wrap || !host) return;
  const st = host.state?.() || {};
  const a = host.armInfo?.() || {};
  const box = wrap.querySelector('#atkReadout');
  if (box) {
    const set = (k, v) => {
      const s = box.querySelector(`[data-k="${k}"] b`);
      if (s) s.textContent = v;
    };
    set('phase', st.phase || 'idle');
    set('driver', st.meleePhase || 'idle');
    set('az/el', `${fmt(st.az)} / ${fmt(st.el)}`);
    set('radius', fmt(st.radius));
    set('tip', fmt(st.tipSpeed, 1) + ' v/s');
    set('edge', fmt(st.edgeAlign));
    set('weight', fmt(st.weight));
    set('blade', fmt(a.bladeLen));
    set('band', `${fmt(a.bandLo)}–${fmt(a.bandHi)}`);
  }
  const bar = wrap.querySelector('#atkStrokeBar');
  if (bar) {
    let cur = bar.querySelector('.ccursor');
    if (!st.live) { if (cur) cur.remove(); return; }
    if (!cur) { cur = el('span', { class: 'ccursor' }); bar.append(cur); }
    const total = +bar.dataset.total || 1;
    const w = +bar.dataset.w || 0, c = +bar.dataset.c || 0;
    const done = st.phase === 'windup' ? st.phaseTick
      : st.phase === 'cut' ? w + st.phaseTick
      : st.phase === 'recover' ? w + c + st.phaseTick : 0;
    cur.style.left = `${clamp(done / total, 0, 1) * 100}%`;
  }
}

export const isVisible = () => !!(wrap && wrap.isConnected);

/* ---- folds: per-limb, compass, help ------------------------------------ */

function fold(key, title, build) {
  const d = el('details', { class: 'atkfold' });
  if (folds[key]) d.open = true;
  d.append(el('summary', {}, title));
  // Built lazily and rebuilt on toggle, so a closed fold costs nothing and an
  // open one is never stale.
  const body = el('div', {});
  if (folds[key]) body.append(build());
  d.append(body);
  d.addEventListener('toggle', () => {
    folds[key] = d.open;
    body.innerHTML = '';
    if (d.open) body.append(build());
  });
  return d;
}

const foldedLimbs = () => fold('limbs', 'per-limb — range of motion & body share', limbsBody);
const foldedCompass = () => fold('compass', 'player flick compass', compassBody);
const foldedHelp = () => fold('help', 'how a stroke works', helpBody);

/**
 * THE PER-LIMB HALF. An attack style says where the POINT goes; what each limb
 * may do getting it there is `poseLimit` on that limb plus the body-wide knobs.
 * The two live in different files and this is explicit about which.
 */
function limbsBody() {
  const wrapEl = el('div', {});
  const a = host?.armInfo?.() || {};
  wrapEl.append(el('div', { class: 'rignote' },
    'Joint limits are a POSE stage, not a physics constraint — Jolt only ' +
    'enforces minAngle/maxAngle on a DYNAMIC body and a live limb is ' +
    'kinematic, so nothing but poseLimit stops the IK raking a joint anywhere. ' +
    'These write the RIG sidecar; the knobs below write tuning.json.'));
  for (const p of (a.armParts || [])) wrapEl.append(limbRow(p));
  if (!(a.armParts || []).length)
    wrapEl.append(el('div', { class: 'rignote' }, 'no arm parts to show.'));

  const t = host?.tuning?.();
  if (!t) {
    wrapEl.append(el('div', { class: 'rignote' },
      'tuning.json is not loaded in this page, so the body-wide melee knobs ' +
      'are read-only defaults here.'));
    return wrapEl;
  }
  if (!t.melee || typeof t.melee !== 'object') t.melee = {};
  const m = t.melee;
  const grid = el('div', { class: 'atkseg',
    style: 'grid-template-columns:repeat(5,minmax(0,1fr));margin-top:6px' });
  const knob = (key, label, tip, o) => {
    grid.append(el('div', { class: 'atkcell' },
      el('div', { class: 'hdr', style: 'font-size:9.5px', title: tip }, label),
      numCell({ ...o, title: tip,
        get: () => num(m[key], o.dflt ?? 0),
        set: v => editTuning('melee.' + key, () => { m[key] = v; }) })));
  };
  knob('torsoShare', 'torso yaw', 'fraction of the stroke\'s azimuth the CHEST ' +
    'takes. Capped at ±0.5 rad in code — the cap is the anatomy, this is the ' +
    'taste. 0 is the A/B if a rig ever reads mirrored.',
    { step: 0.01, min: 0, max: 1, dflt: 0.35 });
  knob('torsoPitch', 'torso pitch', 'fraction of the elevation. Clamps tighter ' +
    'downward: the arm legitimately hangs at -1.5 rad in a low guard and a ' +
    'chest that followed it there would read as a bow.',
    { step: 0.01, min: 0, max: 1, dflt: 0.2 });
  knob('elbowPoleCone', 'elbow cone', 'radians the elbow\'s bend plane may stray ' +
    'from straight-back. A human elbow trails down, back or out — never forward.',
    { step: 0.05, min: 0.05, max: 3.1, dflt: 1.75 });
  knob('headClearM', 'head clear m', 'the blade stays this far outside the ' +
    'wielder\'s own head sphere — a RIGID translate, so |tip − hand| is ' +
    'preserved. 0 is the off switch.', { step: 0.01, min: 0, max: 0.5, dflt: 0.06 });
  knob('handBackFrac', 'hand back', 'fraction of reach the hand may go BEHIND ' +
    'the shoulder\'s frontal plane.', { step: 0.01, min: 0, max: 0.6, dflt: 0.05 });
  knob('azOut', 'az out', 'azimuth stop to the WEAPON side, radians',
    { step: 0.01, min: 0, max: 2.5, dflt: 1.83 });
  knob('azAcross', 'az across', 'azimuth stop ACROSS the body, radians',
    { step: 0.01, min: 0, max: 2.5, dflt: 1.4 });
  knob('elMin', 'el min', 'radians', { step: 0.01, min: -1.6, max: 0, dflt: -1.5 });
  knob('elMax', 'el max', 'radians', { step: 0.01, min: 0, max: 1.6, dflt: 1.48 });
  knob('commitSpeed', 'commit spd', 'input units/s above which a Wind becomes a ' +
    'Slash. A windup drives at 480 against this, which is why it stays a ' +
    'telegraph however far it has to travel.', { step: 10, min: 1, dflt: 900 });
  wrapEl.append(grid);
  return wrapEl;
}

/** One limb's pose limit, in whichever of the three shapes it is authored in. */
function limbRow(p) {
  const row = el('div', { class: 'atklimb' });
  const limb = host?.limbByName?.(p.name);
  row.append(el('div', { class: 'nm', title: p.name }, p.name));
  if (!limb) {
    row.append(el('div', { class: 'hint' }, '—'),
      el('div', { class: 'hint' }, 'not in limbs[]'), el('div', {}));
    return row;
  }
  const pl = limb.poseLimit;
  if (!pl) {
    row.append(el('div', { class: 'hint' }, 'free'),
      el('div', { class: 'hint' }, 'the IK may pose this joint to ANY angle'),
      el('div', { class: 'rigbtns', style: 'margin:0' },
        chip('hinge', false, () => editRig('add hinge limit to ' + p.name, () => {
          limb.poseLimit = { axis: [-1, 0, 0], min: 0, max: 130, hinge: true };
        }), 'one DOF, off-axis swing DISCARDED — the elbow form'),
        chip('axis', false, () => editRig('add axis limit to ' + p.name, () => {
          limb.poseLimit = { axis: [-1, 0, 0], min: -20, max: 85 };
        }), 'clamp the twist about one axis, leave the swing — hip/knee'),
        chip('ball', false, () => editRig('add ball limit to ' + p.name, () => {
          limb.poseLimit = { bone: [0, -1, 0],
            reach: [{ normal: [0, 0, -1], max: 50 }, { normal: [-1, 0, 0], max: 30 }],
            twist: { min: -75, max: 75 } };
        }), 'bound where the bone may POINT plus a roll bound — the shoulder')));
    return row;
  }
  if (pl.bone !== undefined) {
    const tw = pl.twist || (pl.twist = { min: -75, max: 75 });
    row.append(el('div', { class: 'hint', title: 'bound where the bone may ' +
      'POINT (up to two perpendicular planes) plus a roll bound' }, 'ball'));
    const body = el('div', { style: 'display:flex;gap:5px;align-items:center;flex-wrap:wrap' });
    (Array.isArray(pl.reach) ? pl.reach : []).forEach((rr, i) => {
      body.append(el('span', { class: 'hint' }, `p${i}`), numCell({
        step: 1, min: -90, max: 90,
        title: 'at most N degrees past this plane; the plane itself is 0',
        get: () => num(rr.max, 0),
        set: v => editRig(`${p.name} reach plane ${i}`, () => { rr.max = v; }),
      }));
    });
    body.append(el('span', { class: 'hint', title: 'roll about the bone, ' +
      'degrees. The weapon arm\'s elbow-plane override is bounded by this ' +
      'cone, so a tight range makes the elbow stiffer in a cut.' }, 'twist'),
      numCell({ step: 1, min: -180, max: 180,
        get: () => num(tw.min, -75),
        set: v => editRig(p.name + ' twist min', () => { tw.min = v; }) }),
      numCell({ step: 1, min: -180, max: 180,
        get: () => num(tw.max, 75),
        set: v => editRig(p.name + ' twist max', () => { tw.max = v; }) }));
    row.append(body);
  } else {
    row.append(chip(pl.hinge ? 'hinge' : 'axis', true,
      () => editRig(p.name + ' limit kind', () => { pl.hinge = !pl.hinge; }),
      pl.hinge ? 'ONE DOF: the off-axis swing is discarded. Click for the axis form.'
               : 'clamp the twist about the axis, leave the swing. Click for hinge.'));
    const body = el('div', { style: 'display:flex;gap:5px;align-items:center' });
    body.append(el('span', { class: 'hint', title: 'degrees, measured from the ' +
      'REST pose. For the weapon arm the PLANE is steered by the driver; the ' +
      'range is never touched.' }, 'min/max°'),
      numCell({ step: 1, min: -180, max: 180,
        get: () => num(pl.min, 0),
        set: v => editRig(p.name + ' min', () => { pl.min = v; }) }),
      numCell({ step: 1, min: -180, max: 180,
        get: () => num(pl.max, 0),
        set: v => editRig(p.name + ' max', () => { pl.max = v; }) }));
    row.append(body);
  }
  row.append(el('button', { class: 'small danger', title: 'remove the limit',
    onclick: () => editRig('remove ' + p.name + ' limit',
      () => { delete limb.poseLimit; }) }, '✕'));
  return row;
}

/* ---- the player's flick compass ---------------------------------------- */

function compassBody() {
  const wrapEl = el('div', {});
  const pj = raw.player || (raw.player = { sectors: [], neutralAlternate: [] });
  if (!Array.isArray(pj.sectors)) pj.sectors = [];

  const row = el('div', { style: 'display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap' });
  const SZ = 150;
  const pad = el('canvas', { id: 'atkCompass', width: String(SZ), height: String(SZ),
    style: 'border:1px solid #262e3c;border-radius:6px;background:#0f131a;cursor:crosshair',
    title: 'drag to test a flick — the label below names the style the engine ' +
           'would pick. +y is DOWN (raw screen mouse).' });
  const testLbl = el('div', { class: 'hint',
    style: 'font-family:var(--mono);max-width:150px' });
  const updateTest = () => {
    const i = MELEE.quantizeStrike(lib, flick.x, flick.y);
    testLbl.textContent = `(${fmt(flick.x)}, ${fmt(flick.y)}) → ` +
      (i >= 0 ? lib.styles[i].name : 'no sector');
  };
  drawCompass(pad, SZ); updateTest();
  const padPoint = ev => {
    const r = pad.getBoundingClientRect();
    const x = (ev.clientX - r.left) / SZ * 2 - 1, y = (ev.clientY - r.top) / SZ * 2 - 1;
    const l = Math.hypot(x, y);
    flick = l > 1e-4 ? { x: x / l, y: y / l } : { x: 1, y: 0 };
    drawCompass(pad, SZ); updateTest();
  };
  pad.addEventListener('pointerdown', e => {
    padPoint(e);
    const mv = ev => padPoint(ev);
    const up = () => { window.removeEventListener('pointermove', mv);
                       window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', mv);
    window.addEventListener('pointerup', up);
  });
  row.append(el('div', {}, pad, testLbl));

  const list = el('div', { style: 'flex:1;min-width:260px' });
  pj.sectors.forEach((sec, i) => {
    if (!Array.isArray(sec.dir)) sec.dir = [1, 0];
    const r = el('div', { class: 'atklimb',
      style: 'grid-template-columns:26px 52px 52px 1fr auto' });
    r.append(el('div', { class: 'nm' }, dirName(sec.dir)));
    for (const k of [0, 1])
      r.append(numCell({ step: 0.1, min: -1, max: 1,
        title: k ? '+y is DOWN — raw screen mouse' : '+x is right',
        get: () => num(sec.dir[k], 0),
        set: v => editStyles('sector dir', () => { sec.dir[k] = v; }) }));
    r.append(styleSelect(sec.style, v =>
      editStyles('sector style', () => { sec.style = v; })));
    r.append(el('button', { class: 'small danger', onclick: () =>
      editStyles('delete sector', () => { pj.sectors.splice(i, 1); }) }, '✕'));
    list.append(r);
  });
  list.append(el('div', { class: 'rigbtns' },
    chip('+ sector', false, () => editStyles('add sector', () => {
      pj.sectors.push({ dir: [0, -1],
        style: lib.styles[selected]?.name || lib.styles[0]?.name || '' });
    }), 'a new flick direction. It competes for the whole circle by max dot — ' +
       'check the pad afterwards that every sector is still reachable.')));

  if (!Array.isArray(pj.neutralAlternate)) pj.neutralAlternate = [];
  const nr = el('div', { class: 'atkrow' });
  nr.append(el('label', { title: 'a click below melee.pickMinSpeed alternates ' +
    'these two' }, 'no flick'));
  for (let k = 0; k < 2; k++)
    nr.append(styleSelect(pj.neutralAlternate[k], v =>
      editStyles('neutral strike', () => { pj.neutralAlternate[k] = v; })));
  list.append(nr);
  row.append(list);
  wrapEl.append(row);

  // Sectors compete for the circle by MAX DOT rather than tiling it, so adding
  // one can silently swallow another and nothing in the engine would say so.
  const unreachable = pj.sectors.map(s => s.style).filter(name => {
    const i = lib.styles.findIndex(x => x.name === name);
    if (i < 0) return false;
    for (let k = 0; k < 720; k++) {
      const a = k / 720 * Math.PI * 2;
      if (MELEE.quantizeStrike(lib, Math.cos(a), Math.sin(a)) === i) return false;
    }
    return true;
  });
  if (unreachable.length)
    wrapEl.append(el('div', { class: 'rignote atkwarn' },
      '⚠ unreachable by any flick: ' + unreachable.join(', ') +
      ' — another sector wins the max dot everywhere.'));
  return wrapEl;
}

const dirName = d => {
  const a = Math.atan2(num(d[1], 0), num(d[0], 0)) * DEG;
  return ['→', '↘', '↓', '↙', '←', '↖', '↑', '↗'][(Math.round(a / 45) + 8) % 8];
};

function styleSelect(value, onChange) {
  const s = el('select', { class: 'sortsel' });
  s.append(el('option', { value: '' }, '(none)'));
  for (const st of lib.styles) s.append(el('option', { value: st.name }, st.name));
  s.value = value || '';
  s.addEventListener('change', () => onChange(s.value));
  return s;
}

/**
 * Draw the max-dot partition by SAMPLING QuantizeStrike, not by drawing wedges
 * from the sector angles. Once the directions are not evenly spaced the two
 * pictures differ, and the wedge picture is the one that hides an unreachable
 * sector.
 */
function drawCompass(canvas, SZ) {
  const g = canvas.getContext('2d');
  const R = SZ / 2 - 2;
  g.clearRect(0, 0, SZ, SZ);
  const palette = ['#3a5f8f', '#8f6a3a', '#3a8f5f', '#8f3a5f', '#5f3a8f',
                   '#8f8f3a', '#3a8f8f', '#8f3a3a'];
  const steps = 240;
  for (let k = 0; k < steps; k++) {
    const a0 = k / steps * Math.PI * 2, a1 = (k + 1) / steps * Math.PI * 2;
    const i = MELEE.quantizeStrike(lib, Math.cos((a0 + a1) / 2), Math.sin((a0 + a1) / 2));
    g.beginPath();
    g.moveTo(SZ / 2, SZ / 2);
    g.arc(SZ / 2, SZ / 2, R, a0, a1);
    g.closePath();
    g.fillStyle = i >= 0 ? palette[i % palette.length] : '#1a212c';
    g.fill();
  }
  g.lineWidth = 2;
  for (const s of lib.player.sectors) {
    const l = Math.hypot(s.x, s.y) || 1;
    g.strokeStyle = '#dfe6f2';
    g.beginPath();
    g.moveTo(SZ / 2, SZ / 2);
    g.lineTo(SZ / 2 + s.x / l * R, SZ / 2 + s.y / l * R);
    g.stroke();
  }
  g.strokeStyle = '#ffd08a'; g.lineWidth = 3;
  g.beginPath();
  g.moveTo(SZ / 2, SZ / 2);
  g.lineTo(SZ / 2 + flick.x * R, SZ / 2 + flick.y * R);
  g.stroke();
  g.fillStyle = '#7c8798';
  g.font = '9px ui-monospace, monospace';
  g.fillText('up', SZ / 2 - 6, 10);
  g.fillText('down = thrust', SZ / 2 - 32, SZ - 3);
}

/* ---- the help fold ------------------------------------------------------
   The prose that used to sit BETWEEN the controls (LAYOUT RULE 4). It is worth
   having; it is not worth 300px of vertical space on every edit.
   ---------------------------------------------------------------------- */

function helpBody() {
  const d = el('div', { class: 'atkhelp' });
  const p = (...k) => d.append(el('p', {}, ...k));
  const b = t => el('b', {}, t);
  p(b('THE CUT IS CENTRED ON THE AIM. '),
    'The windup lands at aim minus half the cut\'s own travel, so the blade ' +
    'passes THROUGH where it was aimed rather than starting there — a stroke ' +
    'aimed at its own start point cuts the air behind the target every time. ' +
    'The aim is frozen at the END of the windup and never refreshed, which is ' +
    'what makes the telegraph mean something.');
  p(b('WINDUP is a POSE; CUT is a TRAVEL. '),
    'The windup is driven closed-loop and deliberately slowly — under ' +
    'melee.commitSpeed, so the driver stays in Guard and no cut fires — and ' +
    'its length is the whole telegraph. The cut\'s deltas are divided by its ' +
    'tick count and delivered per tick, which is what commits the driver\'s ' +
    'own Slash and gives the sweep the tip speed that scales the damage.');
  p(b('REACH IS A BAND POSITION. '),
    '0 is the neutral 0.60 of the arm\'s own annulus, not zero voxels. ' +
    'Authored against the ARM instead, every chamber and lunge in the shipped ' +
    'library landed outside the band and was clamped: a thrust asking for four ' +
    'voxels moved 0.15 and read as a twitch.');
  p(b('VARIATION IS DETERMINISTIC. '),
    'Every draw is Hash3(mobId ^ salt, tick, index), so ten swings vary and the ' +
    'fight replays. jitter.az/el bow the start pose; jitter.tempo scales both ' +
    'tick counts. The player_* styles author 0 on purpose — a strike must go ' +
    'exactly where it was flicked.');
  p(b('THE PREVIEW IS THE ENGINE\'S OWN DRIVER. '),
    'editor/melee.js is a line-cited port of melee.cpp and strokes.cpp, not a ' +
    'second implementation. Verify it with ', el('code', {}, 'node scripts/test_melee.mjs'),
    ' and the panel itself with ', el('code', {}, 'bash scripts/check_attacks.sh'), '.');
  p(b('EVERYTHING HERE IS UNDOABLE '), 'with the editor\'s own Ctrl+Z — style ' +
    'edits, pose limits and the melee knobs alike, in one stack, in the order ' +
    'you made them.');
  return d;
}

function renderLoaderLog() {
  if (!lib?.log?.length) return;
  wrap.append(el('div', { class: 'rignote atkwarn' },
    'the ENGINE\'s loader would skip these (a bad entry is never fatal): ' +
    lib.log.join(' · ')));
}
