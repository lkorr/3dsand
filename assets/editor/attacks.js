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
// THE TARGET, as the character sees it: `az`/`el` are its bearing from the
// EYES (0/0 = straight ahead of the face, where a player's crosshair is) and
// `dist` how far off it is, world voxels. rig.js turns that into a POINT and
// the program gets the point's bearing and distance about the shoulder
// (strokes.cpp StrokeAimAt), exactly as the engine aims at a real target. It
// used to be the bearing about the shoulder itself, so 0/0 was a target
// straight out from the RIGHT shoulder rather than in front of the character.
let aim = { az: 0, el: 0, dist: 6 };
// Draw the target orb (rig.js drawAimOrb).
let orbOn = true;
let loop = true;
let swingNo = 0;
let trailOn = true;
// DOES THE LOOP RE-ROLL THE JITTER EVERY CYCLE? Off by default, and that is a
// change: it used to, unconditionally. `jitter.az` runs to 0.24 rad and
// `jitter.tempo` to 0.28 on the shipped NPC styles, so every replay was a
// visibly different swing and the recover you were trying to author moved
// under you once every second and a half. That is the reported "trying to solo
// certain animations gives different animations randomly" — it was the
// deterministic sequence being walked, one draw per loop, with nothing saying
// so. Authoring wants ONE swing repeated; `reroll` still steps the sequence
// deliberately, and this chip puts the old behaviour back for watching the
// spread.
let vary = false;
let weaponMode = 'long'; // 'unarmed' | 'short' | 'long' | 'blunt'
let flick = { x: 1, y: 0 }; // the compass pad's test flick
const folds = { limbs: false, compass: false, help: false };
// SOLO: which segment the preview isolates — 'all' runs the program as the
// game does; 'windup' / 'cut' / 'recover' burst through the segments before
// the chosen one, play it at speed, then HOLD its end pose for a beat before
// looping. It exists because a stroke is three motions that each depend on
// where the previous one left the arm, and watching all three at once is how
// "windup az" gets blamed for what the seeded start pose did.
let solo = 'all';
// GOAL FRAME: null, or the key of the destination the preview is HOLDING
// (melee.js strokeGoals). A held goal and a live program are mutually
// exclusive — the arm is either being driven or being posed, and showing both
// at once would mean neither number on screen was the one in the file.
let goalKey = null;
// MANUAL ADJUSTMENT MODE: with a frame held (goalKey), drag handles on the rig
// (rig.js drawPoseHandles) and the frame's numbers follow. `manualBefore` is
// the document as it was when the current drag began, for its single undo.
let manual = false;
let manualBefore = null;
// The shared clip library (assets/anims/*.json), for the program card's clip
// picker. null until fetched; [] when the server has none.
let libClips = null;
let itemListFetched = false;

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

// SLOW x10: the preview's view of the library with every frame's ticks and
// the release multiplied, so a swing plays ten times longer AND stays smooth
// (a keyed frame is a slerp on t, so more ticks is the same path in finer
// steps). Only the rig's preview reads through here; the panels, the saved
// file and the game never see it. A `chase` frame arrives on its per-tick
// rate, so it is NOT stretched the same way -- keyed frames are exact.
const SLOW_FACTOR = 10;
let slow = false;
let slowSrc = null, slowLib = null;
export const slowMotion = () => slow;
export const library = () => {
  if (!slow || !lib) return lib;
  if (slowSrc !== lib) {
    slowSrc = lib;
    slowLib = { ...lib, styles: lib.styles.map(s => ({
      ...s,
      frames: s.frames.map(f => ({ ...f, ticks: f.ticks * SLOW_FACTOR })),
      release: Math.max(1, s.release) * SLOW_FACTOR,
    })) };
  }
  return slowLib;
};
// The document itself, for the bake harness (assets/_bake_poses.html).
export const rawDoc = () => raw;
export const styleIndex = () => selected;
export const currentAim = () => aim;
export const aimOrbEnabled = () => orbOn;
export const looping = () => loop;
export const trailEnabled = () => trailOn;
export const swingNumber = () => swingNo;
export const varying = () => vary;
export const isDirty = () => dirty;
export const currentWeaponMode = () => weaponMode;
export const soloSegment = () => solo;
// The destination the preview is holding, or null. Read by rig.js's weaponTick
// (which poses the arm on it instead of stepping the program) and by
// previewActive (a held frame has nothing live on it).
export const goalFrame = () => goalKey;
export const manualMode = () => manual && !!goalKey;
/** A drag begins: remember the document for one undo entry. */
export function manualBegin() { manualBefore = raw ? snap(raw) : null; }
/**
 * Mutate the HELD frame's raw object (created in this style's frame list if
 * needed) and re-derive the library WITHOUT re-rendering the panel — this runs
 * on every pointermove of a drag.
 */
export function manualEdit(fn) {
  if (!raw || !lib || !goalKey) return;
  const sty = lib.styles[selected];
  const k = +goalKey.slice(1);
  if (!sty || !(k >= 0)) return;
  const d = framesDoc(sty, true);
  if (!d[k]) return;
  fn(d[k], sty.frames[k]);
  dirty = true;
  lib = MELEE.parseStyleLibrary(raw);
  host?.onStylesChanged?.();
}
/** A drag ends: one undo entry for the whole drag, then repaint the panel. */
export function manualEnd(label) {
  if (!raw) return;
  const before = manualBefore, after = snap(raw);
  manualBefore = null;
  if (before && JSON.stringify(before) !== JSON.stringify(after)) {
    const apply = s => { pourInto(raw, s); afterEdit('styles'); };
    host?.pushUndo?.(label || 'manual pose', () => apply(before), () => apply(after));
  }
  render();
}
export function nextSwing() { swingNo = (swingNo + 1) >>> 0; }

/* ==========================================================================
   BAKING A STYLE INTO POSES (melee.js "A FRAME IS A POSE")

   A style still spelled as tip targets is turned into pose frames by holding
   each of its frames the old way — the stroke driver run to the end of that
   frame, exactly what "show" displayed — and reading the arm the rig drew
   (rig.js captureHeldPose). Every frame is captured BEFORE any is written,
   because the old way reaches frame k by running frames 0..k-1 from their
   targets, and a written frame no longer has one.

   The jitter is zeroed while capturing: a pose is what was authored, not one
   draw of it, and the keyed runner adds the bow back per swing. A `chase`
   frame becomes quadInOut — chase has no meaning between two poses.
   ========================================================================== */
const DRIVER_KEYS = ['az', 'el', 'reach', 'lean', 'leanAngle', 'wrist', 'bladeAngle',
                     'elbow', 'chaseRate', 'torsoTwist', 'torsoPitch'];

/** Bake style `i` (every frame must still be a target). true if it wrote. */
export function bakeStyle(i) {
  const sty = lib && lib.styles[i];
  // Never a `:player` copy: the style bar hides those, so frames baked into
  // `player.frames` silently replaced every edit the author made to the base
  // for the player's own swings (2026-09-26).
  if (!sty || (sty.derived && !sty.form) || !sty.frames.length || sty.frames.some(f => f.pose)) return false;
  const keep = { selected, goalKey, manual };
  host?.stop?.();
  selected = i;
  manual = false;
  const jitter = sty.jitter;
  sty.jitter = { az: 0, el: 0, tempo: 0 };
  // A CUT FRAME IS SPLIT AT ITS MIDDLE. Two poses slerp by the SHORTEST joint
  // path between them, and the old driver's cut went round the front (it
  // interpolated the tip's bearing); from its two ends alone a horizontal cut
  // went over the top instead and missed. The middle key puts it in front.
  const poses = [], mids = [];
  try {
    for (let k = 0; k < sty.frames.length; k++) {
      const f = sty.frames[k];
      goalKey = 'f' + k;
      const p = host?.captureHeldPose?.(true);
      if (!p) return false;
      poses.push(p);
      // The middle is built from the two ends (rig.js bakeMidPose), NOT
      // sampled from the old driver halfway: its wrist was mid-spin there,
      // and baking it baked the spin back in (83 degrees a tick, measured).
      mids.push(f.cuts && f.ticks >= 4 && k > 0
        ? host?.bakeMidPose?.(poses[k - 1], p) || null : null);
    }
  } finally {
    sty.jitter = jitter;
    ({ selected, goalKey, manual } = keep);
  }
  const d = framesDoc(sty, true);
  const out = [];
  poses.forEach((p, k) => {
    const fr = d[k] || {};
    for (const key of DRIVER_KEYS) delete fr[key];
    if (fr.ease === 'chase') fr.ease = 'quadInOut';
    if (mids[k]) {
      const t = Math.max(1, Math.round(+fr.ticks || sty.frames[k].ticks));
      const half = Math.floor(t / 2);
      const a = { ...JSON.parse(JSON.stringify(fr)), name: (fr.name || 'cut') + ' a',
                  ticks: half, pose: MELEE.poseToJson(mids[k]) };
      const b = { ...fr, name: (fr.name || 'cut') + ' b', ticks: t - half,
                  pose: MELEE.poseToJson(p) };
      out.push(a, b);
    } else {
      fr.pose = MELEE.poseToJson(p);
      out.push(fr);
    }
  });
  d.length = 0;
  d.push(...out.slice(0, MELEE.MAX_FRAMES));
  lib = MELEE.parseStyleLibrary(raw);
  dirty = true;
  return true;
}

/* ==========================================================================
   DERIVING A STYLE: MIRRORED AND/OR REVERSED (the generated diagonals)

   `mirror` reflects every pose across the body's midline on the SAME arm
   (rig.js mirrorPose): a forehand becomes a backhand. `reverse` runs the
   stroke backwards: the windup goes to where the cut ENDED, the cut keys play
   in reverse order (each segment keeps its own ticks and ease, so the pacing
   is the original's played backwards) and ends where the windup was; the
   recover frames are kept. Frame slots — names, `cuts`, `from`, ticks of the
   windup and recover — stay where they were. Both apply to the default frames
   and to every weapon form. Keyed styles only. A starting point to tweak, not
   a finished stroke.
   ========================================================================== */
function reverseFrames(frames) {
  const cutIdx = frames.map((f, k) => (f.cuts ? k : -1)).filter(k => k >= 0);
  if (!cutIdx.length || cutIdx[0] === 0) return frames;
  const first = cutIdx[0], last = cutIdx[cutIdx.length - 1];
  const keys = frames.slice(first - 1, last + 1);          // windup end + cuts
  const out = frames.map(f => JSON.parse(JSON.stringify(f)));
  // One windup: everything before the last windup key folds into it.
  const wind = out.slice(0, first);
  const w = wind[wind.length - 1];
  w.ticks = wind.reduce((a, f) => a + (+f.ticks || 0), 0);
  w.pose = JSON.parse(JSON.stringify(keys[keys.length - 1].pose));
  const m = keys.length - 1;
  for (let j = 1; j <= m; j++) {
    const f = out[first + j - 1];
    const seg = keys[m - j + 1];                            // segment played backwards
    f.pose = JSON.parse(JSON.stringify(keys[m - j].pose));
    f.ticks = seg.ticks;
    if (seg.ease) f.ease = seg.ease; else delete f.ease;
  }
  // The recover keeps its slot but not its pose: the original's settled
  // where the ORIGINAL cut ended, which is now where this one began. Going
  // all the way back there whipped the blade at 100+ degrees a tick, and
  // holding the new end left the whole trip to the release (~100 too, both
  // measured with scripts/trace_attacks.sh); halfway by bearing splits it.
  const endPose = MELEE.readPose(keys[0].pose);
  for (let k = last + 1; k < out.length; k++) {
    if (!out[k].pose) continue;
    const mid = host?.bakeMidPose?.(endPose, MELEE.readPose(out[k].pose));
    out[k].pose = mid ? MELEE.poseToJson(mid) : JSON.parse(JSON.stringify(keys[0].pose));
  }
  return [w, ...out.slice(first)];
}

/**
 * New style `name` from style `srcName`, inserted after it. opts: { mirror,
 * reverse, label }. Returns true when it wrote.
 */
export function deriveStyle(srcName, name, opts = {}) {
  if (!raw || !lib || lib.styles.some(s => s.name === name)) return false;
  const src = raw.styles.find(s => s.name === srcName);
  if (!src || !Array.isArray(src.frames) || !src.frames.every(f => f.pose)) return false;
  const copy = snap(src);
  copy.name = name;
  if (opts.label) copy.label = opts.label;
  const conv = (frames) => {
    let fr = frames.map(f => JSON.parse(JSON.stringify(f)));
    if (opts.mirror) {
      for (const f of fr) {
        const p = host?.mirrorPose?.(MELEE.readPose(f.pose));
        if (!p) return null;
        f.pose = MELEE.poseToJson(p);
      }
    }
    if (opts.reverse) fr = reverseFrames(fr);
    return fr;
  };
  const main = conv(copy.frames);
  if (!main) return false;
  copy.frames = main;
  for (const form of Object.values(copy.forms || {})) {
    if (!form || !Array.isArray(form.frames) || !form.frames.every(f => f.pose)) continue;
    const fr = conv(form.frames);
    if (fr) form.frames = fr;
  }
  editStyles('derive "' + name + '"', () => {
    raw.styles.splice(raw.styles.findIndex(x => x.name === srcName) + 1, 0, copy);
  });
  return true;
}

