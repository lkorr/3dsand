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

   OWNERSHIP, because three files are in play and they are not the same file:

     assets/mobs/attack_styles.json   THE STYLES and the player's flick
       compass. Owned here; written through /api/model, the same route the
       mob/item sidecars use.
     the rig sidecar (<mob>.json)     PER-LIMB range of motion (`poseLimit`).
       Owned by rig.js; this panel edits the arm's entries in place and marks
       the sidecar dirty, because "how far may this shoulder go" is the
       per-limb half of "how does this attack work on this limb".
     assets/materials/tuning.json     the `melee.*` block: the shared feel, and
       the torso/elbow/head knobs that decide how much of the BODY joins a
       swing. Owned by the Tuning tab; this panel edits the same live object
       and marks it dirty, so one number has one home.

   Every field says which file it writes.
   ========================================================================== */

import * as MELEE from './melee.js';

const STYLES_PATH = 'mobs/attack_styles.json';

let el = null, toast = () => {};
let host = null;            // the {begin, stop, state, ...} rig.js registers
let onChanged = () => {};

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
let showLimbs = true;
let showCompass = true;
let flick = { x: 1, y: 0 }; // the compass pad's test flick

const num = (v, d = 0) => (Number.isFinite(+v) ? +v : d);
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const DEG = 180 / Math.PI;
const TICK_MS = 1000 / 30;   // the sim's own rate; `ticks` in the JSON are these

/* ==========================================================================
   the seam rig.js binds
   ========================================================================== */

/**
 * `h` is { begin(styleIndex), stop(), state(), armInfo(), toast, el,
 *          sidecar(), touchSidecar(), tuning(), touchTuning(), rerender() }.
 * Everything the panel needs from the rig lives behind it, so this file never
 * touches the skeleton and rig.js never parses a style.
 */
export function bind(h) {
  host = h;
  el = h.el;
  toast = h.toast || (() => {});
  onChanged = h.rerender || (() => {});
}

export const library = () => lib;
export const styleIndex = () => selected;
export const currentAim = () => aim;
export const looping = () => loop;
export const trailEnabled = () => trailOn;
export const swingNumber = () => swingNo;
export const isDirty = () => dirty;

/** Advance the jitter draw. The engine keys it on the start TICK, so every
 *  swing of a style is a different one; the panel walks them deliberately so
 *  the author can see the whole spread instead of one sample. */
export function nextSwing() { swingNo = (swingNo + 1) >>> 0; }

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
    // file:// degradation, exactly as the item list and the audio tab do it: a
    // page with no server cannot read assets/, and saying so is better than an
    // empty list that looks like "you have authored no attacks".
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

/**
 * Re-parse after an edit. The library holds RESOLVED INDICES for the player
 * compass (strokes.h:107), so a rename or a delete has to rebuild it or a
 * sector would point at the wrong style — the same reason the engine rebuilds
 * the map with the library on every R.
 */
function touched(reparse) {
  dirty = true;
  if (reparse) {
    const keepName = (selected >= 0 && lib) ? lib.styles[selected]?.name : null;
    lib = MELEE.parseStyleLibrary(raw);
    if (keepName) {
      const i = lib.styles.findIndex(s => s.name === keepName);
      selected = i >= 0 ? i : Math.min(selected, lib.styles.length - 1);
    }
  }
  host?.onStylesChanged?.();
  render();
}

/* ==========================================================================
   small DOM helpers — the panel reuses the rig CSS vocabulary so it looks
   like the rest of the tab rather than like a second application.
   ========================================================================== */

function field(label, input, hint) {
  const f = el('div', { class: 'rigf' }, el('span', {}, label), input);
  if (hint) f.append(el('i', {}, hint));
  return f;
}

/** A number box bound to `obj[key]`, with an optional live derived readout. */
function numBox(obj, key, opts = {}) {
  const i = el('input', {
    class: 'cell num', type: 'number',
    step: opts.step === undefined ? 'any' : String(opts.step),
  });
  if (opts.min !== undefined) i.min = String(opts.min);
  if (opts.max !== undefined) i.max = String(opts.max);
  i.value = obj[key] === undefined ? '' : String(obj[key]);
  i.addEventListener('change', () => {
    let v = num(i.value, opts.dflt ?? 0);
    if (opts.int) v = Math.round(v);
    if (opts.min !== undefined) v = Math.max(opts.min, v);
    if (opts.max !== undefined) v = Math.min(opts.max, v);
    obj[key] = v;
    i.value = String(v);
    (opts.onChange || (() => touched(false)))();
  });
  return i;
}

/**
 * A radians field with a DEGREES box beside it, because nobody authors a
 * -2.30 rad cut and everybody authors a -132 degree one. Both write the same
 * key; radians stay canonical because the JSON and strokes.h are in radians
 * and a stored degree would be a second unit for one fact.
 */