/** Bake every style that can be (an aim effector, the jaws, cannot). */
export function bakeAll() {
  if (!lib) return [];
  const done = [];
  const before = raw ? snap(raw) : null;
  for (let i = 0; i < lib.styles.length; i++) {
    const name = lib.styles[i].name;
    if (bakeStyle(i)) done.push(name);
  }
  if (before && done.length) {
    const after = snap(raw);
    const apply = s => { pourInto(raw, s); afterEdit('styles'); };
    host?.pushUndo?.('bake poses', () => apply(before), () => apply(after));
  }
  afterEdit('styles');
  return done;
}

async function refreshLibClips() {
  try {
    const r = await fetch('/api/models', { cache: 'no-store' });
    const j = await r.json();
    libClips = (j.files || []).filter(f => f.dir === 'anims' && /\.json$/i.test(f.name))
      .map(f => f.name.replace(/\.json$/i, ''));
  } catch { libClips = []; }
}
/**
 * Select a style by index. THE ONE PATH — the chips in the style bar call this
 * too, and that is the fix rather than an accident of refactoring.
 *
 * Clicking a chip used to be `selected = i; render()` and nothing else. The
 * preview loop, meanwhile, holds its OWN index (`strokeStyle` in rig.js) and
 * re-begins THAT one at the end of every cycle — so selecting a different
 * attack while the preview was running repainted the panel around a rig that
 * went on swinging the old style forever. That is the reported "clicking on
 * different animations doesn't switch them". The solo chips had the restart
 * line from the start; the style chips never did.
 */
export async function selectStyle(i) {
  if (!lib || i < 0 || i >= lib.styles.length) return;
  i = formIndex(i);
  if (i === selected) return;
  selected = i;
  // Restart on the NEW style if something is swinging, so the viewport agrees
  // with the panel. Not `nextSwing()`: switching attacks is not asking for a
  // different draw of the jitter, and re-rolling here would make comparing two
  // styles a comparison of two random swings.
  if (host?.state?.().live) await host.begin(selected);
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
    // AN OLD-FRAME FILE IS CONVERTED HERE, in the document the panel edits
    // (strokes.h "THE STROKE FRAME"), so the boxes show the new meaning —
    // windup measured from the target — and a save writes it. The swing is
    // unchanged; only the numbers that describe it move.
    const migrated = MELEE.migrateRawToTargetFrame(raw) |
                     MELEE.migrateRawToFrames(raw);
    lib = MELEE.parseStyleLibrary(raw);
    err = '';
    dirty = migrated;
    if (migrated)
      toast('attack_styles.json converted to the target frame (windup 0/0 = ' +
            'at the target) — the swings are unchanged; save to keep it', false);
    await refreshLibClips();
    if (selected < 0 && lib.styles.length) selected = 0;
    selected = formIndex(selected);
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

  renderWeaponModeBar();
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

/* ---- weapon mode: unarmed / short / long / blunt ----------------------- */

function activeCompassMap() {
  if (!lib) return null;
  return weaponMode === 'unarmed' ? lib.playerUnarmed : lib.player;
}

function activeCompassKey() {
  return weaponMode === 'unarmed' ? 'playerUnarmed' : 'player';
}

function activeCompassRaw() {
  if (!raw) return null;
  const key = activeCompassKey();
  if (!raw[key] || typeof raw[key] !== 'object')
    raw[key] = { sectors: [], neutralAlternate: [] };
  return raw[key];
}

// The weapon FORMS (strokes.h WEAPON FORMS) plus the empty fist. Picking one
// equips a weapon of that class in the preview and switches every held-weapon
// style to that weapon's version of it (formIndex).
const WEAPON_MODES = [
  { id: 'unarmed', label: 'Unarmed' },
  { id: 'short',   label: 'Short' },
  { id: 'long',    label: 'Long' },
  { id: 'blunt',   label: 'Blunt' },
];

const WEAPON_FOR_MODE = { unarmed: null, short: 'dagger', long: 'sword', blunt: 'mace' };

// The style the panel edits for style index `i` under the current weapon:
// that weapon's form of it for a held-weapon style, else the style itself.
function formIndex(i) {
  const s = lib && lib.styles[i];
  if (!s) return i;
  const b = s.form ? s.baseIndex : i;
  const base = lib.styles[b];
  if (!base || base.weapon !== 'held' || !MELEE.WEAPON_FORMS.includes(weaponMode)) return b;
  const f = lib.styles.findIndex(x => x.name === base.name + '@' + weaponMode);
  return f >= 0 ? f : b;
}
// ...and back: the authored style a form belongs to.
const baseIndexOf = (i) => { const s = lib && lib.styles[i]; return s && s.form ? s.baseIndex : i; };

function renderWeaponModeBar() {
  const bar = el('div', { class: 'tagbar', style: 'margin-bottom:2px' });
  for (const m of WEAPON_MODES)
    bar.append(chip(m.label, weaponMode === m.id, async () => {
      if (weaponMode === m.id) return;
      weaponMode = m.id;
      selected = formIndex(selected);
      const want = WEAPON_FOR_MODE[m.id];
      if (want) await host?.equipWeapon?.(want);
      else await host?.equipWeapon?.(null, true);
      render();
    }, m.id === 'unarmed' ? 'empty hands — punches'
     : m.id === 'short'   ? 'short blades — dagger, shortsword (weaponClass "short")'
     : m.id === 'long'    ? 'long blades — sword, cleaver (weaponClass "long")'
     :                       'blunt — mace (weaponClass "blunt")'));
  wrap.append(bar);
}

/* ---- the style list ---------------------------------------------------- */

function renderStyleBar() {
  const bar = el('div', { class: 'tagbar' });
  // Hide derived `:player` entries — they are auto-generated from the base
  // style's `player` override block; the author edits the base.
  const authored = lib.styles.map((s, i) => [s, i]).filter(([s]) => !s.derived);
  for (const [s, i] of authored)
    bar.append(chip(s.name, i === baseIndexOf(selected),
      () => selectStyle(i), s.label));

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
  const base = lib.styles[baseIndexOf(selected)] || lib.styles[0];
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
  selected = formIndex(lib.styles.findIndex(s => s.name === n));
  render();
}

function renameStyle() {
  const s = lib.styles[baseIndexOf(selected)];
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
    for (const mapKey of ['player', 'playerUnarmed']) {
      const m = raw[mapKey];
      if (!m) continue;
      for (const sec of (m.sectors || [])) if (sec.style === old) sec.style = n;
      const na = m.neutralAlternate;
      if (Array.isArray(na)) for (let k = 0; k < na.length; k++) if (na[k] === old) na[k] = n;
    }
  });
  toast(`renamed — check assets/mobs/behaviors.json for "${old}"`);
}

function dupStyle() {
  const s = lib.styles[baseIndexOf(selected)];
  let n = s.name + '_2';
  for (let k = 2; lib.styles.some(x => x.name === n); k++) n = s.name + '_' + k;
  const copy = snap(s.raw);
  copy.name = n;
  editStyles('duplicate "' + s.name + '"', () => {
    raw.styles.splice(raw.styles.findIndex(x => x.name === s.name) + 1, 0, copy);
  });
  selected = formIndex(lib.styles.findIndex(x => x.name === n));
  render();
}

function deleteStyle() {
  const s = lib.styles[baseIndexOf(selected)];
  let refs = 0;
  for (const mapKey of ['player', 'playerUnarmed']) {
    const m = raw[mapKey];
    if (!m) continue;
    refs += (m.sectors || []).filter(x => x.style === s.name).length;
    refs += (m.neutralAlternate || []).filter(x => x === s.name).length;
  }
  if (!confirm(`Delete "${s.name}"?` +
    (refs ? `\n\n${refs} reference(s) in this file's player compasses go with it.` : '') +
    '\n\nUndo (Ctrl+Z) brings it back. A behaviour profile that still names it ' +
    'gets a loud skip and its first available style, never a crash.')) return;
  editStyles('delete "' + s.name + '"', () => {
    raw.styles.splice(raw.styles.findIndex(x => x.name === s.name), 1);
    for (const mapKey of ['player', 'playerUnarmed']) {
      const m = raw[mapKey];
      if (!m) continue;
      m.sectors = (m.sectors || []).filter(x => x.style !== s.name);
      m.neutralAlternate = (m.neutralAlternate || []).filter(x => x !== s.name);
    }
  });
}

/* ---- PER-SEGMENT PACING (strokes.h "PER-SEGMENT PACING") ----------------
 *
 * One dropdown at the end of every segment row. The names are anim.h's ease
 * curves (the same eight a clip keyframe interpolates with), so a name means
 * one thing across the project. What each reads as, because "cubicInOut" is a
 * formula and not a description of a sword:
 * ------------------------------------------------------------------------ */
const EASE_FEEL = {
  linear: 'even speed throughout',
  instant: 'holds, then arrives on the last tick',
  quadIn: 'back-loaded: fastest at the END of the segment',
  quadOut: 'front-loaded: snaps, then settles',
  quadInOut: 'slow at both ends, fast through the middle',
  cubicIn: 'back-loaded, harder',
  cubicOut: 'front-loaded, harder',
  cubicInOut: 'slow ends, very fast middle',
};

/**
 * A pacing <select>. `chase` adds the "chase" option (the windup's and the
 * settle's historical closed-loop drive, which is what an absent key means for
 * those two); `get` returns the stored name or '' for absent.
 */
function paceCell({ get, set, chase, title, disabled }) {
  const sel = el('select', {
    class: 'small', title: title || '',
    style: 'width:100%;min-width:74px;font-family:var(--mono);font-size:11px',
  });
  if (chase)
    sel.append(el('option', {
      value: '', title: 'the closed-loop chase: close on the pose at a capped ' +
        'rate, arriving whenever it arrives — the behaviour before per-segment ' +
        'pacing',
    }, 'chase'));
  for (const k of MELEE.EASES)
    sel.append(el('option', { value: k, title: EASE_FEEL[k] || '' }, k));
  const cur = get();
  sel.value = cur;
  if (disabled) sel.disabled = true;
  sel.addEventListener('change', () => {
    if (sel.value === cur) return;
    set(sel.value);
  });
  return el('div', { class: 'atkcell' }, sel,
    el('em', { title: 'what this curve does to the segment\'s speed' },
      cur ? (EASE_FEEL[cur] || '').split(':')[0] : (chase ? 'closed loop' : '')));
}

/* ---- THE ANGLE GUIDE: which segment row the author is working on --------
 *
 * The rig viewport draws the target line, the segment's az and el as arcs
 * and their values as labels (rig.js drawAngleGuide) while the pointer or
 * the keyboard focus is on a row's az / el / reach box, and for a couple of
 * seconds after, so an edit — which re-renders the panel and drops focus —
 * does not blink the picture away.
 * ------------------------------------------------------------------------ */
let guide = { key: null, until: 0 };
export function guideKey() {
  return guide.key && performance.now() < guide.until ? guide.key : null;
}
function guideCell(cell, key) {
  cell.dataset.guide = key;
  const hold = () => { guide = { key, until: Infinity }; };
  const linger = () => {
    if (guide.key === key) guide = { key, until: performance.now() + 2500 };
  };
  cell.addEventListener('mouseenter', hold);
  cell.addEventListener('focusin', hold);
  cell.addEventListener('input', hold);
  cell.addEventListener('mouseleave', () => {
    if (!cell.contains(document.activeElement)) linger();
  });
  cell.addEventListener('focusout', linger);
  cell.addEventListener('change', linger);
  return cell;
}

/* ---- the joint brakes --------------------------------------------------- */

const JOINT_TIP = {
  shoulder: 'the UPPER ARM relative to the torso — the one that spins when ' +
    'the elbow pole swings round between windup and cut',
  elbow: 'the FOREARM relative to the upper arm — the bend',
  wrist: 'the HAND relative to the forearm — the blade\'s lay and roll',
};

/** Refresh the "lag now" column from the preview (called per frame by the rig). */
export function updateJointLag(lag) {
  if (!wrap) return;
  for (let k = 0; k < MELEE.ARM_JOINTS.length; k++) {
    const c = wrap.querySelector(`[data-joint-lag="${MELEE.ARM_JOINTS[k]}"]`);
    if (!c) continue;
    const v = lag ? lag[k] : null;
    c.textContent = v === null || v === undefined ? '—' : (v * DEG).toFixed(1) + '°';
  }
}

/* ---- THE FRAMES (strokes.h "AN ATTACK IS A LIST OF FRAMES") -------------
 *
 * An attack is a list of frames, and every frame has the SAME controls: where
 * the dagger tip is when the frame ends, what that is measured from, how long
 * it takes, how the speed is spread, whether it cuts, which side the hand
 * leans, how much the wrist lays the blade along the stroke, how much the
 * torso turns with it, and the per-joint smoothing while it runs. "windup",
 * "cut" and "recover" are only names. After the last frame, `release` ticks
 * hand the arm back to the walk cycle.
 * ------------------------------------------------------------------------ */

// The RAW frame list this panel edits. A WEAPON FORM ("horizontal_r@short")
// edits `forms.short.frames`, and a player copy (":player") its own
// `player.frames` — each seeded from the style's own frames (and release) the
// first time it is touched, so the first edit is what makes it that weapon's.
function framesDoc(sty, create) {
  const r = sty.raw;
  if (sty.form) {
    const fo = r.forms && typeof r.forms === 'object' ? r.forms : null;
    const f = fo && fo[sty.form] && typeof fo[sty.form] === 'object' ? fo[sty.form] : null;
    if (f && Array.isArray(f.frames)) return f.frames;
    if (!create) return null;
    const base = lib && lib.styles[sty.baseIndex];
    const own = f || {};
    own.frames = ((base && base.frames) || sty.frames || []).map(MELEE.frameToJson);
    if (own.release === undefined) own.release = (base || sty).release;
    r.forms = fo || {};
    r.forms[sty.form] = own;
    return own.frames;
  }
  if (sty.derived) {
    if (!r.player || typeof r.player !== 'object') {
      if (!create) return null;
      r.player = {};
    }
    if (!Array.isArray(r.player.frames)) {
      if (!create) return null;
      const base = lib && lib.styles[sty.baseIndex];
      r.player.frames = ((base && base.frames) || sty.frames || []).map(MELEE.frameToJson);
    }
    return r.player.frames;
  }
  if (!Array.isArray(r.frames)) {
    if (!create) return null;
    r.frames = (sty.frames || []).map(MELEE.frameToJson);
  }
  return r.frames;
}
// Where a release edit is written. A form becomes the weapon's own on its
// first edit, frames and release together (framesDoc).
const releaseDoc = (sty) => {
  if (sty.form) { framesDoc(sty, true); return sty.raw.forms[sty.form]; }
  return sty.derived ? (sty.raw.player || (sty.raw.player = {})) : sty.raw;
};

const LEAN_TIP = {
  follow: 'the hand LEADS the tip\'s travel, so the edge leads a cut',
  hold: 'keep the side the hand is already on — a return, a feint',
  left: 'hand on the wielder\'s LEFT of the tip',
  right: 'hand on the wielder\'s RIGHT of the tip',
};

// A number box that may be EMPTY, meaning "use the global value".
function optNumCell({ get, set, placeholder, title, step = 0.05, min = 0, max = 1 }) {
  const cell = el('div', { class: 'atkcell' });
  const i = el('input', { class: 'atknum', type: 'number', step: String(step),
    placeholder, title: title || '' });
  const v = get();
  i.value = v === undefined ? '' : String(v);
  i.addEventListener('change', () => {
    if (i.value.trim() === '') { if (get() !== undefined) set(undefined); return; }
    const n = clamp(num(i.value, 0), min, max);
    if (n === get()) { i.value = String(n); return; }
    set(n);
  });
  cell.append(i, el('em', {}, v === undefined ? 'global' : ''));
  return cell;
}

/* ---- THE CLIPBOARD: poses and frames, across frames, styles and forms ----
 *
 * Two slots, so copying one pose never throws away a copied run of frames:
 *   pose   — one frame's arm + torso (shoulder / elbow / wrist, twist, pitch)
 *   frames — one frame or a whole style's list, timing and pacing included
 * Kept in localStorage, so a copy survives a page reload and reaches another
 * style, another weapon form, or the same style in a second tab. Every paste
 * is one undo step, and goes through framesDoc like every other edit, so
 * pasting into an inherited weapon form makes that form its own.
 * ------------------------------------------------------------------------ */
const CLIP_POSE_KEY = 'sandvox.attacks.clipPose';
const CLIP_FRAMES_KEY = 'sandvox.attacks.clipFrames';
const readClip = (key) => {
  try { return JSON.parse(localStorage.getItem(key) || 'null'); } catch { return null; }
};
let clipPose = readClip(CLIP_POSE_KEY);      // { pose, from } | null
let clipFrames = readClip(CLIP_FRAMES_KEY);  // { frames, release, from } | null
const writeClip = (key, v) => {
  try { if (v) localStorage.setItem(key, JSON.stringify(v)); else localStorage.removeItem(key); }
  catch { /* private mode: the clipboard lives for this page only */ }
};

// "horizontal_r [short] · windup" — where a copy came from, for the labels.
const styleLabel = (sty) => (sty.form ? `${sty.baseName} [${sty.form}]`
  : sty.derived ? sty.name : sty.name);
const frameLabel = (sty, k) =>
  `${styleLabel(sty)} · ${(sty.frames[k] && sty.frames[k].name) || 'frame ' + (k + 1)}`;

function copyPose(sty, k) {
  const f = sty.frames[k];
  if (!f || !f.pose) return toast('this frame is not a pose — nothing to copy', true);
  clipPose = { pose: MELEE.poseToJson(f.pose), from: frameLabel(sty, k) };
  writeClip(CLIP_POSE_KEY, clipPose);
  toast('copied pose: ' + clipPose.from);
  render();
}

// part: 'all' (arm + torso) | 'arm' | 'torso'
function pastePose(sty, k, part = 'all') {
  if (!clipPose) return toast('no pose copied yet — ⎘ copy one first', true);
  const src = JSON.parse(JSON.stringify(clipPose.pose));
  editStyles(`paste ${part === 'all' ? 'pose' : part} into frame ${k + 1}`, () => {
    const d = framesDoc(sty, true);
    const fr = d[k] || (d[k] = {});
    const q = fr.pose && typeof fr.pose === 'object' ? { ...fr.pose } : {};
    if (part !== 'torso') {
      q.shoulder = src.shoulder;
      q.elbow = src.elbow;
      if (src.wrist) q.wrist = src.wrist; else delete q.wrist;
    }
    if (part !== 'arm') {
      if (src.twist) q.twist = src.twist; else delete q.twist;
      if (src.pitch) q.pitch = src.pitch; else delete q.pitch;
    }
    // A torso-only paste onto a frame with no arm yet would be half a pose.
    if (q.shoulder && q.elbow) fr.pose = q;
  });
}

function copyFrames(sty, k /* undefined = all */) {
  const list = k === undefined ? sty.frames : [sty.frames[k]];
  if (!list.length || !list[0]) return;
  clipFrames = {
    frames: list.map(MELEE.frameToJson),
    release: sty.release,
    from: k === undefined ? `${styleLabel(sty)} (all ${list.length})` : frameLabel(sty, k),
  };
  writeClip(CLIP_FRAMES_KEY, clipFrames);
  toast('copied ' + (list.length === 1 ? 'frame: ' : 'frames: ') + clipFrames.from);
  render();
}

// at: index to insert before; replace = swap the whole list (and release).
function pasteFrames(sty, at, replace) {
  if (!clipFrames) return toast('no frames copied yet', true);
  const src = JSON.parse(JSON.stringify(clipFrames.frames));
  editStyles(replace ? 'replace frames with copied' : 'paste frames', () => {
    const d = framesDoc(sty, true);
    if (replace) {
      d.length = 0;
      d.push(...src.slice(0, MELEE.MAX_FRAMES));
      releaseDoc(sty).release = clipFrames.release;
    } else {
      d.splice(at, 0, ...src.slice(0, Math.max(0, MELEE.MAX_FRAMES - d.length)));
    }
  });
}

// HOLD a different frame, keeping ✋ manual on if it was: step through a
// style's poses without hunting for the next row's buttons.
function holdFrame(k) {
  const sty = lib && lib.styles[selected];
  if (!sty || !sty.frames.length) return;
  const n = sty.frames.length;
  goalKey = 'f' + (((k % n) + n) % n);
  host?.stop?.();
  render();
}
const heldIndex = () => (goalKey && goalKey[0] === 'f' ? +goalKey.slice(1) : -1);

// KEYS, while the attacks panel is up and a frame is held: Ctrl+C / Ctrl+V
// copy and paste its POSE, Ctrl+Shift+V pastes the arm only, and , / . step
// to the previous / next frame. Capture phase, ahead of the voxel editor's
// own Ctrl+C / Ctrl+V, which would otherwise copy a voxel selection nobody
// can see from this lane.
document.addEventListener('keydown', (ev) => {
  if (!isVisible() || !lib || heldIndex() < 0) return;
  const tg = ev.target, tag = (tg && tg.tagName || '').toLowerCase();
  if (tg && (tg.isContentEditable || tag === 'input' || tag === 'textarea' || tag === 'select')) return;
  const sty = lib.styles[selected];
  if (!sty) return;
  const k = heldIndex(), ctrl = ev.ctrlKey || ev.metaKey, key = ev.key.toLowerCase();
  let used = true;
  if (ctrl && key === 'c') copyPose(sty, k);
  else if (ctrl && key === 'v') pastePose(sty, k, ev.shiftKey ? 'arm' : 'all');
  else if (!ctrl && !ev.altKey && key === ',') holdFrame(k - 1);
  else if (!ctrl && !ev.altKey && key === '.') holdFrame(k + 1);
  else used = false;
  if (used) { ev.preventDefault(); ev.stopImmediatePropagation(); }
}, true);

// The clipboard and the held-frame stepper, over the frame list.
// DOM append, minus the nulls a conditional control leaves behind.
const add = (node, ...kids) => node.append(...kids.filter(k => k != null));
function clipboardBar(sty) {
  const frames = sty.frames || [];
  const hk = heldIndex();
  const bar = el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap;margin-bottom:4px;' +
    'padding:3px 6px;border:1px dashed var(--line,#3a3a3a);border-radius:4px' });
  add(bar, el('span', { class: 'lbl', title: 'what ⎀ paste will put down. Copies ' +
    'survive a page reload and work across styles and weapon forms.' }, 'clipboard'),
    el('span', { class: 'hint', title: clipPose ? 'the copied pose' : '' },
      'pose: ' + (clipPose ? clipPose.from : '—')),
    clipPose ? chip('✕', false, () => { clipPose = null; writeClip(CLIP_POSE_KEY, null); render(); },
      'forget the copied pose') : null,
    el('span', { class: 'hint', style: 'margin-left:8px' },
      'frames: ' + (clipFrames ? clipFrames.from : '—')),
    clipFrames ? chip('✕', false, () => { clipFrames = null; writeClip(CLIP_FRAMES_KEY, null); render(); },
      'forget the copied frames') : null,
    el('span', { class: 'spacer', style: 'flex:1' }),
    chip('⎘ copy all frames', false, () => copyFrames(sty), 'copy this whole ' +
      'animation (every frame, its timing, and the release) — then paste it ' +
      'over another style or another weapon\'s version'),
    chip('⎀ replace all', false, () => {
      if (confirm(`Replace all ${frames.length} frames of ${styleLabel(sty)} with ` +
        `the copied ${clipFrames.from}? (Ctrl+Z undoes it)`)) pasteFrames(sty, 0, true);
    }, 'replace every frame here with the copied frames', ) );
  if (!clipFrames) bar.lastChild.disabled = true;
  const step = el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap;margin-bottom:4px' });
  add(step, el('span', { class: 'lbl' }, 'holding'),
    chip('◀', false, () => holdFrame(hk < 0 ? frames.length - 1 : hk - 1), 'hold the previous frame (key , )'),
    el('b', {}, hk >= 0 ? `${hk + 1} / ${frames.length}  ${frames[hk]?.name || ''}` : 'nothing'),
    chip('▶', false, () => holdFrame(hk < 0 ? 0 : hk + 1), 'hold the next frame (key . )'),
    hk >= 0 ? chip('✋ manual', manual, () => { manual = !manual; render(); },
      'drag handles on the held frame') : null,
    el('span', { class: 'hint' }, hk >= 0
      ? 'Ctrl+C / Ctrl+V copy / paste this frame\'s pose · Ctrl+Shift+V arm only · , . step'
      : '◎ show a frame (or ◀ ▶) to hold it; then Ctrl+C / Ctrl+V work on its pose'));
  return [bar, step];
}