function radBox(obj, key, opts = {}) {
  const wrap = el('div', { class: 'rigvec' });
  const rad = el('input', { class: 'cell num', type: 'number', step: '0.01' });
  const deg = el('input', { class: 'cell num', type: 'number', step: '1',
                            title: 'degrees — the same value, converted' });
  const sync = from => {
    if (from !== 'rad') rad.value = (num(obj[key], 0)).toFixed(3);
    if (from !== 'deg') deg.value = Math.round(num(obj[key], 0) * DEG);
  };
  sync('');
  rad.addEventListener('change', () => {
    obj[key] = clamp(num(rad.value, 0), -Math.PI * 1.05, Math.PI * 1.05);
    sync('rad'); touched(false);
  });
  deg.addEventListener('change', () => {
    obj[key] = clamp(num(deg.value, 0) / DEG, -Math.PI * 1.05, Math.PI * 1.05);
    sync('deg'); touched(false);
  });
  wrap.append(rad, deg);
  if (opts.title) wrap.title = opts.title;
  return wrap;
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
  renderTransport();
  const sty = lib.styles[selected];
  if (sty) {
    renderProgram(sty);
    renderStrokeBar(sty);
  } else {
    wrap.append(el('div', { class: 'rignote' },
      'No style selected. A style is a WINDUP that raises the blade into a ' +
      'stance, then a CUT that drives it through the target line — fed to the ' +
      'same driver the mouse feeds, so an NPC swing is a motion a person ' +
      'could make.'));
  }
  renderReadout();
  if (showLimbs) renderLimbSection();
  if (showCompass) renderCompass();
  renderLoaderLog();
}

/* ---- the style list ---------------------------------------------------- */

function renderStyleBar() {
  const bar = el('div', { class: 'tagbar' });
  bar.append(el('span', { class: 'hint' }, 'attacks'));

  // Grouped by who replays them, because they are authored to different rules:
  // the player_* entries have short windups (a click must feel owned) and
  // jitter 0 (a strike must go exactly where it was flicked).
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
    chip('+ attack', false, () => {
      const n = prompt('style id (lowercase, no spaces — this is what a ' +
        'behaviour profile names)', 'rising_cut');
      if (!n) return;
      if (lib.styles.some(s => s.name === n)) return toast('that id exists', true);
      // Seeded from a real style rather than from zeros: an all-zero program
      // is a stroke that never moves, which reads as "the editor is broken".
      const base = lib.styles[selected] || lib.styles[0];
      const seed = base ? JSON.parse(JSON.stringify(base.raw)) : {
        windup: { ticks: 12, az: 0.3, el: 0.05, reach: -0.05 },
        cut: { ticks: 7, az: -2.3, el: 0, reach: 0.1 },
        recover: { ticks: 10 },
        jitter: { az: 0.1, el: 0.04, tempo: 0.25 },
      };
      seed.name = n;
      seed.label = n;
      raw.styles = raw.styles || [];
      raw.styles.push(seed);
      touched(true);
      selected = lib.styles.findIndex(s => s.name === n);
      render();
    }, 'a new stroke program, seeded from the selected one'),
    selected >= 0 ? chip('rename', false, () => {
      const s = lib.styles[selected];
      const n = prompt('rename style id', s.name);
      if (!n || n === s.name) return;
      if (lib.styles.some(x => x.name === n)) return toast('that id exists', true);
      // RENAME EVERY REFERENCE. A style id is named from three places — the
      // player compass, the neutral pair, and behaviours.json — and the engine
      // skips an unknown one LOUDLY rather than crashing, so a half-rename is
      // a silent behaviour change rather than an error. The first two live in
      // this file and are fixed here; the third is another file and is
      // reported instead of silently edited.
      const old = s.name;
      s.raw.name = n;
      for (const sec of (raw.player?.sectors || []))
        if (sec.style === old) sec.style = n;
      const na = raw.player?.neutralAlternate;
      if (Array.isArray(na)) for (let k = 0; k < na.length; k++)
        if (na[k] === old) na[k] = n;
      touched(true);
      toast(`renamed — check assets/mobs/behaviors.json for "${old}" ` +
            '(a profile naming it will now get a loud skip)');
    }, 'renames the id and every reference to it inside THIS file') : null,
    selected >= 0 ? chip('duplicate', false, () => {
      const s = lib.styles[selected];
      let n = s.name + '_2';
      for (let k = 2; lib.styles.some(x => x.name === n); k++) n = s.name + '_' + k;
      const copy = JSON.parse(JSON.stringify(s.raw));
      copy.name = n;
      raw.styles.splice(raw.styles.indexOf(s.raw) + 1, 0, copy);
      touched(true);
      selected = lib.styles.findIndex(x => x.name === n);
      render();
    }) : null,
    selected >= 0 ? el('button', {
      class: 'small danger',
      onclick: () => {
        const s = lib.styles[selected];
        const refs = (raw.player?.sectors || []).filter(x => x.style === s.name).length +
          (raw.player?.neutralAlternate || []).filter(x => x === s.name).length;
        if (!confirm(`Delete "${s.name}"?` +
          (refs ? `\n\n${refs} reference(s) in this file's player compass will ` +
                  'also be removed.' : '') +
          '\n\nA behaviour profile that still names it gets a loud skip and ' +
          'its first available style, never a crash.')) return;
        raw.styles.splice(raw.styles.indexOf(s.raw), 1);
        if (raw.player) {
          raw.player.sectors = (raw.player.sectors || []).filter(x => x.style !== s.name);
          raw.player.neutralAlternate =
            (raw.player.neutralAlternate || []).filter(x => x !== s.name);
        }
        selected = -1;
        touched(true);
        if (lib.styles.length) selected = 0;
        render();
      },
    }, '✕ attack') : null,
    el('button', {
      class: 'small' + (dirty ? ' primary' : ''),
      title: 'write assets/mobs/attack_styles.json',
      onclick: () => save(),
    }, dirty ? 'Save •' : 'Save'),
    chip('↻', false, () => { load(true).then(() => render()); },
      're-read from disk (another session may have edited it)'));
  wrap.append(bar);
}

/* ---- transport --------------------------------------------------------- */

function renderTransport() {
  const st = host?.state?.() || {};
  const bar = el('div', { class: 'tagbar' });
  bar.append(
    el('button', {
      class: 'small primary',
      title: 'run this stroke program through the REAL driver on the rig',
      onclick: () => {
        if (selected < 0) return toast('pick an attack first', true);
        const a = host?.armInfo?.();
        if (!a || a.chain < 0) {
          return toast('this rig has no arm chain tagged "arm" whose effector ' +
            'is the weapon socket\'s part — the driver has nothing to steer', true);
        }
        nextSwing();
        host.begin(selected);
        render();
      },
    }, '▶ swing'),
    el('button', { class: 'small', onclick: () => { host?.stop?.(); render(); } }, '■ stop'),
    chip('loop', loop, () => { loop = !loop; render(); },
      'replay the program forever — the authoring mode'),
    chip('trail', trailOn, () => {
      trailOn = !trailOn; host?.onTrailToggled?.(); render();
    }, 'draw the blade\'s swept edge in the viewport, coloured by phase'),
    el('span', { class: 'hint' }, `jitter draw #${swingNo}`),
    chip('reroll', false, () => {
      nextSwing();
      if (st.live) host?.begin?.(selected);
      render();
    }, 'the next deterministic jitter draw — Hash3(seed, k, 0), the same ' +
       'sequence the game walks'),
    el('span', { class: 'spacer' }),
    el('span', { class: 'hint' }, 'aim'),
    aimSlider('az', -1.4, 1.4),
    aimSlider('el', -1.0, 1.0),
    chip('centre', false, () => { aim = { az: 0, el: 0 }; render(); },
      'aim straight ahead and level'));
  wrap.append(bar);
  wrap.append(el('div', { class: 'rignote' },
    'THE CUT IS CENTRED ON THE AIM. The windup lands at aim minus half the ' +
    'cut\'s own travel, so the blade passes THROUGH where it was aimed rather ' +
    'than starting there — a stroke aimed at its own start point cuts the air ' +
    'behind the target every time. The aim is frozen at the END of the windup ' +
    'and never refreshed, which is what makes the telegraph mean something.'));
}

function aimSlider(key, lo, hi) {
  const s = el('input', { type: 'range', min: String(lo), max: String(hi),
                          step: '0.01', style: 'width:90px' });
  s.value = String(aim[key]);
  s.title = key === 'az'
    ? 'the target\'s BEARING about the shoulder, radians; + is the mob\'s right'
    : 'the target\'s ELEVATION about the shoulder, radians; 0 is level';
  s.addEventListener('input', () => {
    aim[key] = num(s.value, 0);
    const lbl = s.nextSibling;
    if (lbl) lbl.textContent = `${key} ${aim[key].toFixed(2)}`;
  });
  const box = el('span', { class: 'hint' }, `${key} ${aim[key].toFixed(2)}`);
  const g = el('span', { style: 'display:inline-flex;gap:4px;align-items:center' });
  g.append(s, box);
  return g;
}

/* ---- the program editor ------------------------------------------------ */

function renderProgram(sty) {
  const r = sty.raw;
  const box = el('div', { class: 'clipprops' });

  const lbl = el('input', { class: 'cell', type: 'text', style: 'width:230px' });
  lbl.value = sty.label || '';
  lbl.addEventListener('change', () => { r.label = lbl.value; touched(true); });
  box.append(field('label', lbl, 'human text for the dev readout'));
  wrap.append(box);

  wrap.append(segmentRow('windup', r, 'windup',
    'A POSE, relative to the aim, driven closed-loop and deliberately SLOWLY ' +
    '(under melee.commitSpeed, so the driver stays in Guard and no cut fires). ' +
    'Its length IS the whole telegraph — there is no UI indicator by design.'));
  wrap.append(segmentRow('cut', r, 'cut',
    'A TRAVEL, not a pose: how far the point goes and how fast. The deltas are ' +
    'divided by the tick count and delivered per tick, which is what commits ' +
    'the driver\'s own Slash and gives the sweep the tip speed that scales the ' +
    'damage — SPEED IS THE DAMAGE.'));

  // recover + jitter
  const tail = el('div', { class: 'clipprops' });
  if (!r.recover || typeof r.recover !== 'object') r.recover = { ticks: 10 };
  tail.append(field('recover ticks',
    numBox(r.recover, 'ticks', { int: true, min: 1, max: 120, dflt: 10,
                                 onChange: () => touched(true) }),
    tickHint(r.recover.ticks) + ' — a hold with no input: the follow-through ' +
    'unwinds and the arm is handed back to the walk cycle'));
  if (!r.jitter || typeof r.jitter !== 'object') r.jitter = { az: 0, el: 0, tempo: 0 };
  tail.append(field('jitter az', radBox(r.jitter, 'az'), 'bows the START pose, ±'));
  tail.append(field('jitter el', radBox(r.jitter, 'el')));
  tail.append(field('jitter tempo',
    numBox(r.jitter, 'tempo', { step: 0.01, min: 0, max: 1,
                                onChange: () => touched(true) }),
    'scales BOTH tick counts, ± — what stops two duelists beating time together'));
  wrap.append(tail);
  wrap.append(el('div', { class: 'rignote' },
    'VARIATION IS DETERMINISTIC (CLAUDE.md rule 1): every draw is ' +
    'Hash3(mobId ^ salt, tick, index), so ten swings vary and the fight ' +
    'replays. Jitter 0 on the player_* styles is deliberate — a strike must ' +
    'go exactly where it was flicked.'));
}