// A FRAME THAT IS A POSE (melee.js "A FRAME IS A POSE"): the arm is posed with
// the ✋ manual handles on the rig, so the row holds only what the handles do
// not reach — the torso — and a way to start from the previous frame's arm.
function poseRow(sty, f, k, edit, rawOf) {
  const p = f.pose;
  const setPose = (label, fn) => edit(label, d => {
    const fr = rawOf(d, k);
    fr.pose = { ...(fr.pose || {}) };
    fn(fr.pose, d);
  });
  const deg = (key, label, lim, title) => numCell({
    step: 1, min: -lim, max: lim, int: true, title,
    get: () => Math.round((p[key] || 0) * DEG), sub: label,
    set: v => setPose(`frame ${label}`, q => {
      if (v) q[key] = Math.round((v / DEG) * 1e6) / 1e6; else delete q[key];
    }) });
  return el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
    el('span', { class: 'lbl', style: 'min-width:4.5em', title: 'This frame IS a ' +
      'pose: the shoulder, elbow and wrist as they stand at its end. Between ' +
      'frames every joint turns straight from one pose to the next on the ' +
      'frame\'s pacing — nothing else steers the arm.' }, 'pose'),
    deg('twist', 'torso twist °', 90, 'TORSO TWIST at the end of this frame, ' +
      'degrees, + toward the wielder\'s right. Blended from the previous frame.'),
    deg('pitch', 'torso pitch °', 45, 'TORSO PITCH at the end of this frame, ' +
      'degrees, + = chest up. Blended from the previous frame.'),
    ...(k > 0 ? [el('button', { class: 'small', style: 'padding:0 5px',
      title: 'copy the PREVIOUS frame\'s arm and torso into this one (then pose it)',
      onclick: () => setPose('copy previous pose', (q, d) => {
        const prev = rawOf(d, k - 1).pose;
        if (prev) Object.assign(q, JSON.parse(JSON.stringify(prev)));
      }) }, '← previous')] : []),
    ...(k + 1 < sty.frames.length ? [el('button', { class: 'small', style: 'padding:0 5px',
      title: 'copy the NEXT frame\'s arm and torso into this one',
      onclick: () => setPose('copy next pose', (q, d) => {
        const nx = rawOf(d, k + 1).pose;
        if (nx) Object.assign(q, JSON.parse(JSON.stringify(nx)));
      }) }, 'next →')] : []),
    el('button', { class: 'small', style: 'padding:0 5px',
      title: 'FLIP THE ELBOW SOLUTION: turn the upper arm and forearm half a ' +
        'turn each about their own bone. Elbow, hand and blade stay exactly ' +
        'where they are; the elbow\'s bend changes sign and the shoulder\'s ' +
        '180° twist comes off (or goes on). Use it when a frame looks right ' +
        'but the shoulder spins going into or out of it. Click again to undo.',
      onclick: () => {
        const cur = p && p.joint ? p : MELEE.readPose(p);
        const flipped = cur && host?.flipElbowPose?.(cur);
        if (!flipped) return toast('flip elbow: no pose or no weapon arm on the rig', true);
        const j = MELEE.poseToJson(flipped);
        setPose('flip elbow', q => {
          q.shoulder = j.shoulder;
          q.elbow = j.elbow;
          if (j.wrist) q.wrist = j.wrist; else delete q.wrist;
        });
      } }, '⇅ flip elbow'),
    el('span', { class: 'spacer', style: 'width:6px' }),
    el('button', { class: 'small', style: 'padding:0 5px', onclick: () => copyPose(sty, k),
      title: 'copy this frame\'s pose (arm + torso) to the clipboard' }, '⎘ copy pose'),
    el('button', { class: 'small', style: 'padding:0 5px', onclick: () => pastePose(sty, k, 'all'),
      disabled: !clipPose || undefined,
      title: clipPose ? 'paste the copied pose (arm + torso) from ' + clipPose.from : 'nothing copied' },
      '⎀ paste'),
    el('button', { class: 'small', style: 'padding:0 5px', onclick: () => pastePose(sty, k, 'arm'),
      disabled: !clipPose || undefined,
      title: 'paste only the ARM (shoulder, elbow, wrist) of the copied pose; keep this torso' },
      '⎀ arm'),
    el('button', { class: 'small', style: 'padding:0 5px', onclick: () => pastePose(sty, k, 'torso'),
      disabled: !clipPose || undefined,
      title: 'paste only the TORSO (twist, pitch) of the copied pose; keep this arm' },
      '⎀ torso'));
}

function framesSection(sty) {
  const box = el('div', { style: 'margin-top:6px' });
  const frames = sty.frames || [];
  const edit = (label, fn) => editStyles(label, () => fn(framesDoc(sty, true)));
  const rawOf = (doc, k) => doc[k] || (doc[k] = {});

  box.append(el('div', { class: 'atkrow', style: 'margin-bottom:4px' },
    el('label', { title: 'Every frame has the same controls. The tip ' +
      'position is WHERE THE DAGGER TIP IS when the frame ends.' }, 'frames'),
    el('span', { class: 'hint' },
      sty.form ? (sty.formInherited
          ? `${sty.form} weapons — the style's default frames; the first edit makes them ${sty.form}'s own`
          : `${sty.form} weapons' own ${frames.length} frames`)
      : sty.derived ? 'player copy — edits write this style\'s player frames'
                  : `${frames.length} frames · angles in degrees`),
    sty.form && !sty.formInherited ? chip('⟲ back to default', false, () => {
      editStyles(`${sty.baseName}: ${sty.form} back to default`, () => {
        delete sty.raw.forms[sty.form];
        if (!Object.keys(sty.raw.forms).length) delete sty.raw.forms;
      });
    }, `drop the ${sty.form} version: ${sty.form} weapons swing the style's default frames again`) : null));
  box.append(...clipboardBar(sty));
  // STILL TIP TARGETS: one click turns every frame into the pose it shows now.
  if (frames.length && !MELEE.styleKeyed(sty)) {
    const partly = frames.some(f => f.pose);
    box.append(el('div', { class: 'atkrow', style: 'gap:6px;margin-bottom:4px' },
      partly ? el('span', { class: 'hint', style: 'color:#e0a050' },
        'some frames are poses and some are tip targets — the arm is keyed only ' +
        'when EVERY frame is a pose') :
      chip('⤓ bake to poses', false, () => {
        const i = lib.styles.indexOf(sty);
        const before = snap(raw);
        if (!bakeStyle(i)) { toast('could not bake: no arm on this rig for the style', true); return; }
        const after = snap(raw);
        const apply = s => { pourInto(raw, s); afterEdit('styles'); };
        host?.pushUndo?.('bake poses', () => apply(before), () => apply(after));
        afterEdit('styles');
      }, 'Turn every frame into the ARM POSE it shows now ("show"), and from then ' +
         'on play the style by turning each joint straight from one pose to the ' +
         'next. Tip targets, lean, blade angle and elbow direction go away; you ' +
         'pose frames with ✋ manual instead.')));
  }

  frames.forEach((f, k) => {
    const gk = 'f' + k;
    const fb = el('div', { class: 'atkframe', style:
      'border:1px solid var(--line,#3a3a3a);border-radius:4px;padding:4px 6px;' +
      'margin-bottom:5px;' + (f.cuts ? 'border-left:3px solid #e06c5a;' : '') +
      (goalKey === gk ? 'background:rgba(120,160,255,0.10);outline:1px solid #6d8fd6;' : '') });

    // ---- row 1: name, time, pacing, cuts, measured-from, frame buttons ----
    const nameIn = el('input', { class: 'atknum wide', type: 'text',
      style: 'width:9em', title: 'the frame\'s name — windup, cut, recover, ' +
        'anything: names are only labels' });
    nameIn.value = f.name || '';
    nameIn.addEventListener('change', () => {
      if (nameIn.value === (f.name || '')) return;
      edit('frame name', d => { rawOf(d, k).name = nameIn.value; });
    });
    const cutsChip = chip(f.cuts ? '✂ cuts' : 'no cut', f.cuts,
      () => edit(f.cuts ? 'frame stops cutting' : 'frame cuts', d => {
        const fr = rawOf(d, k);
        if (f.cuts) delete fr.cuts; else fr.cuts = true;
      }),
      'CUTS: the damage sweep is live during this frame. The cutting frames ' +
      'are one run; frames before it are the windup (the aim locks at its end), ' +
      'frames after it are the return.');
    const fromSel = el('select', { class: 'small',
      title: 'what the tip position is measured FROM. target: 0°/0° points ' +
        'straight at whatever is being attacked, so the frame follows the ' +
        'enemy. body: 0°/0° is straight ahead of the wielder, whatever the ' +
        'target — a stance.' });
    fromSel.append(el('option', { value: 'target' }, 'from target'),
                   el('option', { value: 'body' }, 'from body'));
    fromSel.value = f.fromBody ? 'body' : 'target';
    fromSel.addEventListener('change', () => edit('frame measured from', d => {
      const fr = rawOf(d, k);
      if (fromSel.value === 'body') fr.from = 'body'; else delete fr.from;
    }));
    const btn = (label, title, onclick, disabled) => el('button', {
      class: 'small', title, onclick, ...(disabled ? { disabled: true } : {}),
      style: 'padding:0 5px' }, label);
    const showing = goalKey === gk;
    fb.append(el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
      el('b', { style: 'min-width:1.4em' }, String(k + 1)), nameIn,
      numCell({ int: true, step: 1, min: 1, max: 240, dflt: 8,
        title: 'ticks (30 per second) the frame takes. It always ARRIVES on ' +
          'its last tick.',
        get: () => f.ticks, sub: fmt(f.ticks * TICK_MS / 1000, 2) + ' s',
        set: v => edit('frame ticks', d => { rawOf(d, k).ticks = v; }) }),
      paceCell({
        chase: !f.pose,
        title: 'PACING: how the travel is spread over the ticks. linear = even; ' +
          '…In = slow start, fast finish; …Out = fast start, slow finish. ' +
          'Every curve ARRIVES on the last tick. chase = close on the pose at ' +
          'a capped speed (the box that appears), arriving whenever it arrives ' +
          '— how every stock windup and return moved before frames.',
        get: () => (f.ease === 'chase' ? '' : (f.ease || 'linear')),
        set: v => edit('frame pacing', d => {
          const fr = rawOf(d, k);
          if (v === '') { fr.ease = 'chase'; return; }
          delete fr.chaseRate;
          if (v === 'linear') delete fr.ease; else fr.ease = v;
        }) }),
      ...(f.ease === 'chase' && !f.pose ? [numCell({
        step: 1, min: 1, max: 200, dflt: 16,
        title: 'CHASE SPEED: the most the tip may move per tick while chasing, ' +
          'in input units (16 ≈ 4.6°/tick sideways, 6.1°/tick up/down). A ' +
          'frame too short for the distance ends SHORT of its pose.',
        get: () => f.chaseRate,
        sub: `≈${fmt(f.chaseRate * 0.005 * DEG, 1)}°/t`,
        set: v => edit('chase speed', d => {
          const fr = rawOf(d, k);
          if (v === 16) delete fr.chaseRate; else fr.chaseRate = v;
        }) })] : []),
      cutsChip, fromSel,
      el('span', { class: 'spacer', style: 'flex:1' }),
      chip(showing ? '◉ showing' : '◎ show', showing, () => {
        goalKey = showing ? null : gk;
        if (!goalKey) manual = false;
        host?.stop?.();
        render();
      }, 'HOLD the arm exactly on this frame\'s end pose in the preview'),
      // MANUAL: hold THIS frame and pose it by dragging the arm (rig.js
      // drawPoseHandles). Clicking it on another frame moves manual there.
      chip('✋ manual', manual && showing, () => {
        if (manual && showing) { manual = false; }
        else { manual = true; goalKey = gk; host?.stop?.(); }
        render();
      }, 'MANUAL ADJUSTMENT of this frame: holds it and puts drag handles on ' +
         'the arm. tip = where the point goes (az + el) · shoulder = click for ' +
         'separate az ↔ / el ↕ · upper arm = az + el · reach = extension · ' +
         'elbow = which way the elbow points · wrist = blade angle + which side ' +
         'the hand sits. This frame\'s numbers update as you drag; each drag is ' +
         'one undo. Click again to stop.'),
      btn('▲', 'move this frame earlier', () => edit('move frame', d => {
        [d[k - 1], d[k]] = [d[k], d[k - 1]];
      }), k === 0),
      btn('▼', 'move this frame later', () => edit('move frame', d => {
        [d[k + 1], d[k]] = [d[k], d[k + 1]];
      }), k === frames.length - 1),
      btn('⧉', 'duplicate this frame', () => edit('duplicate frame', d => {
        d.splice(k + 1, 0, JSON.parse(JSON.stringify(rawOf(d, k))));
      }), frames.length >= MELEE.MAX_FRAMES),
      btn('⎘', 'copy this whole FRAME (timing, pacing, cuts, pose) to the clipboard',
        () => copyFrames(sty, k)),
      btn('⎀', clipFrames ? `paste the copied frame(s) AFTER this one — ${clipFrames.from}`
        : 'nothing copied', () => pasteFrames(sty, k + 1, false),
        !clipFrames || frames.length >= MELEE.MAX_FRAMES),
      btn('✕', 'delete this frame', () => edit('delete frame', d => {
        d.splice(k, 1);
      }), frames.length <= 1)));

    if (f.pose) fb.append(poseRow(sty, f, k, edit, rawOf));
    else {
    // ---- row 2: where the tip ends up, lean, wrist -------------------------
    const deg = (key, label) => guideCell(numCell({
      step: 1, min: -200, max: 200,
      title: `${label} of the tip at the END of this frame, in degrees, ` +
        (f.fromBody ? 'from straight ahead of the body' : 'from the target') +
        (key === 'az' ? ' (+ = to the wielder\'s right)' : ' (+ = up)'),
      get: () => Math.round(f[key] * DEG),
      sub: label,
      set: v => edit(`frame ${label}`, d => { rawOf(d, k)[key] = v / DEG; }),
    }), gk);
    const leanSel = el('select', { class: 'small',
      title: 'which side of the tip the HAND is on. ' +
        Object.entries(LEAN_TIP).map(([n, t]) => `${n}: ${t}`).join('; ') });
    for (const n of MELEE.LEANS)
      leanSel.append(el('option', { value: n, title: LEAN_TIP[n] }, 'lean ' + n));
    leanSel.value = f.lean || 'follow';
    leanSel.addEventListener('change', () => edit('frame lean', d => {
      const fr = rawOf(d, k);
      if (leanSel.value === 'follow') delete fr.lean; else fr.lean = leanSel.value;
      if (leanSel.value !== 'angle') delete fr.leanAngle;
    }));
    // LEAN ANGLE, shown only in "angle" mode: which way the hand sits off the
    // shoulder-to-tip line, 0° = below it, + toward the weapon side.
    const leanAngleCell = f.lean === 'angle' ? numCell({
      step: 5, min: -180, max: 180,
      title: 'LEAN ANGLE, degrees about the shoulder-to-tip line: where the ' +
        'HAND sits relative to the blade. 0° = below it, +90° = out to the ' +
        'weapon side, −90° = in across the body, 180° = above it.',
      get: () => Math.round((f.leanAngle || 0) * DEG), sub: 'lean °',
      set: v => edit('frame lean angle', d => {
        rawOf(d, k).leanAngle = Math.round((v / DEG) * 1e6) / 1e6;
      }) }) : null;
    fb.append(el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
      el('span', { class: 'lbl', style: 'min-width:4.5em' }, 'tip ends at'),
      deg('az', 'azimuth'), deg('el', 'elevation'),
      // ARM EXTENSION, in %. The file stores `reach` as an offset from the
      // neutral 0.60 of the arm's band (strokes.cpp StrokeReachIn), which read
      // as a mystery number whose useful range was -0.6..+0.4 and clipped
      // silently past it. The box shows the same fact as the author thinks of
      // it: 0% = the tip drawn in as close as this arm goes, 100% = the arm
      // straight out. Stored unchanged: reach = % / 100 - 0.60.
      guideCell(numCell({
        step: 5, min: 0, max: 100, int: true,
        title: 'ARM EXTENSION: how far from the shoulder the tip is, along the ' +
          'direction az/el point. 0% = pulled in as close as this arm goes ' +
          '(elbow bent), 100% = arm straight out. The line underneath is the ' +
          'resulting distance from the shoulder on the arm being previewed.',
        get: () => Math.round((MELEE.kNeutralReach + f.reach) * 100),
        sub: 'extension · ' + reachSub(f.reach) + ' from shoulder',
        set: v => edit('frame extension', d => {
          rawOf(d, k).reach = Math.round((v / 100 - MELEE.kNeutralReach) * 1e6) / 1e6;
        }),
      }), gk),
      leanSel, leanAngleCell,
      numCell({
        step: 0.05, min: 0, max: 1,
        title: 'WRIST: how much the blade is laid along the stroke. 1 = ' +
          'pointing exactly where this frame says; 0 = riding the grip angle.',
        get: () => f.wristAlign, sub: 'wrist align',
        set: v => edit('frame wrist', d => {
          const fr = rawOf(d, k);
          if (v === 1) delete fr.wrist; else fr.wrist = v;
        }) })));

    // ---- row 2b: the arm's shape — blade angle and elbow direction ---------
    // Empty = AUTO, and the placeholder says so: auto is the old rule (hand
    // held at melee.handExtend of the arm; elbow trailing the hand's travel),
    // shown rather than hidden.
    const degOpt = (key, opts) => optNumCell({
      step: 5, min: opts.min, max: opts.max, placeholder: opts.placeholder,
      title: opts.title,
      get: () => (opts.isSet(f) ? Math.round(opts.read(f) * DEG) : undefined),
      set: v => edit(opts.label, d => {
        const fr = rawOf(d, k);
        if (v === undefined) delete fr[key];
        else fr[key] = Math.round((v / DEG) * 1e6) / 1e6;
      }),
    });
    fb.append(el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
      el('span', { class: 'lbl', style: 'min-width:4.5em' }, 'arm shape'),
      degOpt('bladeAngle', {
        label: 'frame blade angle', min: 0, max: 180,
        placeholder: 'blade ∠ auto',
        title: 'BLADE ANGLE, degrees: how far the blade is cocked off the line ' +
          'from the shoulder to the tip. 0° = the blade continues the arm ' +
          'straight (point where the arm points); 90° = square to it. The hand ' +
          'is placed from this, on the LEAN side. A tip closer to the shoulder ' +
          'than the blade is long cannot be reached at 0° — raise the angle or ' +
          'the extension. EMPTY = auto: the old rule, hand held at ' +
          'melee.handExtend of the arm and the angle solved from that.',
        isSet: fr => fr.bladeAngle >= 0, read: fr => fr.bladeAngle,
      }),
      degOpt('elbow', {
        label: 'frame elbow direction', min: -180, max: 180,
        placeholder: 'elbow auto',
        title: 'ELBOW DIRECTION, degrees about the shoulder-to-hand line: 0° = ' +
          'elbow points DOWN, +90° = OUT to the weapon side, −90° = IN across ' +
          'the body, 180° = UP. With the hand placed, this is the arm\'s one ' +
          'remaining freedom, so it sets the shoulder\'s rotation and the ' +
          'elbow\'s bend together — use it to stop an elbow folding backwards. ' +
          'EMPTY = auto: the old rule, the elbow trails the hand\'s travel ' +
          'within melee.elbowPoleCone.',
        isSet: fr => fr.elbowSet, read: fr => fr.elbow,
      }),
      el('span', { class: 'hint' }, 'blended from the previous frame over this one')));

    // ---- row 3: torso and the joint smoothing ------------------------------
    const tw = host?.tuning?.()?.melee || {};
    const row3 = el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
      el('span', { class: 'lbl', style: 'min-width:4.5em' }, 'torso'),
      optNumCell({
        title: 'TORSO TWIST: fraction of the tip\'s azimuth the torso turns ' +
          'with. Empty = the global melee.torsoShare.',
        placeholder: 'twist ' + fmt(num(tw.torsoShare, 0.35), 2),
        get: () => (f.torsoTwist >= 0 ? f.torsoTwist : undefined),
        set: v => edit('frame torso twist', d => {
          const fr = rawOf(d, k);
          if (v === undefined) delete fr.torsoTwist; else fr.torsoTwist = v;
        }) }),
      optNumCell({
        title: 'TORSO PITCH: fraction of the tip\'s elevation the torso ' +
          'leans with. Empty = the global melee.torsoPitch.',
        placeholder: 'pitch ' + fmt(num(tw.torsoPitch, 0.2), 2),
        get: () => (f.torsoPitch >= 0 ? f.torsoPitch : undefined),
        set: v => edit('frame torso pitch', d => {
          const fr = rawOf(d, k);
          if (v === undefined) delete fr.torsoPitch; else fr.torsoPitch = v;
        }) }));
    fb.append(row3);
    }
    const row4 = el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
      el('span', { class: 'lbl', style: 'min-width:4.5em',
        title: 'PER-JOINT SMOOTHING while this frame runs, applied to the posed ' +
          'arm after the IK. smooth = half-life in ticks (bigger = lazier). ' +
          'max°/t = the most the joint may turn in one tick. 0 = off.' },
        'smoothing'));
    for (const name of MELEE.ARM_JOINTS) {
      const js = (f.joints && f.joints[name]) || { smooth: 0, maxDeg: 0 };
      const write = (key, v) => edit(`frame ${name} ${key}`, d => {
        const fr = rawOf(d, k);
        if (!fr.joints || typeof fr.joints !== 'object') fr.joints = {};
        const j = fr.joints[name] && typeof fr.joints[name] === 'object'
          ? fr.joints[name] : (fr.joints[name] = {});
        if (v > 0) j[key] = v; else delete j[key];
        if (!Object.keys(j).length) delete fr.joints[name];
        if (!Object.keys(fr.joints).length) delete fr.joints;
      });
      row4.append(el('span', { class: 'hint', title: JOINT_TIP[name],
        style: 'margin-left:4px' }, name),
        numCell({ step: 0.5, min: 0, max: 60, dflt: 0,
          title: `${name} smoothing half-life in ticks (0 = off). ` + JOINT_TIP[name],
          get: () => js.smooth, sub: js.smooth > 0 ? 'smooth' : 'smooth off',
          set: v => write('smooth', v) }),
        numCell({ step: 1, min: 0, max: 180, dflt: 0,
          title: `${name}: most degrees it may turn per tick (0 = off). ` + JOINT_TIP[name],
          get: () => js.maxDeg, sub: js.maxDeg > 0 ? 'max°/t' : 'max off',
          set: v => write('maxDeg', v) }));
    }
    fb.append(row4);
    box.append(fb);
  });

  // ---- after the frames ----------------------------------------------------
  box.append(el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
    chip('+ frame', false, () => edit('add a frame', d => {
      const last = d[d.length - 1] ? JSON.parse(JSON.stringify(d[d.length - 1])) : {};
      delete last.cuts;
      last.name = `frame ${d.length + 1}`;
      last.ticks = last.ticks || 8;
      d.push(last);
    }), 'append a frame (a copy of the last one, not cutting) — then set where ' +
        'its tip ends up'),
    el('span', { class: 'lbl', style: 'margin-left:8px' }, 'release'),
    numCell({ int: true, step: 1, min: 1, max: 120, dflt: 6,
      title: 'RELEASE: ticks the arm takes to hand back to the walk cycle after ' +
        'the last frame.',
      get: () => sty.release,
      sub: fmt(sty.release * TICK_MS / 1000, 2) + ' s',
      set: v => editStyles('release', () => { releaseDoc(sty).release = v; }) }),
    MELEE.styleKeyed(sty) ? null : chip('⌖ centre on target', false, () => edit('centre the cut on the target', d => {
      // The midpoint of the CUT (from the pose it starts at to where it ends)
      // onto the target: every target-measured frame shifts by the same
      // amount, so the shape of the swing is unchanged.
      const fc = frames.findIndex(x => x.cuts);
      let lc = -1;
      for (let q = frames.length - 1; q >= 0; q--) if (frames[q].cuts) { lc = q; break; }
      if (fc < 0) return;
      const a = frames[fc > 0 ? fc - 1 : fc], b = frames[lc];
      const mAz = (a.az + b.az) / 2, mEl = (a.el + b.el) / 2;
      d.forEach((fr, q) => {
        if (frames[q] && !frames[q].fromBody) {
          fr.az = Math.round((num(fr.az, 0) - mAz) * 1e6) / 1e6;
          fr.el = Math.round((num(fr.el, 0) - mEl) * 1e6) / 1e6;
        }
      });
    }), 'shift every target-measured frame so the MIDDLE of the cut passes ' +
        'through the target (the swing\'s shape is unchanged)')));

  // Jitter (style-wide: a per-swing draw, not a frame property).
  if (!sty.raw.jitter || typeof sty.raw.jitter !== 'object')
    sty.raw.jitter = { az: 0, el: 0, tempo: 0 };
  const jr = sty.raw.jitter;
  box.append(el('div', { class: 'atkrow', style: 'gap:6px;flex-wrap:wrap' },
    el('span', { class: 'lbl', title: 'deterministic per-swing variation: ' +
      'tempo stretches every frame up to the cut; the bow wobbles the frames ' +
      'before the cut. 0 = every swing identical.' }, 'jitter'),
    numCell({ step: 0.01, min: 0, max: 0.6, get: () => num(jr.tempo, 0), sub: 'tempo',
      set: v => editStyles('jitter tempo', () => { jr.tempo = v; }) }),
    degCell(jr, 'az', 'bow az', 'jitter'),
    degCell(jr, 'el', 'bow el', 'jitter')));

  // The live joint lag (preview), which updateJointLag fills per frame.
  const lagRow = el('div', { class: 'atkrow', style: 'gap:6px' },
    el('span', { class: 'hint' }, 'smoothing lag now:'));
  for (const name of MELEE.ARM_JOINTS)
    lagRow.append(el('span', { class: 'hint' }, name + ' '),
      el('span', { 'data-joint-lag': name,
        style: 'font-family:var(--mono);font-size:11px' }, '—'));
  box.append(lagRow);
  return box;
}

/* ---- the program card -------------------------------------------------- */