const tickHint = t => `${Math.max(1, Math.round(num(t, 0)))} ticks = ` +
  `${(Math.max(1, Math.round(num(t, 0))) * TICK_MS / 1000).toFixed(2)} s @30 Hz`;

function segmentRow(name, r, key, hint) {
  if (!r[key] || typeof r[key] !== 'object') r[key] = { ticks: 8, az: 0, el: 0, reach: 0 };
  const seg = r[key];
  const box = el('div', { class: 'clipprops' });
  box.append(el('span', { class: 'hint', style: 'min-width:52px' }, name));
  box.append(field('ticks',
    numBox(seg, 'ticks', { int: true, min: 1, max: 120, dflt: 8,
                           onChange: () => touched(true) }),
    tickHint(seg.ticks)));
  box.append(field('az', radBox(seg, 'az'),
    name === 'windup' ? '0 straight ahead, + to the mob\'s right'
                      : 'how far the point travels in azimuth'));
  box.append(field('el', radBox(seg, 'el'), '0 level'));
  box.append(field('reach',
    numBox(seg, 'reach', { step: 0.01, min: -1, max: 1,
                           onChange: () => touched(true) }),
    reachHint(seg.reach)));
  wrap.append(box);
  return el('div', { class: 'rignote' }, hint);
}

/**
 * REACH IS A BAND POSITION, and this readout is the reason the field is not
 * just a number. `0` means the neutral 0.60 of the arm's own annulus, not zero
 * voxels — authored against the ARM instead, every chamber and lunge in the
 * shipped library landed outside the band and was clamped: the commanded
 * radius moved 0.15 voxels on a stroke asking for four, and a thrust read as
 * a twitch (strokes.cpp:160).
 */
function reachHint(offset) {
  const a = host?.armInfo?.();
  const o = num(offset, 0);
  const at = MELEE.kNeutralReach + o;
  let s = `band position ${at.toFixed(2)} (0 = fully drawn back, 1 = extended)`;
  if (a && a.bandLo !== undefined && a.bandHi > a.bandLo) {
    const r = a.bandLo + clamp(at, 0, 1) * (a.bandHi - a.bandLo);
    s += ` = ${r.toFixed(2)} vox on this arm`;
    if (at < 0 || at > 1) s += ' — CLAMPED, this stroke cannot reach there';
  }
  return s;
}

/* ---- the stroke timeline ----------------------------------------------- */

/**
 * windup | cut | recover as one proportional bar, with the live phase cursor.
 * The bar is the ONE place the three tick counts are visible against each
 * other, which is the question an author actually has: is the telegraph long
 * enough to read, and is the cut short enough to be fast.
 */
function renderStrokeBar(sty) {
  const st = host?.state?.() || {};
  const w = sty.windup.ticks, c = sty.cut.ticks, rc = sty.recoverTicks;
  const total = Math.max(1, w + c + rc);
  const bar = el('div', {
    class: 'cliplane',
    style: 'width:100%;cursor:default;display:flex;overflow:hidden',
    title: `${total} ticks = ${(total * TICK_MS / 1000).toFixed(2)} s`,
  });
  const seg = (n, frac, colour, tip) => el('div', {
    style: `flex:${frac} 0 0;background:${colour};display:flex;align-items:center;` +
           'justify-content:center;font-size:10px;color:#0b0e13;font-weight:600;' +
           'overflow:hidden;white-space:nowrap',
    title: tip,
  }, n);
  bar.append(
    seg(`windup ${w}`, w, '#6aa9ff', 'the telegraph: driven under commitSpeed, no cut fires'),
    seg(`cut ${c}`, c, '#ffd08a', 'the travel: this is the part that damages'),
    seg(`recover ${rc}`, rc, '#3d4756', 'hand-back: PoseWeight ramps down'));
  // live cursor
  if (st.live) {
    const done = st.phase === 'windup' ? st.phaseTick
      : st.phase === 'cut' ? w + st.phaseTick
      : st.phase === 'recover' ? w + c + st.phaseTick : 0;
    bar.append(el('span', {
      class: 'ccursor',
      style: `left:${clamp(done / total, 0, 1) * 100}%`,
    }));
  }
  bar.id = 'atkStrokeBar';
  bar.dataset.total = String(total);
  bar.dataset.w = String(w);
  bar.dataset.c = String(c);
  wrap.append(el('div', { class: 'clipscrubwrap' },
    el('span', { class: 'hint', style: 'min-width:86px' },
      `${total} t / ${(total * TICK_MS / 1000).toFixed(2)} s`),
    bar));
}

/* ---- the live readout -------------------------------------------------- */

/**
 * WHAT THE DRIVER IS ACTUALLY DOING. Every number here answers a question that
 * a bare "the attack looks wrong" cannot distinguish between — the same
 * argument NpcStroke's three tally words make in strokes.h: "the NPC does not
 * hit very hard" has four causes and from outside all four are the same zero.
 */