function programCard(sty) {
  const r = sty.raw;
  const card = el('div', { class: 'atkcard program' });
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

  card.append(framesSection(sty));

  // ---- THE BODY ANIMATION (strokes.h AttackStyle::clip) -----------------
  // The program above drives the WEAPON ARM. Everything else the swing does
  // with the body — the step, the shoulder, the off hand — is a CLIP,
  // keyframed in the clip lane, saved to the library with "→ library", and
  // named here. The engine starts it with the stroke (Mob::PlayClip at
  // BeginStroke, both drivers) and the arm claim still wins on the arm.
  {
    const row = el('div', { class: 'atkrow' });
    const sel = el('select', { class: 'small',
      title: 'a clip from assets/anims/ (the "→ library" button in the clip ' +
        'lane puts one there) played on the body WITH this stroke. The stroke ' +
        'keeps the weapon arm; the clip is the rest of the body. Empty = none. ' +
        'A rig without the clip\'s parts simply does not play it.' });
    const cur = typeof r.clip === 'string' ? r.clip : '';
    sel.append(el('option', { value: '' }, '(no body clip)'));
    const names = new Set(libClips || []);
    const rigClips = Object.keys(host?.sidecar?.()?.clips || {});
    for (const n of names) sel.append(el('option', { value: n }, n));
    for (const n of rigClips)
      if (!names.has(n)) sel.append(el('option', { value: n }, n + ' (this rig only)'));
    if (cur && !names.has(cur) && !rigClips.includes(cur))
      sel.append(el('option', { value: cur }, cur + ' (not found)'));
    sel.value = cur;
    sel.addEventListener('change', () => {
      const v = sel.value;
      if (v === cur) return;
      editStyles('clip', () => { if (v) r.clip = v; else delete r.clip; });
    });
    row.append(el('label', { title: 'body animation played with the stroke' }, 'clip'), sel,
      el('button', { class: 'small', title: 're-read assets/anims/',
        onclick: async () => { await refreshLibClips(); render(); } }, '↻'));
    if (libClips && !libClips.length && !rigClips.length)
      row.append(el('span', { class: 'hint' },
        'none yet — key one in the clip lane, then "→ library"'));
    card.append(row);
  }

  card.append(weaponRow(sty), lungeBlock(sty), targetRow(sty));
  card.append(derivedLine(sty));
  return card;
}

/* ---- WHAT SWINGS IT, AND WHEN (docs/PLAN_impact_unarmed.md §4/§5) --------
 *
 * Four fields that each default to the behaviour a style authored before they
 * existed already had — the held weapon, never a fallback, the profile's
 * reach, no leap, the chest — so an older file loads unchanged and a newer one
 * loads on an older binary. That is also why they are edited HERE rather than
 * in a second panel: they are part of the same authoring act as the windup
 * and the cut, and splitting "what the arm does" from "which arm" is how you
 * get a punch authored against a sword's coordinates.
 * ------------------------------------------------------------------------ */

function weaponRow(sty) {
  const r = sty.raw;
  const row = el('div', { class: 'atkrow' });

  // ---- the weapon picker --------------------------------------------
  // "held" plus THE OPEN RIG'S OWN natural weapons, because only the rig can
  // answer what "jaws" means: StyleUsable asks a creature whether the part is
  // there and alive, which no amount of reading this JSON can decide
  // (melee.js's citation block says the same about the port).
  const nat = host?.naturalWeapons?.() || [];
  const sel = el('select', { class: 'small', title:
    'WHAT SWINGS THIS STYLE. "held" is whatever is in the fist — every style ' +
    'authored before the unarmed package. A natural weapon is a part of the ' +
    'rig that IS a weapon (the Natural weapons block in the rig panel): the ' +
    'driver steers THAT part, and PickAttackStyle drops the style entirely ' +
    'when the creature has not got it or it has been cut off.' });
  sel.append(el('option', { value: 'held' }, 'held item'));
  for (const w of nat)
    sel.append(el('option', { value: w.name },
      w.name + ' (' + w.mode + ' · ' + (w.part || '?') + ')'));
  const cur = sty.weapon || 'held';
  if (cur !== 'held' && !nat.some(w => w.name === cur))
    sel.append(el('option', { value: cur }, cur + ' — not on this rig'));
  sel.value = cur;
  sel.addEventListener('change', () => {
    const v = sel.value;
    if (v === cur) return;
    editStyles('weapon', () => {
      if (v === 'held') delete r.weapon; else r.weapon = v;
    });
  });
  row.append(el('label', { title: 'the effector the stroke driver moves' }, 'weapon'), sel);

  // ---- fallback -------------------------------------------------------
  row.append(chip('fallback', !!sty.fallback, () =>
    editStyles('fallback', () => {
      if (sty.fallback) delete r.fallback; else r.fallback = true;
    }),
    'ONLY WHEN THERE IS NOTHING BETTER. PickAttackStyle drops every fallback ' +
    'style as a group the moment any non-fallback one is still usable, which ' +
    'is what makes a duelist\'s punches invisible while it holds a sword and ' +
    'the whole of its repertoire once disarmed. Every punch in the shipped ' +
    'library is one; the zombie\'s bites are not.'));

  // ---- reach ----------------------------------------------------------
  row.append(el('label', { title: 'world voxels' }, 'reach'), numCell({
    step: 1, min: 0, max: 200,
    title: 'WORLD VOXELS, centre to centre, overriding the behaviour ' +
      'profile\'s attack.reach for THIS style alone; 0 = use the profile\'s. ' +
      'A lunging bite closes 22 and a punch closes 9, and a profile can only ' +
      'state one number — MobSystem::AttackReachOf hands the AI the longest ' +
      'of them and BeginStroke refuses a style the target is outside.',
    get: () => num(r.reach, 0),
    sub: sty.reach > 0 ? fmt(sty.reach * MELEE.kVoxelMeters, 2) + ' m' : 'profile',
    subTitle: 'the same distance in metres',
    set: v => editStyles('reach', () => { if (v > 0) r.reach = v; else delete r.reach; }),
  }));
  if (cur !== 'held' && !nat.some(w => w.name === cur && w.usable))
    row.append(el('span', { class: 'hint' },
      '⚠ the open rig declares no usable "' + cur + '"'));
  return row;
}

/**
 * THE BALLISTIC OPENING, and the timeline that makes it authorable.
 *
 * strokes.h states the contract and leaves one thing to the author: "lining
 * the landing up with the cut is the AUTHOR's job (windup.ticks ~ ticks; the
 * tuner's Attacks lane shows both)". This is that. The two numbers are on
 * different clocks — `ticks` is what the horizontal magnitude is BUDGETED
 * against, while the flight actually ends when gravity brings `rise` back down
 * — so the panel solves the arc and prints the landing tick beside the tick
 * the cut starts on.
 */
function lungeBlock(sty) {
  const r = sty.raw;
  const wrapEl = el('div', {});
  const has = MELEE.lungeAny(sty.lunge) ||
              (r.lunge && typeof r.lunge === 'object');
  const head = el('div', { class: 'atkrow' },
    el('label', { title:
      'A POUNCE IS NOT A FASTER WALK. The walk drive resolves a whole step ' +
      'against the body\'s box every tick and cannot leave the ground, so a ' +
      'lunge hands the body a VELOCITY (Mob::Launch) at the start of a named ' +
      'phase and UpdateFall carries it, wall test and all.' }, 'lunge'));
  if (!has) {
    head.append(chip('+ lunge', false, () => editStyles('add lunge', () => {
      r.lunge = { ticks: 9, speed: 2.4, rise: 1.1, at: 'windup' };
    }), 'give this style a leap'));
    wrapEl.append(head);
    return wrapEl;
  }
  head.append(el('button', { class: 'small danger', title: 'no leap',
    onclick: () => editStyles('remove lunge', () => { delete r.lunge; }) }, '✕'));
  wrapEl.append(head);

  const j = r.lunge;
  const seg = el('div', { class: 'atkseg' });
  seg.append(el('div', {}),
    el('div', { class: 'hdr', title: 'the flight the author is BUDGETING for' }, 'ticks'),
    el('div', { class: 'hdr', title: 'm/s, horizontal ceiling' }, 'speed'),
    el('div', { class: 'hdr', title: 'm/s, straight up' }, 'rise'),
    el('div', { class: 'hdr', title: 'which phase\'s START fires it' }, 'at'));
  seg.append(el('div', { class: 'lbl' }, 'leap'));
  seg.append(numCell({
    int: true, step: 1, min: 0, max: 60, dflt: 9,
    title: 'THE FLIGHT THE MAGNITUDE IS BUDGETED AGAINST, not the flight that ' +
      'happens: the horizontal is min(speed, (gap − the effector\'s reach) ÷ ' +
      'this), so the same authored style is a long leap from far out and a ' +
      'short hop from close in. The landing is decided by `rise` and gravity — ' +
      'the preview prints both.',
    get: () => Math.max(0, Math.round(num(j.ticks, 0))),
    sub: fmt(Math.max(0, Math.round(num(j.ticks, 0))) * TICK_MS / 1000, 2) + ' s',
    set: v => editStyles('lunge ticks', () => { j.ticks = v; }),
  }));
  seg.append(numCell({
    step: 0.1, min: 0, max: 20,
    title: 'METRES PER SECOND, a CEILING on the horizontal — how fast this ' +
      'creature can throw itself. Converted once by MetresPerSecToCells, for ' +
      'the reason every other authored length here is derived.',
    get: () => num(j.speed, 0),
    sub: fmt(num(j.speed, 0) / MELEE.kVoxelMeters, 1) + ' v/s',
    set: v => editStyles('lunge speed', () => { j.speed = v; }),
  }));
  seg.append(numCell({
    step: 0.1, min: 0, max: 20,
    title: 'METRES PER SECOND straight up. A LUNGE WITH NO RISE LANDS THE TICK ' +
      'AFTER IT LEFT — UpdateFall\'s launch latch buys exactly one — so every ' +
      'authored lunge must climb (scripts/test_melee.mjs asserts it).',
    get: () => num(j.rise, 0),
    sub: fmt(num(j.rise, 0) / MELEE.kVoxelMeters, 1) + ' v/s',
    set: v => editStyles('lunge rise', () => { j.rise = v; }),
  }));
  const at = el('select', { class: 'small', title:
    'WHICH PHASE\'S START FIRES IT. "windup" (the default) makes the leap the ' +
    'telegraph itself; "cut" throws the body after the blow has committed. A ' +
    'leap during the recover would be a creature hurling itself at somebody ' +
    'after the blow landed, so those two are the only values the loader takes.' });
  for (const v of ['windup', 'cut']) at.append(el('option', { value: v }, v));
  at.value = j.at === 'cut' ? 'cut' : 'windup';
  at.addEventListener('change', () => {
    const v = at.value;
    if (v === (j.at || 'windup')) return;
    editStyles('lunge at', () => { j.at = v; });
  });
  seg.append(el('div', { class: 'atkcell' }, at));
  wrapEl.append(seg);
  return wrapEl;
}

/**
 * WHICH LIMB A BLOW IS AIMED AT — weights over LIMB TAGS, not names, so any
 * rig that tags its parts answers the same table (strokes.h StyleTargetWeight).
 * One row per tag the OPEN RIG publishes, which is the only honest list: a
 * weight on a tag this creature has not got is a weight that never wins.
 */
function targetRow(sty) {
  const r = sty.raw;
  const tags = host?.limbTags?.() || [];
  const wrapEl = el('div', {});
  const head = el('div', { class: 'atkrow' },
    el('label', { title:
      'Absent = today\'s behaviour, the chest. Drawn counter-based on (mobId, ' +
      'tick) at BeginStroke like every other variation here, over the ' +
      'victim\'s LIVE base limbs, and recorded on NpcStroke::targetLimb so a ' +
      'gate can assert the DISTRIBUTION rather than inferring it from where ' +
      'the wounds landed.' }, 'target'));
  if (!tags.length) {
    head.append(el('span', { class: 'hint' },
      'the open rig tags no limbs — tag them in the rig panel and the weights ' +
      'appear here'));
    wrapEl.append(head);
    return wrapEl;
  }
  const have = r.target && typeof r.target === 'object' ? r.target : null;
  head.append(el('span', { class: 'hint' },
    have ? 'weights over this rig\'s tags; 0 = never' : 'none — the chest'),
    chip(have ? 'clear' : '+ table', false, () => editStyles('target table', () => {
      if (have) delete r.target;
      else r.target = Object.fromEntries(tags.map(t => [t, t === 'head' ? 0.35 : 0.2]));
    }), have ? 'drop the table: the blow goes to the chest again'
            : 'weight this style over the tags this rig publishes'));
  wrapEl.append(head);
  if (!have) return wrapEl;

  const grid = el('div', { class: 'atkseg',
    style: 'grid-template-columns:repeat(' +
           Math.min(6, Math.max(2, tags.length)) + ',minmax(0,1fr))' });
  // Normalised alongside the raw weight, because the draw is proportional and
  // "0.35" means nothing without the total it is 0.35 OF.
  let total = 0;
  for (const t of tags) total += Math.max(0, num(have[t], 0));
  for (const t of tags)
    grid.append(el('div', { class: 'atkcell' },
      el('div', { class: 'hdr', style: 'font-size:9.5px', title: 'limb tag' }, t),
      numCell({
        step: 0.05, min: 0, max: 10,
        title: 'relative weight; the draw is proportional, so only the ratios ' +
               'matter. 0 (or absent) means this tag is never chosen.',
        get: () => num(have[t], 0),
        sub: total > 1e-6
          ? Math.round(Math.max(0, num(have[t], 0)) / total * 100) + '%' : '—',
        subTitle: 'share of the draw',
        set: v => editStyles('target ' + t, () => {
          if (v > 0) have[t] = v; else delete have[t];
        }),
      })));
  wrapEl.append(grid);
  return wrapEl;
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
  // THE WHOLE PATH: `cut` is a list of legs and the phase is one phase, so
  // every number here is summed over it (strokes.h "A CUT IS A PATH").
  const cutAll = MELEE.cutTravel(sty);
  const w = sty.windup.ticks, c = cutAll.ticks, rc = sty.recover.ticks;
  const total = w + c + rc;
  const cutDeg = Math.round(Math.hypot(cutAll.az, cutAll.el) * DEG);
  const cutS = c * TICK_MS / 1000;
  const d = el('div', { class: 'atkderived' });
  d.append(el('span', {},
    el('b', {}, String(total)), ' ticks / ', el('b', {}, fmt(total * TICK_MS / 1000)),
    ' s  ·  telegraph ', el('b', {}, fmt(w * TICK_MS / 1000)),
    ' s  ·  cut sweeps ', el('b', {}, cutDeg + '°'), ' in ', el('b', {}, fmt(cutS)),
    ' s (', el('b', {}, String(Math.round(cutDeg / Math.max(cutS, 1e-3)))), '°/s)'));
  // WHERE THE TARGET SITS ALONG THE PATH, which is the one thing about a
  // multi-leg cut an author cannot read off the rows: a dogleg's midpoint is
  // usually its corner, and a hook whose aim lands there meets the target with
  // its hesitation instead of its edge.
  if (sty.cut.length > 1) {
    const k = Number.isInteger(sty.aimLeg) && sty.aimLeg >= 0 ? sty.aimLeg : -1;
    d.append(el('span', {}, ` · ${sty.cut.length} legs, target in ` +
      (k >= 0 ? `cut ${k + 1}` : 'the middle of the whole travel')));
  }
  if (sty.jitter.tempo > 0) {
    const lo = Math.max(2, Math.round(w * (1 - sty.jitter.tempo)));
    const hi = Math.max(2, Math.round(w * (1 + sty.jitter.tempo)));
    d.append(el('span', {}, ` · windup varies ${lo}–${hi} ticks`));
  }
  // HOW THE RECOVER IS SPENT, because "10 ticks of recover" says nothing about
  // the thing that is actually being authored: whether the arm is DRIVEN home
  // or crossfaded out of the cut pose.
  const rv = sty.recover;
  const globalFade = Math.round(
    num(host?.meleeTuning?.().recoverTime, 0.22) * 1000 / TICK_MS);
  d.append(el('span', {}, rv.posed
    ? ` · recover drives ${rv.settle} then fades ` +
      `${rv.fade || globalFade}${rv.fade ? '' : ' (global)'}`
    : ` · recover CROSSFADES from the cut pose over ` +
      `${rv.fade || globalFade}${rv.fade ? '' : ' (global)'} ticks`));
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
      onclick: async () => {
        if (selected < 0) return toast('pick an attack first', true);
        const arm = host?.armInfo?.();
        if (!arm || arm.chain < 0)
          return toast('this rig has no arm chain tagged "arm" whose effector ' +
            'is the weapon socket\'s part — the driver has nothing to steer', true);
        goalKey = null;   // swinging means the program drives again
        nextSwing();
        await host.begin(selected);
        render();
      },
    }, '▶ swing'),
    el('button', { class: 'small', onclick: () => { host?.stop?.(); render(); } }, '■'),
    chip('loop', loop, () => { loop = !loop; render(); },
      'replay the program forever — the authoring mode'),
    chip('vary', vary, () => { vary = !vary; render(); },
      'RE-ROLL THE JITTER ON EVERY LOOP, the pre-2026-09-21 behaviour. Off, ' +
      'the loop replays the SAME swing, which is what authoring a recover ' +
      'needs — with the shipped NPC jitter (up to 0.24 rad of start bow and ' +
      '±0.28 of tempo) every cycle was a different swing and nothing said so. ' +
      'On, you see the spread the game will actually produce. `reroll` steps ' +
      'the sequence one draw either way.'),
    chip('slow ×10', slow, () => {
      slow = !slow;
      if (host?.state?.().live) host?.begin?.(selected);
      render();
    }, 'play every frame (and the release) ten times longer in this preview ' +
       'only — to read a swing tick by tick. Nothing is saved.'),
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

  // ---- SOLO: one segment at a time --------------------------------------
  const t3 = el('div', { class: 'atkrow' });
  t3.append(el('label', { title: 'isolate one segment of the program' }, 'solo'));
  const soloTips = {
    all: 'the whole program, as the game runs it',
    windup: 'ONLY the windup: from wherever the arm hangs, steer to the windup ' +
      'pose (target + windup) and HOLD there. This is the pose the cut ' +
      'starts from — if the readout\'s "commit" residual is not ~0, the ' +
      'windup ran out of ticks before it got there.',
    cut: 'ONLY the cut: the windup is run through instantly, then the travel ' +
      'from the pose it reached, along the authored path, then HOLD at the end.',
    recover: 'ONLY the recover: windup and cut run through instantly, then the ' +
      'hand-back — no input, the driver\'s follow-through unwinds and the arm ' +
      'claim fades over melee.recoverTime.',
  };
  for (const k of ['all', 'windup', 'cut', 'recover'])
    t3.append(chip(k, solo === k, () => {
      solo = k;
      goalKey = null;   // solo PLAYS a segment; a goal HOLDS one
      if (host?.state?.().live) host?.begin?.(selected);
      render();
    }, soloTips[k]));
  card.append(t3);

  // ---- GOAL FRAMES: the destinations, held still -------------------------
  //
  // A stroke program is a list of destinations with pacing between them, and
  // watching the arm travel is the wrong way to author a destination: what is
  // on screen at any instant is wherever the interpolation had got to. These
  // chips put the arm EXACTLY on one authored target and leave it there, so
  // the az/el/reach boxes in the program card can be set against the pose
  // they actually produce. Set the goals first, then pick the pacing.
  if (sty) {
    const goals = MELEE.strokeGoals(sty);
    const t5 = el('div', { class: 'atkrow' });
    t5.append(el('label', {
      title: 'HOLD one of this style\'s destinations, with no interpolation ' +
        'and no time passing. The frame re-resolves as you edit, so nudging ' +
        'an az box or an aim slider moves the held pose under you.',
    }, 'goal'));
    t5.append(chip('off', !goalKey, () => {
      if (!goalKey) return;
      goalKey = null;
      manual = false;
      host?.stop?.();
      render();
    }, 'back to the driven preview'));
    for (const g of goals)
      t5.append(chip(g.label, goalKey === g.key, () => {
        goalKey = g.key;
        // A held frame and a live program are exclusive — stop the driver
        // rather than letting the next tick fight the snap.
        host?.stop?.();
        render();
      }, `hold the arm where the program ARRIVES at the end of frame ${g.frame + 1} ` +
        `("${g.label}") — the swing run from rest and stopped on that frame's ` +
        'last tick, exactly what a solo of it freezes on. The yellow cross is ' +
        'the authored target; a gap to it is how far short the frame stops ' +
        '(a chase frame out of ticks). Uses this swing\'s jitter draw.'));
    card.append(t5);
    // The resolved numbers, because "hold the windup" is only useful if you
    // can read what it resolved TO on this arm — and how far the arm that
    // actually arrives is from what the row asks for.
    if (goalKey) {
      const g = host?.goalPose?.(goalKey);
      const line = el('div', { class: 'atkderived' });
      const deg = v => String(Math.round(v * DEG)) + '°';
      if (g) {
        const a = g.arrived;
        const off = a ? Math.hypot(a.az - g.az, a.el - g.el) : 0;
        line.append(el('span', {},
          el('b', {}, 'holding '),
          (MELEE.strokeGoals(sty).find(q => q.key === goalKey) || {}).label || goalKey,
          a ? ' — arrives az ' : ' — az ',
          el('b', {}, deg(a ? a.az : g.az)), ' · el ',
          el('b', {}, deg(a ? a.el : g.el)), ' · reach ',
          el('b', {}, fmt(a ? a.reach : g.reach, 1) + ' vox'),
          a ? el('span', { class: 'hint' },
            `  · authored ${deg(g.az)} / ${deg(g.el)} / ${fmt(g.reach, 1)} vox` +
            (off > 0.035 ? ` — stops ${deg(off)} short` : '')) : '',
          el('span', { class: 'hint' },
            '  (absolute, in the rig\'s facing basis)')));
      } else
        line.append(el('span', { class: 'hint' },
          'this style has no such frame any more'));
      card.append(line);
    }
  }

  // ---- WEAPON PICKER. A blade is not decoration here: the driver MEASURES
  // hand-to-point and solves the whole reach band against it, so an empty hand
  // is a different set of numbers rather than the same swing without a prop.
  const t2 = el('div', { class: 'atkrow' });
  const armed = host?.weaponEquipped?.();
  const curWeapon = host?.weaponName?.() || '';
  const melee = host?.meleeItems?.() || [];
  if (melee.length) {
    const sel = el('select', { class: 'small',
      title: 'which weapon is in the fist — the driver measures the blade and ' +
        'solves its reach band against it, so each weapon is a different swing' });
    sel.append(el('option', { value: '' }, '(unarmed)'));
    for (const it of melee) {
      const o = el('option', { value: it.id }, it.name || it.id);
      if (it.id === curWeapon) o.selected = true;
      sel.append(o);
    }
    sel.addEventListener('change', async () => {
      const v = sel.value;
      if (v) await host?.equipWeapon?.(v);
      else await host?.equipWeapon?.(null, true);
      render();
    });
    t2.append(el('label', { title: 'the held weapon the stroke preview uses' }, 'weapon'), sel);
  } else {
    t2.append(el('button', {
      class: 'small' + (armed ? '' : ' primary'),
      title: armed
        ? 'swap the weapon in the rig\'s hand'
        : 'put a sword in this rig\'s hand — the stroke driver MEASURES the ' +
          'blade, so an empty fist is a different swing, not the same one undressed',
      onclick: async () => { await host?.equipWeapon?.(); render(); },
    }, armed ? '◆ ' + (curWeapon || 'armed') : '+ sword'));
    if (armed)
      t2.append(el('button', {
        class: 'small', title: 'empty the hand',
        onclick: async () => { await host?.equipWeapon?.(null, true); render(); },
      }, 'disarm'));
    if (!itemListFetched) {
      itemListFetched = true;
      host?.ensureItemList?.().then(() => render());
    }
  }
  t2.append(el('span', { class: 'spacer' }),
    el('label', { title: 'the TARGET: its bearing from the eyes (0/0 = straight ' +
      'ahead of the face) and its distance in voxels. Every frame not measured ' +
      'from the body is measured from it; ⌖ on the windup row centres the cut on it' }, 'aim'),
    aimSlider('az'), aimSlider('el'), aimSlider('dist'),
    chip('centre', false, () => { aim = { ...aim, az: 0, el: 0 }; render(); }),
    chip('orb', orbOn, () => { orbOn = !orbOn; render(); },
      'show the target as an orb in the viewport'));
  card.append(t2);

  if (sty) card.append(strokeBar(sty), lungeTimeline(sty));
  card.append(readout());
  return card;
}

/**
 * LANDS AT TICK N, CUT STARTS AT TICK M — the one line an author can act on.
 *
 * Both numbers are counted from the first tick of the program, and they are
 * measured on different clocks on purpose:
 *
 *   LANDS  the ballistic flight the authored `rise` actually buys against
 *          physics.gravity, integrated on the stroke's own 30 Hz clock. It is
 *          NOT `lunge.ticks` — that number is the BUDGET the horizontal
 *          magnitude is solved against, and the two agreeing is the thing
 *          being authored, not something the loader enforces.
 *   CUT    the post-jitter `windupTicks` of the LIVE program when one is
 *          running, and the authored count otherwise. A style with
 *          jitter.tempo has no single answer here, which is itself worth
 *          seeing: a leap timed against the mean lands early half the time.
 *
 * The preview flies the arc from the STYLE'S OWN reach, because that is the
 * distance BeginStroke commits from, and arrives at the EFFECTOR's reach —
 * confusing those two is what made the engine's first lunge hop on the spot.
 */
function lungeTimeline(sty) {
  const box = el('div', { class: 'atkderived', id: 'atkLunge' });
  const info = host?.lungeInfo?.(sty);
  if (!info) { box.style.display = 'none'; return box; }
  box.append(el('span', { id: 'atkLungeText' }, lungeText(sty, info)));
  return box;
}

function lungeText(sty, info) {
  const cur = host?.cursorTicks?.() || { windup: 0 };
  const cutAt = cur.windup > 0 ? cur.windup : sty.windup.ticks;
  const land = info.at === 'cut' ? cutAt + info.landTicks : info.landTicks;
  const slack = land - cutAt;
  const state = info.state ? ` · ${info.state} ×${fmt(info.scale, 2)}` : '';
  const live = info.live
    ? `  ·  in the air ${info.live.airTicks}t, ${fmt(info.live.x, 1)} vox out` +
      (info.live.landed ? ' (landed)' : '')
    : '';
  return `lunge: lands at tick ${land}, cut starts at tick ${cutAt}` +
    (Math.abs(slack) <= 1 ? '  ✓ lined up'
      : slack > 0 ? `  ⚠ ${slack} ticks late — the cut fires in mid-air`
                  : `  ⚠ ${-slack} ticks early — it lands and then winds up`) +
    `  ·  budget ${info.budget}t, travels ${fmt(info.travel, 1)} vox from a ` +
    `${fmt(info.standOff, 0)}-voxel stand-off to a ${fmt(info.armReach, 1)}-` +
    `voxel reach${state}${live}`;
}