function renderReadout() {
  const st = host?.state?.() || {};
  const a = host?.armInfo?.() || {};
  const line = (k, v, tip) => {
    const s = el('span', { class: 'hint', title: tip || '' }, `${k} ${v}`);
    s.dataset.k = k;                    // so tickUI can refill it in place
    return s;
  };
  const box = el('div', { class: 'clipprops', style: 'font-family:var(--mono)' });
  box.id = 'atkReadout';
  if (a.chain < 0 || a.chain === undefined) {
    box.append(el('span', { class: 'hint' },
      '⚠ no weapon arm on this rig: the driver needs a chain tagged "arm" ' +
      'whose effector is the part the weapon socket names. Add one in the ' +
      'Parts panel, then pick a socket and an item.'));
    wrap.append(box);
    return;
  }
  box.append(
    line('arm', a.armName || '?', 'derived from the rig — the chain whose ' +
      'effector is the socket\'s part, never hardcoded'),
    line('hand', a.handSign > 0 ? 'right (.R)' : 'left (.L)',
      'the side decides the asymmetric azimuth stops: azOut to the weapon ' +
      'side, azAcross across the body'),
    line('reach', (a.reach ?? 0).toFixed(2) + ' vox',
      'L1 + L2 off the LIVE bone lengths'),
    line('blade', (a.bladeLen ?? 0).toFixed(2) + ' vox',
      'MEASURED hand-to-point distance of whatever is in the fist — never ' +
      'authored, so a dagger and a greatsword steer the same way'),
    line('band', `[${(a.bandLo ?? 0).toFixed(2)}, ${(a.bandHi ?? 0).toFixed(2)}]`,
      'the annulus of tip radii this arm can serve, given that blade'),
    line('phase', st.phase || 'idle',
      'the STROKE PROGRAM\'s phase; the driver has its own (below)'),
    line('driver', st.meleePhase || 'idle',
      'MeleeState: guard / wind / slash / recover. A windup deliberately ' +
      'stays in GUARD — that is what "under commitSpeed" means.'),
    line('az/el', `${(st.az ?? 0).toFixed(2)} / ${(st.el ?? 0).toFixed(2)}`),
    line('radius', (st.radius ?? 0).toFixed(2)),
    line('weight', (st.weight ?? 0).toFixed(2),
      'PoseWeight: how much of the arm the stroke claims, 0..1'),
    line('tip', (st.tipSpeed ?? 0).toFixed(1) + ' vox/s',
      'SPEED IS THE DAMAGE (melee.h). Below melee.minSpeedMps the sweep does ' +
      'nothing at all.'),
    line('edge', (st.edgeAlign ?? 1).toFixed(2),
      'MeleeEdgeAlign: 1 is a perfectly edge-on cut, melee.edgeFloor is a ' +
      'pure slap with the flat'));
  if (a.blade === false)
    box.append(el('span', { class: 'hint' },
      '· no item held: the point IS the hand, so the band is the bare arm. ' +
      'Load one in the Held item panel to preview a real blade.'));
  wrap.append(box);
}

/**
 * Per-frame refresh of the LIVE parts only — the readout values and the stroke
 * cursor. Deliberately NOT a re-render: rebuilding the panel ten times a
 * second would tear focus out of every number box the author is typing in, and
 * the compass pad would lose its drag. Same reason the clip lane has
 * updateClipCursorUI().
 */