function aimSlider(key) {
  const g = el('span', { style: 'display:inline-flex;gap:3px;align-items:center' });
  const [lo, hi, st] = key === 'az' ? [-1.4, 1.4, 0.01]
    : key === 'el' ? [-1.0, 1.0, 0.01] : [2, 30, 0.5];
  const s = el('input', { type: 'range', min: String(lo), max: String(hi), step: String(st),
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
  const w = sty.windup.ticks, c = MELEE.cutTravel(sty).ticks,
        rc = sty.recover.ticks;
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
    s(`windup ${w}`, w, '#6aa9ff', 'the telegraph: driven under commitSpeed, no cut fires'));
  // ONE BAND PER LEG, alternating shade. The cut is still ONE phase — there is
  // no hand-back between legs and `phaseTick` runs across all of them — so the
  // boundaries are drawn as a change of shade rather than as a gap: what an
  // author is reading here is where the point changes direction, and a gap
  // would say "and here it stops", which is exactly what it does not do.
  const aimK = Number.isInteger(sty.aimLeg) && sty.aimLeg >= 0
    ? sty.aimLeg : -1;
  sty.cut.forEach((leg, k) => {
    const isAim = aimK >= 0 ? k === aimK : false;
    bar.append(s(
      sty.cut.length > 1 ? `cut ${k + 1} · ${leg.ticks}` : `cut ${leg.ticks}`,
      leg.ticks, k % 2 ? '#e8b464' : '#ffd08a',
      `the travel, leg ${k + 1} of ${sty.cut.length}: ` +
      `${Math.round(Math.hypot(leg.az, leg.el) * DEG)}° in ${leg.ticks} ticks` +
      (isAim ? ' — the ◎ leg ⌖ centres on' : '')));
  });
  bar.append(
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
    line('phase', phaseText(st), 'the STROKE PROGRAM\'s phase'),
    line('effector', effectorText(a),
      'WHICH PART THE DRIVER IS MOVING, and how (game/impact.h ' +
      'StrikeEffectorMode). "held" is the socket\'s hand with an item in it; ' +
      '"chain" is a natural weapon an IK chain serves — the same solve, no ' +
      'wrist steering, the part\'s own edge; "aim" is a part in no chain (the ' +
      'jaws), rotated with a share of the spine until its forward lies along ' +
      'the stroke. An aim effector shows the yaw/pitch it was commanded, which ' +
      'is the number a bite that does not snap is missing.'),
    line('driver', st.meleePhase || 'idle',
      'MeleeState: guard / wind / recover. The driver has no cut of its ' +
      'own — the PROGRAM\'s cut phase is the cut.'),
    line('az/el', `${fmt(st.az)} / ${fmt(st.el)}`, 'the live stroke, radians'),
    line('start', startText(st),
      'WHERE THE SWING BEGAN: the driver seeds its stroke from wherever the ' +
      'blade IS (melee.cpp take-over), so an arm hanging at the side starts ' +
      'a swing at el ≈ −1.3 whatever the style says. The windup has to climb ' +
      'from here.'),
    line('target', targetText(st),
      'what the windup is steering TO: target + windup (az / el, radians) ' +
      'and the band position; then, once committed, the frozen aim'),
    line('commit', commitText(st),
      'target minus reached, at the instant the aim froze. Not ~0 means the ' +
      'windup ran out of ticks — see "fit" — and the cut spends its own ticks ' +
      'closing the gap, which reads as the blade wandering in elevation.'),
    line('fit', fitText(st),
      'the closed loop is capped at 16 units/tick × the aim gain (strokes.cpp ' +
      'steerTo), so a raise of Δel needs Δel ÷ cap ticks. More than the windup ' +
      'has = the pose is not reached before the cut.'),
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

// The readout's derived strings, shared by the first render and tickUI.
const phaseText = st => (st.phase || 'idle') + (st.holding ? ' (held)' : '');
const effectorText = a => {
  if (!a || !a.effMode || a.effMode === 'none') return '—';
  const what = a.effWeapon ? ' · ' + a.effWeapon : '';
  const aim = a.aim
    ? `  ${fmt(a.aim.yaw)} / ${fmt(a.aim.pitch)} rad, spine ${fmt(a.aim.share, 2)}`
    : '';
  return `${a.effPart || '?'} · ${a.effMode}${what}${aim}`;
};
const startText = st => st.start
  ? `${fmt(st.start.az)} / ${fmt(st.start.el)}` : '—';
function targetText(st) {
  if (!st.live) return '—';
  if (st.phase === 'windup')
    return `${fmt(st.wantAz)} / ${fmt(st.wantEl)} @ ${fmt(st.wantReach, 1)}v`;
  if (st.aimed) return `aim ${fmt(st.aimAz)} / ${fmt(st.aimEl)}`;
  return '—';
}
const commitText = st => st.commit
  ? `${fmt(st.commit.dAz)} / ${fmt(st.commit.dEl)} / ${fmt(st.commit.dR, 1)}v`
  : '—';
function fitText(st) {
  if (!st.start || !st.capEl) return '—';
  const dEl = Math.abs(st.wantEl - st.start.el);
  const dAz = Math.abs(st.wantAz - st.start.az);
  const need = Math.ceil(Math.max(dEl / st.capEl, dAz / (st.capAz || 1)));
  const have = st.windupTicks || 0;
  return `${need} of ${have} ticks` + (need > have ? ' ⚠ short' : '');
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
    set('phase', phaseText(st));
    set('effector', effectorText(a));
    set('driver', st.meleePhase || 'idle');
    set('az/el', `${fmt(st.az)} / ${fmt(st.el)}`);
    set('start', startText(st));
    set('target', targetText(st));
    set('commit', commitText(st));
    set('fit', fitText(st));
    set('radius', fmt(st.radius));
    set('tip', fmt(st.tipSpeed, 1) + ' v/s');
    set('edge', fmt(st.edgeAlign));
    set('weight', fmt(st.weight));
    set('blade', fmt(a.bladeLen));
    set('band', `${fmt(a.bandLo)}–${fmt(a.bandHi)}`);
  }
  // The flight, live. Text only — rebuilding the element would drop the
  // author's focus, which is the whole reason tickUI is not a re-render.
  {
    const lt = wrap.querySelector('#atkLungeText');
    const sty = lib?.styles[selected];
    const info = sty ? host.lungeInfo?.(sty) : null;
    if (lt && sty && info) lt.textContent = lungeText(sty, info);
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
const foldedCompass = () => {
  const mode = WEAPON_MODES.find(m => m.id === weaponMode);
  return fold('compass', (mode ? mode.label : 'player') + ' flick compass', compassBody);
};
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
  knob('commitSpeed', 'ref spd', 'reference drive speed, input units/s: 35% ' +
    'of it reads as Wind (what a parry can arrest), and the whoosh volume ' +
    'scales against it. Nothing commits on it — the driver has no cut of ' +
    'its own.', { step: 10, min: 1, dflt: 900 });
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
  const compassKey = activeCompassKey();
  const pj = activeCompassRaw();
  if (!Array.isArray(pj.sectors)) pj.sectors = [];
  const compassMap = activeCompassMap();

  const row = el('div', { style: 'display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap' });
  const SZ = 150;
  const pad = el('canvas', { id: 'atkCompass', width: String(SZ), height: String(SZ),
    style: 'border:1px solid #262e3c;border-radius:6px;background:#0f131a;cursor:crosshair',
    title: 'drag to test a flick — the label below names the style the engine ' +
           'would pick. +y is DOWN (raw screen mouse).' });
  const testLbl = el('div', { class: 'hint',
    style: 'font-family:var(--mono);max-width:150px' });
  const updateTest = () => {
    const i = MELEE.quantizeStrike(compassMap, flick.x, flick.y);
    testLbl.textContent = `(${fmt(flick.x)}, ${fmt(flick.y)}) → ` +
      (i >= 0 ? lib.styles[i].name : 'no sector');
  };
  drawCompass(pad, SZ, compassMap); updateTest();
  const padPoint = ev => {
    const r = pad.getBoundingClientRect();
    const x = (ev.clientX - r.left) / SZ * 2 - 1, y = (ev.clientY - r.top) / SZ * 2 - 1;
    const l = Math.hypot(x, y);
    flick = l > 1e-4 ? { x: x / l, y: y / l } : { x: 1, y: 0 };
    drawCompass(pad, SZ, compassMap); updateTest();
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
      if (MELEE.quantizeStrike(compassMap, Math.cos(a), Math.sin(a)) === i) return false;
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
function drawCompass(canvas, SZ, map) {
  map = map || activeCompassMap() || lib.player;
  const g = canvas.getContext('2d');
  const R = SZ / 2 - 2;
  g.clearRect(0, 0, SZ, SZ);
  const palette = ['#3a5f8f', '#8f6a3a', '#3a8f5f', '#8f3a5f', '#5f3a8f',
                   '#8f8f3a', '#3a8f8f', '#8f3a3a'];
  const steps = 240;
  for (let k = 0; k < steps; k++) {
    const a0 = k / steps * Math.PI * 2, a1 = (k + 1) / steps * Math.PI * 2;
    const i = MELEE.quantizeStrike(map, Math.cos((a0 + a1) / 2), Math.sin((a0 + a1) / 2));
    g.beginPath();
    g.moveTo(SZ / 2, SZ / 2);
    g.arc(SZ / 2, SZ / 2, R, a0, a1);
    g.closePath();
    g.fillStyle = i >= 0 ? palette[i % palette.length] : '#1a212c';
    g.fill();
  }
  g.lineWidth = 2;
  for (const s of map.sectors) {
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
  p(b('AZ AND EL ARE BEARINGS ABOUT THE SHOULDER, in the body\'s facing frame. '),
    'Azimuth 0 is straight ahead of the wielder, positive to their right; ' +
    'elevation 0 is level with the shoulder, positive up. They are not ' +
    'screen angles and not joint angles: they say where the POINT of the ' +
    'blade is, and the arm is solved to put it there. The AIM is the ' +
    'target\'s bearing in the same frame — the sliders here; in the game it ' +
    'is the camera line for the player and the target\'s position for an ' +
    'NPC — and every windup value is an OFFSET from it.');
  p(b('AN ATTACK IS A LIST OF FRAMES, and every frame is the same. '),
    'Each frame says WHERE THE TIP IS when it ends (az / el / reach), what ' +
    'that is measured from (the target, or the body), how many ticks it ' +
    'takes, how the speed is spread (pacing), whether it CUTS (the damage ' +
    'sweep is live), which side of the tip the hand LEANS, how much the WRIST ' +
    'lays the blade along the stroke, how much the TORSO turns with it, and ' +
    'the per-joint SMOOTHING while it runs. "windup", "cut", "recover" are ' +
    'only names. After the last frame, RELEASE ticks hand the arm back.');
  p(b('THE CUT IS THE RUN OF CUTTING FRAMES. '),
    'Frames before it are the windup — the aim LOCKS at the end of the last ' +
    'one, which is what makes the telegraph mean something — and frames ' +
    'after it are the return. ⌖ centre on target shifts every ' +
    'target-measured frame so the middle of the cut passes through the target.');
  p(b('PACING. '),
    'linear and the curves always ARRIVE on the frame\'s last tick. chase ' +
    'closes on the pose at a capped speed (its box) and arrives whenever it ' +
    'arrives — possibly never, in a short frame; the stock windups and returns ' +
    'use it because that is how they have always moved.');
  p(b('A SWING STARTS FROM WHEREVER THE ARM IS. '),
    'The first frame travels from the blade\'s live position (with the arm ' +
    'hanging, el ≈ −1.3 rad), so give it the ticks to get where it is going.');
  p(b('SOLO shows one phase. '),
    'windup: the frames before the cut, then hold. cut: those run through ' +
    'instantly, then the cutting frames, then hold. recover: both run ' +
    'through, then the return and the release. Hold is 0.8 s, then it loops. ' +
    '◎ show on a frame HOLDS the arm on that frame\'s end pose instead.');
  p(b('REACH IS A BAND POSITION. '),
    '0 is the neutral 0.60 of the arm\'s own annulus, not zero voxels. ' +
    'Authored against the ARM instead, every chamber and lunge in the shipped ' +
    'library landed outside the band and was clamped: a thrust asking for four ' +
    'voxels moved 0.15 and read as a twitch.');
  p(b('VARIATION IS DETERMINISTIC. '),
    'Every draw is Hash3(mobId ^ salt, tick, index), so ten swings vary and the ' +
    'fight replays. jitter bow az/el wobbles the frames before the cut; ' +
    'jitter tempo stretches every frame up to the cut. The player copies ' +
    'author 0 on purpose — a strike must go exactly where it was flicked.');
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