export function tickUI() {
  if (!wrap || !host) return;
  const st = host.state?.() || {};
  const a = host.armInfo?.() || {};
  const box = wrap.querySelector('#atkReadout');
  if (box) {
    const set = (k, v) => {
      const s = box.querySelector(`[data-k="${k}"]`);
      if (s) s.textContent = `${k} ${v}`;
    };
    set('phase', st.phase || 'idle');
    set('driver', st.meleePhase || 'idle');
    set('az/el', `${(st.az ?? 0).toFixed(2)} / ${(st.el ?? 0).toFixed(2)}`);
    set('radius', (st.radius ?? 0).toFixed(2));
    set('weight', (st.weight ?? 0).toFixed(2));
    set('tip', (st.tipSpeed ?? 0).toFixed(1) + ' vox/s');
    set('edge', (st.edgeAlign ?? 1).toFixed(2));
    set('band', `[${(a.bandLo ?? 0).toFixed(2)}, ${(a.bandHi ?? 0).toFixed(2)}]`);
    set('blade', (a.bladeLen ?? 0).toFixed(2) + ' vox');
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

/** True when the Attacks lane is the one on screen (rig.js gates tickUI). */
export const isVisible = () => !!(wrap && wrap.isConnected);

/* ---- per-limb: what an attack may do to each part ---------------------- */

/**
 * THE PER-LIMB HALF. An attack style says where the POINT goes; what each limb
 * is allowed to do getting it there is `poseLimit` on that limb (anim.h:243)
 * plus the three body-wide knobs below. The two live in different files and
 * this section is explicit about which.
 *
 * The engine's own note is worth repeating in the UI: joint limits are a POSE
 * STAGE, not a physics constraint. Jolt's minAngle/maxAngle and swing-twist
 * cone bound a DYNAMIC body, and a live limb is kinematic — so the ragdoll
 * limits sitting in the same sidecar say NOTHING about an IK-driven pose.
 */
function renderLimbSection() {
  const a = host?.armInfo?.() || {};
  wrap.append(el('div', { class: 'tagbar' },
    el('span', { class: 'hint' }, 'per-limb'),
    chip('hide', false, () => { showLimbs = false; render(); })));
  wrap.append(el('div', { class: 'rignote' },
    'JOINT LIMITS ARE A POSE STAGE, NOT A PHYSICS CONSTRAINT. Jolt only ' +
    'enforces a limb\'s <b>minAngle/maxAngle</b> and swing-twist cone on a ' +
    'DYNAMIC body, and a live limb is kinematic — nothing whatsoever stops ' +
    'the IK from raking a thigh out behind the body except <b>poseLimit</b>. ' +
    'These write the RIG sidecar (' + (host?.sidecarName?.() || '&lt;mob&gt;.json') +
    '); the knobs under them write assets/materials/tuning.json.'));

  const parts = a.armParts || [];
  if (!parts.length) {
    wrap.append(el('div', { class: 'rignote' }, 'no arm parts to show.'));
  }
  for (const p of parts) wrap.append(limbLimitRow(p));

  // ---- the body-wide share, from tuning.json --------------------------
  const t = host?.tuning?.();
  if (!t) {
    wrap.append(el('div', { class: 'rignote' },
      'tuning.json is not loaded in this page, so the body-wide melee knobs ' +
      'are read-only defaults here. Open the tuner with its server to edit ' +
      'them alongside the styles.'));
    return;
  }
  if (!t.melee || typeof t.melee !== 'object') t.melee = {};
  const m = t.melee;
  const knob = (key, label, hint, opts) =>
    field(label, numBox(m, key, {
      ...opts,
      onChange: () => { host?.touchTuning?.(); host?.onTuningChanged?.(); render(); },
    }), hint);
  const box = el('div', { class: 'clipprops' });
  box.append(
    knob('torsoShare', 'torso twist', 'fraction of the stroke\'s azimuth the ' +
      'CHEST takes. Capped at ±0.5 rad in code — the cap is the anatomy, this ' +
      'is the taste. 0 is the A/B if a rig ever reads mirrored.',
      { step: 0.01, min: 0, max: 1 }),
    knob('torsoPitch', 'torso pitch', 'fraction of the elevation. Clamps ' +
      'tighter downward: the arm legitimately hangs at -1.5 rad in a low ' +
      'guard and a chest that followed it there would read as a bow.',
      { step: 0.01, min: 0, max: 1 }),
    knob('elbowPoleCone', 'elbow cone', 'radians the elbow\'s bend plane may ' +
      'stray from straight-back. A human elbow trails down, back or out — ' +
      'never forward. An unbounded pole handed to a bounded axis is a solve ' +
      'the clamp then throws away.', { step: 0.05, min: 0.05, max: 3.1 }),
    knob('headClearM', 'head clear (m)', 'the blade stays this far outside the ' +
      'wielder\'s own head sphere — a RIGID translate of the whole segment, so ' +
      '|tip − hand| is preserved. 0 is the off switch.',
      { step: 0.01, min: 0, max: 0.5 }),
    knob('handBackFrac', 'hand-back', 'fraction of reach the hand may go ' +
      'BEHIND the shoulder\'s frontal plane. The tip window keeps the POINT in ' +
      'front; this is what stops the upper arm going behind the torso.',
      { step: 0.01, min: 0, max: 0.6 }),
    knob('azOut', 'az out', 'azimuth stop to the WEAPON side, radians',
      { step: 0.01, min: 0, max: 2.5 }),
    knob('azAcross', 'az across', 'azimuth stop ACROSS the body',
      { step: 0.01, min: 0, max: 2.5 }),
    knob('elMin', 'el min', '', { step: 0.01, min: -1.6, max: 0 }),
    knob('elMax', 'el max', '', { step: 0.01, min: 0, max: 1.6 }),
    knob('commitSpeed', 'commit speed', 'input units/s above which a Wind ' +
      'becomes a Slash. A windup drives at 480 units/s against this, which is ' +
      'why it stays a telegraph however far it has to travel.',
      { step: 10, min: 1 }));
  wrap.append(box);
}

/**
 * One limb's pose limit, in whichever of the three shapes it is authored in.
 * Editing writes the RIG sidecar in place, exactly as the Parts panel does, so
 * the ordinary model save picks it up.
 */
function limbLimitRow(p) {
  const row = el('div', { class: 'clipprops' });
  row.append(el('span', { class: 'hint', style: 'min-width:70px' }, p.name));
  const limb = host?.limbByName?.(p.name);
  if (!limb) {
    row.append(el('span', { class: 'hint' }, '(not in this rig\'s limbs[])'));
    return row;
  }
  const touchRig = () => { host?.touchSidecar?.(); host?.rebuild?.(); render(); };
  const pl = limb.poseLimit;
  if (!pl) {
    row.append(el('span', { class: 'hint' },
      'no poseLimit — this joint may be posed to ANY angle by the IK'),
      chip('+ hinge', false, () => {
        limb.poseLimit = { axis: [-1, 0, 0], min: 0, max: 130, hinge: true };
        touchRig();
      }, 'one DOF, off-axis swing DISCARDED — the elbow form'),
      chip('+ axis', false, () => {
        limb.poseLimit = { axis: [-1, 0, 0], min: -20, max: 85 };
        touchRig();
      }, 'clamp the twist about one axis, leave the swing — the hip/knee form'),
      chip('+ ball', false, () => {
        limb.poseLimit = { bone: [0, -1, 0],
          reach: [{ normal: [0, 0, -1], max: 50 }, { normal: [-1, 0, 0], max: 30 }],
          twist: { min: -75, max: 75 } };
        touchRig();
      }, 'bound where the bone may POINT plus a roll bound — the shoulder ' +
         'form, and the one a mouse-aimed arm needs'));
    return row;
  }
  if (pl.bone !== undefined) {
    // ---- ball form
    row.append(el('span', { class: 'hint' }, 'ball'));
    row.append(field('bone', vecBox(pl, 'bone', touchRig),
      'the bone direction in this part\'s own REST frame'));
    const reach = Array.isArray(pl.reach) ? pl.reach : (pl.reach = []);
    reach.forEach((r, i) => {
      row.append(field(`plane ${i}`, el('span', { class: 'rigvec' },
        vecBox(r, 'normal', touchRig),
        numBox(r, 'max', { step: 1, min: -90, max: 90, onChange: touchRig })),
        'at most N degrees past this plane; the plane itself is 0. The two ' +
        'normals must be PERPENDICULAR — the closed-form projection is only ' +
        'the nearest legal direction when they are, and a tilted pair is a ' +
        'load error in the engine.'));
    });
    if (reach.length < 2)
      row.append(chip('+ plane', false, () => {
        reach.push({ normal: [0, 0, -1], max: 50 }); touchRig();
      }));
    if (reach.length)
      row.append(el('button', { class: 'small danger',
        onclick: () => { reach.pop(); touchRig(); } }, '− plane'));
    if (!pl.twist) pl.twist = { min: -75, max: 75 };
    row.append(field('twist °', el('span', { class: 'rigvec' },
      numBox(pl.twist, 'min', { step: 1, min: -180, max: 180, onChange: touchRig }),
      numBox(pl.twist, 'max', { step: 1, min: -180, max: 180, onChange: touchRig })),
      'roll about the bone. The weapon arm\'s elbow-plane override is bounded ' +
      'by this cone, so a tight twist range makes the elbow stiffer in a cut.'));
  } else {
    // ---- axis / hinge form
    const hinge = !!pl.hinge;
    row.append(chip(hinge ? 'hinge' : 'axis', true, () => {
      pl.hinge = !hinge; touchRig();
    }, hinge ? 'ONE DOF: the off-axis swing is discarded. Click for the axis form.'
             : 'clamp the twist about the axis, leave the swing. Click for hinge.'));
    row.append(field('axis', vecBox(pl, 'axis', touchRig),
      'in the part\'s own REST frame. For the weapon arm this is the plane the ' +
      'driver STEERS — the range is never touched, only which plane the one ' +
      'degree of freedom lives in.'));
    row.append(field('min °', numBox(pl, 'min',
      { step: 1, min: -180, max: 180, onChange: touchRig })));
    row.append(field('max °', numBox(pl, 'max',
      { step: 1, min: -180, max: 180, onChange: touchRig })));
  }
  row.append(el('button', {
    class: 'small danger', title: 'remove the limit entirely',
    onclick: () => { delete limb.poseLimit; touchRig(); },
  }, '✕'));
  return row;
}

function vecBox(obj, key, after) {
  const wrapEl = el('div', { class: 'rigvec' });
  if (!Array.isArray(obj[key])) obj[key] = [0, 0, 0];
  for (let k = 0; k < 3; k++) {
    const i = el('input', { class: 'cell num', type: 'number', step: '0.1' });
    i.value = String(num(obj[key][k], 0));
    i.addEventListener('change', () => {
      obj[key][k] = num(i.value, 0);
      after();
    });
    wrapEl.append(i);
  }
  return wrapEl;
}

/* ---- the player's flick compass ---------------------------------------- */

/**
 * THE FLICK COMPASS (strokes.h:105 / the `player` block).
 *
 * `dir` is a unit-ish direction in SCREEN space (+x right, +y DOWN, raw
 * mouse), quantized by MAX DOT PRODUCT at the attack press — so the sectors do
 * not tile the circle, they compete for it, and adding one is adding a line.
 * The pad below draws the resulting partition by actually asking
 * QuantizeStrike, rather than by drawing wedges from the angles: a max-dot
 * partition is not the wedges you would guess once the directions are not
 * evenly spaced, and guessing is how a sector ends up unreachable.
 */
function renderCompass() {
  wrap.append(el('div', { class: 'tagbar' },
    el('span', { class: 'hint' }, 'player flick compass'),
    el('span', { class: 'hint' },
      '(melee.controlMode 0 — the click-to-strike layer)'),
    chip('hide', false, () => { showCompass = false; render(); })));

  const pj = raw.player || (raw.player = { sectors: [], neutralAlternate: [] });
  if (!Array.isArray(pj.sectors)) pj.sectors = [];

  const row = el('div', { style: 'display:flex;gap:14px;align-items:flex-start;flex-wrap:wrap' });

  // ---- the pad
  const SZ = 168;
  const pad = el('canvas', { id: 'atkCompass', width: String(SZ), height: String(SZ),
    style: 'border:1px solid #262e3c;border-radius:6px;background:#0f131a;cursor:crosshair',
    title: 'drag to test a flick — the label under the pad names the style the ' +
           'engine would pick' });
  drawCompass(pad, SZ);
  const testLbl = el('div', { class: 'hint', style: 'font-family:var(--mono)' });
  const updateTest = () => {
    const i = MELEE.quantizeStrike(lib, flick.x, flick.y);
    const n = i >= 0 ? lib.styles[i].name : '(no sector — a click with no flick '
      + 'alternates the neutral pair)';
    testLbl.textContent = `flick (${flick.x.toFixed(2)}, ${flick.y.toFixed(2)}) → ${n}`;
  };
  updateTest();
  const padPoint = ev => {
    const r = pad.getBoundingClientRect();
    const x = (ev.clientX - r.left) / SZ * 2 - 1;
    const y = (ev.clientY - r.top) / SZ * 2 - 1;
    const l = Math.hypot(x, y);
    flick = l > 1e-4 ? { x: x / l, y: y / l } : { x: 1, y: 0 };
    drawCompass(pad, SZ);
    updateTest();
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

  // ---- the sector list
  const list = el('div', { style: 'flex:1;min-width:280px' });
  pj.sectors.forEach((sec, i) => {
    if (!Array.isArray(sec.dir)) sec.dir = [1, 0];
    const r = el('div', { class: 'clipprops', style: 'margin:3px 0' });
    r.append(el('span', { class: 'hint', style: 'min-width:34px' }, dirName(sec.dir)));
    const dx = numBox(sec.dir, 0, { step: 0.1, min: -1, max: 1,
                                    onChange: () => touched(true) });
    const dy = numBox(sec.dir, 1, { step: 0.1, min: -1, max: 1,
                                    onChange: () => touched(true) });
    r.append(field('dir', el('span', { class: 'rigvec' }, dx, dy),
      '+x right, +y DOWN — raw screen mouse'));
    r.append(field('style', styleSelect(sec.style, v => {
      sec.style = v; touched(true);
    })));
    r.append(el('button', { class: 'small danger', onclick: () => {
      pj.sectors.splice(i, 1); touched(true);
    } }, '✕'));
    list.append(r);
  });
  list.append(el('div', { class: 'rigbtns' },
    chip('+ sector', false, () => {
      pj.sectors.push({ dir: [0, -1], style: lib.styles[selected]?.name ||
                        lib.styles[0]?.name || '' });
      touched(true);
    }, 'a new flick direction. It competes for the whole circle by max dot — ' +
       'check the pad afterwards that every sector is still reachable.')));

  // ---- the neutral pair
  if (!Array.isArray(pj.neutralAlternate)) pj.neutralAlternate = [];
  const nr = el('div', { class: 'clipprops' });
  nr.append(el('span', { class: 'hint', style: 'min-width:70px' }, 'no flick'));
  for (let k = 0; k < 2; k++)
    nr.append(field(k ? 'then' : 'first',
      styleSelect(pj.neutralAlternate[k], v => {
        pj.neutralAlternate[k] = v; touched(true);
      })));
  nr.append(el('span', { class: 'hint' },
    'a click below melee.pickMinSpeed alternates these two'));
  list.append(nr);
  row.append(list);
  wrap.append(row);

  const unreachable = pj.sectors
    .map(s => s.style)
    .filter(name => {
      const i = lib.styles.findIndex(x => x.name === name);
      if (i < 0) return false;
      // Sample the circle and see whether this style ever wins the max dot.
      for (let k = 0; k < 720; k++) {
        const a = k / 720 * Math.PI * 2;
        if (MELEE.quantizeStrike(lib, Math.cos(a), Math.sin(a)) === i) return false;
      }
      return true;
    });
  if (unreachable.length)
    wrap.append(el('div', { class: 'rignote', style: 'color:var(--amber)' },
      '⚠ unreachable by any flick: ' + unreachable.join(', ') +
      ' — another sector wins the max dot everywhere. Move its `dir` apart ' +
      'from its neighbours, or drop it.'));
}

const dirName = d => {
  const x = num(d[0], 0), y = num(d[1], 0);
  const a = Math.atan2(y, x) * DEG;
  const names = ['→', '↘', '↓', '↙', '←', '↖', '↑', '↗'];
  return names[(Math.round(a / 45) + 8) % 8];
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
    const am = (a0 + a1) / 2;
    const i = MELEE.quantizeStrike(lib, Math.cos(am), Math.sin(am));
    g.beginPath();
    g.moveTo(SZ / 2, SZ / 2);
    g.arc(SZ / 2, SZ / 2, R, a0, a1);
    g.closePath();
    g.fillStyle = i >= 0 ? palette[i % palette.length] : '#1a212c';
    g.fill();
  }
  // sector directions
  g.lineWidth = 2;
  for (const s of lib.player.sectors) {
    const l = Math.hypot(s.x, s.y) || 1;
    g.strokeStyle = '#dfe6f2';
    g.beginPath();
    g.moveTo(SZ / 2, SZ / 2);
    g.lineTo(SZ / 2 + s.x / l * R, SZ / 2 + s.y / l * R);
    g.stroke();
  }
  // the test flick
  g.strokeStyle = '#ffd08a';
  g.lineWidth = 3;
  g.beginPath();
  g.moveTo(SZ / 2, SZ / 2);
  g.lineTo(SZ / 2 + flick.x * R, SZ / 2 + flick.y * R);
  g.stroke();
  // "+y is DOWN" is the thing everybody gets wrong; label it on the picture.
  g.fillStyle = '#7c8798';
  g.font = '9px ui-monospace, monospace';
  g.fillText('up', SZ / 2 - 6, 10);
  g.fillText('down (thrust)', SZ / 2 - 30, SZ - 3);
}

/* ---- the loader's own log ---------------------------------------------- */

function renderLoaderLog() {
  if (!lib?.log?.length) return;
  wrap.append(el('div', { class: 'rignote', style: 'color:var(--amber)' },
    'the ENGINE\'s loader would skip these (a bad entry is never fatal): ' +
    lib.log.join(' · ')));
}
