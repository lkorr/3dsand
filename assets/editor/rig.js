/* ============================================================================
   rig.js — Wave 2b: parts/rig UI, animation timeline, onion skin, gait preview.

   Split out of editor.js to keep each file readable: editor.js owns the
   document, the three.js scene and the voxel brushes; this file owns the
   sidecar JSON and everything that poses or sequences models.

   THE RULE THAT MATTERS HERE (PLAN_voxel_editor.md §D): the gait preview
   renders the EXPORTED DATA, never a parallel implementation. Every formula
   in section 5 is transcribed from src/game/mob.cpp:391-408 and the loader
   around mob.cpp:229-284, with the source lines quoted. If the engine changes,
   this file is wrong and must follow — do not "improve" the math here.

   Sidecar handling: the parsed object is mutated IN PLACE and written back
   whole by editor.js. Fields this UI does not understand (clips, chains,
   spring, anything a later wave adds) are therefore preserved verbatim. Never
   rebuild the sidecar from scratch from the form state.

   Sections:
     1  state + helpers
     2  sidecar accessors (limbs, gait, flipbooks)
     3  parts / rig panel
     4  anchor gizmo binding
     5  gait preview  <-- mirrors mob.cpp, see above
     6  timeline + flipbook frames
     7  onion skinning
     8  keyboard + wiring
   ========================================================================== */

import * as ed from './editor.js';
import * as AN from './anim.js';
import * as MELEE from './melee.js';
import * as ATK from './attacks.js';
import * as VOX from './vox.js';
import * as LIB from './limblib.js';

/* ==========================================================================
   1. state + helpers
   ========================================================================== */

let el = null, toast = () => {};
let sideEl = null, timelineEl = null;
let clipWrap = null;          // renderClipLane()'s own container, see below

let selectedPart = null;      // limb name, or null
let selectedParts = new Set(); // shift-click multi-select (names)
// Socket being dragged, by INDEX into sidecar.sockets, or null. Selecting a
// socket takes the gizmo over from the joint anchor — both are "a point on a
// part", so they share one gizmo rather than fighting over the viewport.
let selectedSocket = null;
const collapsedGroups = new Set();

// --- limb library (assets/limbs/, see limblib.js) -------------------------
//
// The catalogue of saved parts, fetched once per tab activation and after
// every save. Entries are {name, limbs, root, dim, from} — the .json's `meta`
// plus enough of its limb list to describe the part without opening the .vox.
// `libErr` carries the file:// degradation message, exactly as the item list
// does: a file:// page cannot read assets/limbs/ and must say so rather than
// showing an empty shelf that looks like "you have saved nothing".
let libParts = null;          // null = not loaded yet, [] = loaded and empty
let libErr = '';
let libOpen = true;           // the section is expanded
let libFilter = '';           // name/tag search box

// --- the OTHER CREATURES shelf -------------------------------------------
//
// The saved library answers "wear the arm I put aside earlier". This answers
// the question you actually have while drawing the fifth character: "what does
// THAT one's arm look like on this one?" — and it needs no curation step at
// all, because every mob file on disk is already a bag of the same named
// .vox models plus the same limbs[] sidecar a library part is (see limblib.js;
// a saved part is literally a mob file one rung down the hierarchy).
//
// So this is the same shelf over a different index: `crParts` is a flat list of
// every limb of every OTHER creature, and swapping one in runs the identical
// limblib code path — partFromPrefab on the foreign document, then the same
// graft. The only thing this file adds is the catalogue and the tag filter.
//
// `null` = not scanned yet, `[]` = scanned and nothing found. Entries are
// {path, from, root, tag, limbs:[{name,tag}], dim} — one per candidate ROOT
// limb, i.e. one per thing you could swap in.
let crParts = null;
let crErr = '';
let crFilter = '';
let crSameTagOnly = true;     // "only limbs of this type", the default question
// 'saved' = assets/limbs/, 'creatures' = every other mob file.
let libSource = 'saved';
// path -> {prefab, palette} for creature .vox files already read. A mob .vox is
// tens of KB and the point of the shelf is trying several in a row, so the
// second look at a creature must not re-parse it.
const crVoxCache = new Map();

// --- held-item preview ----------------------------------------------------
//
// WHY THE ITEM IS LOADED HERE AT ALL. The rig says only WHERE the fist closes;
// which way the blade POINTS is the item's `grip.rotation` (item.h, and the
// note over sockets() below). Authoring that angle used to mean typing Euler
// degrees on the tuner's Items tab, saving, launching the game and looking —
// which is how the sword ended up laid along the forearm, i.e. inside the arm.
// So the preview hangs the ITEM'S OWN ART off the socket and edits the ITEM'S
// sidecar. The rig sidecar is not touched: nothing here writes socket.rotation,
// which stays identity exactly as mob.h asks.
//
// `itemDoc` is the item's .vox parsed into editor models; `itemSc` is its
// sidecar. Both are null until an item is picked, and the whole feature is
// server-only for the same reason the Audio tab is: a file:// page cannot read
// assets/items/.
let itemList = null;          // [{id, name}] from items.json, or null
let heldItemId = null;        // which item is previewed, or null for none
let itemDoc = null;           // { models:[...] } parsed from the item's .vox
let itemSc = null;            // the item's sidecar JSON, mutated in place
let itemDirty = false;        // sidecar has unsaved grip edits
let itemErr = null;           // load failure, shown in the panel
let gripCtx = 'held_right';   // which grip context is being edited
let gaitOn = false;
// NB: the gait phase itself lives in anim.gaitPhase (the AnimState mirror),
// not here — it is runtime state the transcribed pipeline owns.
// A FRACTION OF THE DEF'S TOP SPEED, and it defaults to a plain WALK rather
// than to 1.0 for the same reason avatar.cpp splits walk from run at 0.80 and
// not at 0.50: `speed` in the sidecar is the SPRINT reference. Previewing at
// 1.0 is previewing a sprint, which is not what the button next to it says —
// and with the locomotion family now driving the clips, it visibly picked
// `run`. 0.58 is where a plain walk sits (avatar.cpp quotes "35 of 60").
let gaitSpeedScale = 0.58;

let playing = false;
let frameIndex = 0;
let frameClockMs = 0;
let activeTag = null;         // flipbook tag name, or null for "all models"
let onionOn = false;
let onionRange = 2;

// --- clip editor ---------------------------------------------------------
let activeClip = null;        // clip name, or null
let clipCursorMs = 0;         // scrub position
let clipPlaying = false;
let autoKey = false;
let selectedKey = null;       // { part, tMs }
// Live pose being authored for the selected part, in the part's LOCAL frame.
// Applied on top of the sampled clip pose so dragging the gizmo shows the
// result immediately; "Key" commits it into the track.
let poseEdit = null;          // { part, rot:{x,y,z,w}, pos:{x,y,z} }

const num = (v, d = 0) => (Number.isFinite(+v) ? +v : d);
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

/* ==========================================================================
   2. sidecar accessors

   Every one of these creates the container lazily and returns a LIVE
   reference, so form edits land straight in the object that gets saved.
   ========================================================================== */

function sc() {
  let s = ed.getSidecar();
  if (!s) { s = {}; ed.setSidecar(s); }
  return s;
}
const limbs = () => {
  const s = sc();
  if (!Array.isArray(s.limbs)) s.limbs = [];
  return s.limbs;
};
const gait = () => {
  const s = sc();
  if (!s.gait || typeof s.gait !== 'object') s.gait = {};
  return s.gait;
};
const flipbooks = () => {
  const s = sc();
  if (!s.flipbooks || typeof s.flipbooks !== 'object') s.flipbooks = {};
  return s.flipbooks;
};
const clips = () => {
  const s = sc();
  if (!s.clips || typeof s.clips !== 'object') s.clips = {};
  return s.clips;
};
const chains = () => {
  const s = sc();
  if (!Array.isArray(s.chains)) s.chains = [];
  return s.chains;
};
// Sockets: WHERE A HELD ITEM ATTACHES (src/game/mob.h MobSocketDef).
//
// A socket is a POINT AND A FRAME on one part, and nothing more — no item
// knowledge, no grip data. How a particular weapon sits in the fist is the
// ITEM's business (assets/items/<id>.json's `grip`), so the same socket serves
// a sword, a torch and an empty hand. That split is what lets one item be held
// correctly by any rig that publishes a socket, and it is the reason this
// editor edits only the point.
//
// It is named for the CONTEXT an item asks for ("held_right"), NOT for the
// limb it rides: an item looks up its grip by context, and `part` is the
// separate question of which limb carries that context on THIS rig. A
// left-handed creature puts "held_right" on its own hand.L and every item
// still hangs correctly with no per-item edits.
const sockets = () => {
  const s = sc();
  if (!Array.isArray(s.sockets)) s.sockets = [];
  return s.sockets;
};
const limbByName = n => limbs().find(l => l.name === n) || null;

/* --- held-item preview: load / save --------------------------------------
 *
 * These ride the same /api/model routes the tuner's Items tab uses, so there
 * is ONE path-containment check and one write format for a per-item file.
 */

/** The socket currently driving the preview, or null. */
function activeSocket() {
  const SK = sockets();
  if (selectedSocket !== null && selectedSocket >= 0 && selectedSocket < SK.length)
    return SK[selectedSocket];
  return null;
}

/** items.json, fetched once. Null (with a note in the panel) under file://. */
async function ensureItemList() {
  if (itemList) return itemList;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent('items/items.json'));
    if (!r.ok) throw new Error('items.json ' + r.status);
    const j = JSON.parse(await r.text());
    // items.json is a LIST of defs; tolerate both the bare array and a wrapped
    // object so this does not break if the schema grows a header.
    const arr = Array.isArray(j) ? j : (j.items || []);
    itemList = arr.map(it => ({ id: it.id || it.name, name: it.name || it.id,
                                kind: it.kind || '' }))
                  .filter(it => it.id);
  } catch (e) {
    itemErr = 'needs the tuner server (python scripts/tuner_server.py) — ' +
              'a file:// page cannot read assets/items/';
    itemList = null;
  }
  return itemList;
}

/**
 * Load one item's art + sidecar. The .vox comes back as BYTES and is parsed by
 * the same vox.js the editor uses for everything else, so the preview shows the
 * real art rather than a stand-in box.
 */
async function loadHeldItem(id) {
  itemErr = null; itemDoc = null; itemSc = null; itemDirty = false;
  if (!id) { heldItemId = null; return; }
  heldItemId = id;
  try {
    const rs = await fetch('/api/model?path=' + encodeURIComponent('items/' + id + '.json'));
    if (!rs.ok) throw new Error(id + '.json ' + rs.status);
    const scText = await rs.text();
    // SUPERSEDED while in flight (another equip, or an unequip): drop it, or
    // a sword nobody is holding comes back as a trail with no sword.
    if (heldItemId !== id) return;
    itemSc = JSON.parse(scText);

    const modelName = itemSc.model || id;
    const rv = await fetch('/api/model?path=' + encodeURIComponent('items/' + modelName + '.vox'));
    if (!rv.ok) throw new Error(modelName + '.vox ' + rv.status);
    // `.prefab` is the EDITOR-shaped parse (dim/grid/offset per model), the
    // same one openPath() builds the document from — not the raw .vox models.
    const parsed = VOX.readVox(await rv.arrayBuffer());
    if (heldItemId !== id) { itemSc = null; return; }
    itemDoc = parsed.prefab;
    if (!itemDoc?.models?.length) throw new Error('no models in ' + modelName + '.vox');
    // Default the context to one the item actually declares, so the panel opens
    // on something editable instead of an empty "held_right" that is not there.
    const ctxs = Object.keys(itemSc.grip || {});
    if (ctxs.length && !ctxs.includes(gripCtx)) gripCtx = ctxs[0];
  } catch (e) {
    itemErr = String(e.message || e);
    itemDoc = null; itemSc = null;
  }
  ed.invalidate();
  renderAllPanels();
}

/**
 * ARM THE RIG, in one click.
 *
 * Previewing a stroke needs a blade in the fist — the driver MEASURES the
 * hand-to-point distance and solves the whole reach band against it, so an
 * empty hand is not "the same swing without a sword", it is a different set of
 * numbers (the point becomes the hand and the band collapses to the bare arm).
 * Getting there used to be four steps in two panels: find the socket, select
 * it, open the item dropdown, pick one. This is the shortcut.
 *
 * `id` defaults to the first sword-ish item the catalogue offers, so a project
 * that renames `sword` still gets a weapon rather than a silent no-op.
 */
async function equipWeapon(id) {
  const list = await ensureItemList();
  if (!list || !list.length) {
    toast('no items to equip — the item list needs the tuner server', true);
    return false;
  }
  const want = id
    || list.find(it => it.id === 'sword')?.id
    || list.find(it => /sword|blade|cleaver|axe/i.test(it.id))?.id
    || list[0].id;
  // The grip context has to name a socket this rig actually publishes, or the
  // item resolves no grip and draws nowhere. Prefer the socket's own name.
  const sock = weaponSocket();
  if (!sock) {
    toast('this rig publishes no socket — add one in the Parts panel first ' +
          '(select the hand, then "+ socket")', true);
    return false;
  }
  if (sock.name) gripCtx = sock.name;
  await loadHeldItem(want);
  if (itemErr) { toast('could not equip ' + want + ': ' + itemErr, true); return false; }
  // loadHeldItem re-points gripCtx at a context the ITEM declares if this one
  // is not among them; say so rather than leaving a sword hanging off nothing.
  if (!itemSc?.grip?.[gripCtx])
    toast(`"${want}" declares no grip for "${gripCtx}" — it will not be held`, true);
  else toast(`equipped ${want} on ${sock.part || sock.name}`);
  rebuildSkeleton();
  ed.invalidate();
  renderAllPanels();
  return true;
}

const weaponEquipped = () => !!(itemDoc && itemSc && heldItemId);

/** Write the item's sidecar back. Separate from the rig save: different file. */
async function saveHeldItem() {
  if (!itemSc || !heldItemId) return;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent('items/' + heldItemId + '.json'),
      { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(itemSc, null, 2) + '\n' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    itemDirty = false;
    toast('saved items/' + heldItemId + '.json');
  } catch (e) {
    toast('item save failed: ' + (e.message || e), true);
  }
  renderAllPanels();
}

/* --- limb library: catalogue, save, load ---------------------------------
 *
 * The three operations the library exists for, in the order you use them:
 * `refreshLibrary` shows the shelf, `saveLimbToLibrary` puts a part on it,
 * `loadLimbFromLibrary` wears one. The FORMAT lives in limblib.js; everything
 * here is I/O and the UI's idea of what is selected.
 */

/** List assets/limbs/, pairing each .vox with its .json. */
async function refreshLibrary() {
  try {
    const r = await fetch('/api/models');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    const files = (j.files || []).filter(f => f.dir === LIB.LIMB_DIR);
    const stems = [...new Set(files.filter(f => f.name.toLowerCase().endsWith('.vox'))
                                   .map(f => f.name.slice(0, -4)))];
    // The .json is fetched per part so the shelf can show limb count and tags
    // without opening any .vox. A part with no .json still lists (it is a
    // usable lump of voxels), it just shows no rig info.
    libParts = await Promise.all(stems.sort().map(async name => {
      const base = { name, limbs: [], root: '', dim: null, from: '' };
      try {
        const rj = await fetch('/api/model?path=' +
                               encodeURIComponent(LIB.partJsonPath(name)));
        if (!rj.ok) return base;
        const meta = JSON.parse(await rj.text());
        return {
          name,
          limbs: (meta.limbs || []).map(l => ({ name: l.name, tag: l.tag })),
          root: meta.root || '',
          dim: meta.meta?.dim || null,
          from: meta.meta?.from || '',
        };
      } catch { return base; }
    }));
    libErr = '';
  } catch (e) {
    libParts = null;
    libErr = 'needs the tuner server (python scripts/tuner_server.py) — ' +
             'a file:// page cannot read assets/limbs/';
  }
}

/**
 * Save the selected limb (with its descendants, unless `single`) to the shelf.
 *
 * Writes the .vox first and the .json second: a part whose geometry landed but
 * whose metadata did not is still loadable, whereas the reverse describes a
 * limb that does not exist.
 */
async function saveLimbToLibrary(root, name, single) {
  const doc = ed.getDoc();
  if (!doc) return;
  let part;
  try {
    part = LIB.partFromPrefab(doc, limbs(), root, {
      group: !single, from: ed.getDocName?.() || '',
    });
  } catch (e) { return toast(String(e.message || e), true); }

  try {
    // The art palette is the DOCUMENT's, and the part's grids hold indices
    // into it — writing the material palette alone would strip every painted
    // colour off the saved limb.
    const palette = VOX.paletteFromMaterials(ed.getMaterials() || []);
    ed.getArtPalette().writeInto(palette);
    const bytes = VOX.writeVox(VOX.prefabToVoxModels(part.prefab), palette,
                               { scene: true });
    const rv = await fetch('/api/model?path=' +
                           encodeURIComponent(LIB.partVoxPath(name)),
      { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
        body: bytes });
    if (!rv.ok) throw new Error('vox ' + rv.status);

    const rj = await fetch('/api/model?path=' +
                           encodeURIComponent(LIB.partJsonPath(name)),
      { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(part.json, null, 2) + '\n' });
    if (!rj.ok) throw new Error('json ' + rj.status);

    toast(`saved ${name} — ${part.names.length} limb` +
          (part.names.length === 1 ? '' : 's'));
    await refreshLibrary();
  } catch (e) {
    toast('limb save failed: ' + (e.message || e), true);
  }
  renderAllPanels();
}

/**
 * Wear a saved part. `replace` is a limb name to swap out (its subtree goes
 * with it); when null the part is ADDED under the selected limb.
 *
 * The joint is what makes this a swap rather than a paste: the incoming root's
 * `anchorLocal` is aligned onto the anchor of whatever it replaces, so the
 * skeleton keeps working and only the shape changes.
 */
async function loadLimbFromLibrary(name, replace) {
  let part;
  try {
    const rv = await fetch('/api/model?path=' +
                           encodeURIComponent(LIB.partVoxPath(name)));
    if (!rv.ok) throw new Error(name + '.vox ' + rv.status);
    const vox = await rv.arrayBuffer();
    let meta = null;
    try {
      const rj = await fetch('/api/model?path=' +
                             encodeURIComponent(LIB.partJsonPath(name)));
      if (rj.ok) meta = JSON.parse(await rj.text());
    } catch { /* a bare .vox is legal — it arrives unrigged */ }
    part = LIB.readPart(vox, meta);
  } catch (e) { return toast('limb load failed: ' + (e.message || e), true); }
  wearPart(part, name, replace);
}

/**
 * Graft an already-parsed part onto the creature. Shared by both shelves: a
 * library part and a limb lifted straight out of another mob file are the same
 * object by the time they get here (see readPart / partFromCreature), and the
 * skeleton surgery below is the part that must not be written twice.
 *
 * `part` is {prefab, palette, root, limbs}; `label` is what to call it in the
 * toast; `replace` is a limb name to swap out (its subtree goes with it), or
 * null to ADD the part under the selected limb.
 */
function wearPart(part, label, replace) {
  if (!part.prefab.models.length) return toast(label + ' has no models', true);
  // ONE undo step for the whole swap. The graft is structural and the skeleton
  // surgery around it is a sidecar edit; recorded separately, Ctrl+Z would put
  // the old models back under a sidecar that has already forgotten them.
  ed.beginStructural();
  try { wearPartInner(part, label, replace); }
  finally { ed.endStructural(replace ? `swap ${label}` : `add ${label}`); }
}

function wearPartInner(part, label, replace) {
  const L = limbs();
  const old = replace ? limbByName(replace) : null;
  // WHERE IT LANDS. Replacing: the joint of the limb going away. Adding: the
  // joint of the limb it is being parented to, which is the closest thing to
  // "where a new part belongs" the rig knows. Neither exists on an unrigged
  // file, and placementFor then drops it at the origin.
  const host = replace ? old : limbByName(selectedPart);
  const target = host && Array.isArray(host.anchor) ? host.anchor : null;
  const parent = replace ? (old && old.parent) : (selectedPart || undefined);

  // The subtree being replaced goes as one: swapping an upper arm cannot
  // leave the old forearm and hand parented to a limb that is gone.
  const doomed = replace ? LIB.subtreeNames(L, replace) : [];

  const taken = new Set(ed.getModels().map(m => m.name)
                          .filter(n => !doomed.includes(n)));
  LIB.renameWithin(part, LIB.uniqueNames(part, taken));

  const at = LIB.placementFor(part, target);
  // Drop the doomed subtree from the SIDECAR here; its MODELS go inside the
  // graft, which removes and adds in one step (see graftModels).
  for (const n of doomed) {
    const k = L.findIndex(l => l.name === n);
    if (k >= 0) L.splice(k, 1);
  }
  // Anything that was parented to a limb now gone re-parents to the incoming
  // root, which is standing in exactly that place. Left dangling it would be
  // an orphan limb the engine refuses to load (mob.cpp:308).
  for (const l of L)
    if (doomed.includes(l.parent)) l.parent = part.root;

  const { shift } = ed.graftModels(
    part.prefab.models.filter(m => !VOX.isArtLayerName(m.name)),
    at, part.palette, doomed);

  // A part grafted below the origin rebases the whole document (graftModels
  // returns by how much). Models moved; anchors did not, so both the incoming
  // ones and every anchor already on the creature have to follow, or the
  // skeleton detaches from the art by exactly this much.
  const at2 = { x: at.x + shift.x, y: at.y + shift.y, z: at.z + shift.z };
  if (shift.x || shift.y || shift.z) {
    for (const l of L)
      if (Array.isArray(l.anchor))
        l.anchor = [l.anchor[0] + shift.x, l.anchor[1] + shift.y,
                    l.anchor[2] + shift.z];
    // A socket is the same kind of thing as an anchor — a point on a limb in
    // prefab coords — so it rebases identically. Missing this puts the sword
    // back where the hand used to be.
    for (const s of sockets())
      if (Array.isArray(s.offset))
        s.offset = [s.offset[0] + shift.x, s.offset[1] + shift.y,
                    s.offset[2] + shift.z];
  }

  for (const e of LIB.sidecarEntries(part, at2, parent)) L.push(e);

  selectedPart = part.root;
  selectedSocket = null;
  touched();
  bindGizmo();
  ed.invalidate();
  toast(`${replace ? 'swapped in' : 'added'} ${label}`);
  renderAllPanels();
}

/* --- the other-creature shelf: catalogue and lift -------------------------
 *
 * A creature file IS a limb library — it just has not been told so. Everything
 * below turns `mobs/asha.vox` + `mobs/asha.json` into the same {prefab,
 * palette, root, limbs} object `LIB.readPart` produces for a saved part, so
 * `wearPart` above cannot tell the two apart.
 */

/** The .vox+.json of one creature, parsed once and kept. */
async function creatureDoc(path) {
  if (crVoxCache.has(path)) return crVoxCache.get(path);
  const rv = await fetch('/api/model?path=' + encodeURIComponent(path),
                         { cache: 'no-store' });
  if (!rv.ok) throw new Error(path + ' ' + rv.status);
  const parsed = VOX.readVox(await rv.arrayBuffer());
  if (!parsed.prefab) throw new Error(path + ' has no models');
  const out = { prefab: parsed.prefab, palette: parsed.palette };
  crVoxCache.set(path, out);
  return out;
}

const sidecarPathOf = p => p.replace(/\.vox$/i, '.json');

/**
 * Scan every OTHER creature for limbs that could be worn here.
 *
 * Only the .json sidecars are read: they carry the whole limb tree (name,
 * parent, tag) and are a couple of KB each, whereas the .vox is the art. So
 * the shelf lists instantly and a file is only parsed when you actually swap
 * one of its limbs in.
 *
 * A "part" here is a limb WITH ITS DESCENDANTS, which is the only unit that
 * makes sense to move: lifting an upper arm and leaving the forearm behind
 * would strand it parented to a limb that no longer exists on either creature.
 * Every limb is offered as a root, so you can take a whole arm or just a hand.
 */
async function refreshCreatures() {
  // A rescan means "the files may have moved on", and the open creature's own
  // entry is the one that actually does: save it and the disk copy this cache
  // holds is a version of the character that no longer exists anywhere.
  crVoxCache.clear();
  try {
    const r = await fetch('/api/models', { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    // mobs/ and models/ only. microvox/ and items/ have no skeletons, and
    // listing thirty plants that can never be swapped in is noise.
    //
    // THE OPEN CREATURE IS INCLUDED, deliberately. It was excluded at first on
    // the reasoning that swapping a limb for itself does nothing — which is
    // true only until you have already swapped it OUT. At that moment the file
    // on disk is the only place the original arm still exists, and leaving it
    // off the shelf means the one limb you most want back is the one limb the
    // list refuses to show. It is marked `self` and sorted first, because
    // "put it back" is the likeliest reason to be reading this list at all.
    const voxes = (j.files || []).filter(f =>
      (f.dir === 'mobs' || f.dir === 'models') &&
      f.name.toLowerCase().endsWith('.vox'));
    const jsons = new Set((j.files || []).map(f => f.path));

    const out = [];
    await Promise.all(voxes.map(async f => {
      const sp = sidecarPathOf(f.path);
      if (!jsons.has(sp)) return;         // no sidecar = no limbs to speak of
      let meta;
      try {
        const rj = await fetch('/api/model?path=' + encodeURIComponent(sp),
                               { cache: 'no-store' });
        if (!rj.ok) return;
        meta = JSON.parse(await rj.text());
      } catch { return; }
      const ls = Array.isArray(meta.limbs) ? meta.limbs : [];
      if (!ls.length) return;
      const from = f.name.replace(/\.vox$/i, '');
      for (const l of ls) {
        if (!l || !l.name) continue;
        const kin = LIB.subtreeNames(ls, l.name);
        out.push({
          path: f.path, from, root: l.name, tag: l.tag || '',
          limbs: kin.map(n => {
            const e = ls.find(x => x.name === n);
            return { name: n, tag: (e && e.tag) || '' };
          }),
        });
      }
    }));
    // Grouped by creature, then in the file's own limb order within it, so the
    // list reads like the rigs it came from rather than one alphabetical mush.
    // The open creature's own saved copy sorts to the front — see above.
    const here = ed.getDocPath();
    out.sort((a, b) => (a.path === here ? 0 : 1) - (b.path === here ? 0 : 1) ||
                       a.from.localeCompare(b.from));
    crParts = out;
    crErr = '';
  } catch (e) {
    crParts = null;
    crErr = 'needs the tuner server (python scripts/tuner_server.py) — ' +
            'a file:// page cannot read the other creatures';
  }
}

/** Lift `root` (and its descendants) out of `path` and wear it here. */
async function loadLimbFromCreature(entry, replace) {
  let part;
  try {
    const src = await creatureDoc(entry.path);
    const sp = sidecarPathOf(entry.path);
    const rj = await fetch('/api/model?path=' + encodeURIComponent(sp),
                           { cache: 'no-store' });
    if (!rj.ok) throw new Error(sp + ' ' + rj.status);
    const meta = JSON.parse(await rj.text());
    // partFromPrefab is the SAVE half of the library, run against a document
    // that happens not to be open. That is the whole trick: it already rebases
    // offsets onto the part's own min corner and converts the prefab `anchor`
    // to the `anchorLocal` a swap needs, which is exactly the coordinate work
    // that would otherwise have to be written a second time here (and is the
    // bug that parks a swapped arm a torso-width out in the air).
    const built = LIB.partFromPrefab({ models: src.prefab.models },
                                     meta.limbs || [], entry.root,
                                     { from: entry.from });
    part = {
      prefab: built.prefab, palette: src.palette,
      root: built.json.root, limbs: built.json.limbs, meta: built.json.meta,
    };
  } catch (e) {
    return toast('limb lift failed: ' + (e.message || e), true);
  }
  wearPart(part, `${entry.from} › ${entry.root}`, replace);
}

/* --- held-item preview: PLACEMENT ----------------------------------------
 *
 * TRANSCRIBED FROM src/game/avatar.cpp EquipItem (the SOCKET x GRIP block,
 * ~L231-311). Same rule as the gait preview in section 5: this renders the
 * exported data through the engine's own formula, it does not invent a
 * placement. If avatar.cpp changes, this is wrong and must follow.
 *
 * The engine's steps, in order:
 *   q         = socket.rotation * grip.rotation      (composed FORWARD)
 *   gripLocal = hilt.center - grip.translation       (hilt box present)
 *             = -grip.translation                    (no hilt box)
 *   the item's ORIGIN goes where that gripLocal, rotated by q, lands the grip
 *   point on the socket:  origin = socketW - Rotate(q, gripLocal)
 *
 * Lengths: the sidecar authors hilt/translation in MICRO units and the engine
 * divides by `scale` at load (avatar.cpp:291 `inv`), so everything below is
 * converted to world voxels before it is composed.
 *
 * FRAMES (melee.cpp, the hilt block): the HILT box is authored in the .vox's
 * own Z-up scene coordinates and the engine maps it (x, z, -y) and rebases it
 * on the model's scene min; `grip.translation` is taken as it stands, in the
 * item's engine (Y-up) frame. This preview used the hilt's raw numbers as if
 * they were already Y-up, which put every hilt centre on the wrong axes -- the
 * preview's blade sat off the game's, and grips tuned here by eye came out
 * displaced in game by exactly that difference (cleaver, shortsword, dagger;
 * corrected 2026-09-27). hiltEngine() is the one conversion both the
 * placement and the wireframe read.
 */

/**
 * The item's hilt box in the ITEM'S ENGINE FRAME, item micro units, measured
 * from the prefab's min corner (the frame the item's voxels are drawn in):
 * { center, size }, or null with no hilt. melee.cpp's conversion exactly:
 * lo = (min.x, min.z, -(min.y + size.y)), extent = (size.x, size.z, size.y),
 * minus modelOrigin = (sceneMin.x, sceneMin.y, sceneMin.z - 1).
 */
function hiltEngine() {
  const h = itemSc?.hilt;
  if (!h || !Array.isArray(h.min) || !Array.isArray(h.size)) return null;
  const mn = h.min.map(Number), sz = h.size.map(Number);
  const m = itemDoc?.models?.find(x => x.name === (itemSc.model || heldItemId)) ||
            itemDoc?.models?.[0];
  const sm = m?.sceneMin || { x: 0, y: 0, z: 0 };
  const off = m?.offset || { x: 0, y: 0, z: 0 };
  return {
    center: {
      x: mn[0] + sz[0] / 2 - sm.x + off.x,
      y: mn[2] + sz[2] / 2 - sm.y + off.y,
      z: -(mn[1] + sz[1] / 2) - (sm.z - 1) + off.z,
    },
    size: { x: sz[0], y: sz[2], z: sz[1] },
  };
}

/** Euler degrees (X then Y then Z, the engine's order) -> quaternion. */
function eulerToQuat(d) {
  const h = v => (v * Math.PI) / 360;                  // deg -> half-radians
  const cx = Math.cos(h(d[0])), sx = Math.sin(h(d[0]));
  const cy = Math.cos(h(d[1])), sy = Math.sin(h(d[1]));
  const cz = Math.cos(h(d[2])), sz = Math.sin(h(d[2]));
  // qz * qy * qx — X applied first, matching Euler order X-then-Y-then-Z.
  return AN.qnorm({
    x: sx * cy * cz - cx * sy * sz,
    y: cx * sy * cz + sx * cy * sz,
    z: cx * cy * sz - sx * sy * cz,
    w: cx * cy * cz + sx * sy * sz,
  });
}

// Rotation of a vector by a quaternion is AN.qrot (anim.cpp:40 QuatRotate) —
// the engine's own transcription, not a second copy of the formula.
const qrot = AN.qrot;

/**
 * Quaternion -> Euler degrees in the engine's X-then-Y-then-Z order, i.e. the
 * exact inverse of eulerToQuat above. Used when a ring drag has to be written
 * back as the three numbers the sidecar stores.
 *
 * Gimbal lock (|pitch| = 90) is handled the standard way: at the singularity
 * roll and yaw are the same rotation, so roll is pinned to 0 and the whole
 * angle is reported as yaw. Without that branch the atan2s go to 0/0 and the
 * fields fill with NaN, which then writes NaN into the JSON.
 */
function quatToEuler(q) {
  const { x, y, z, w } = AN.qnorm(q);
  const deg = r => (r * 180) / Math.PI;
  // The composed matrix is Rz*Ry*Rx (X applied first). Reading the angles off
  // it gives pitch = asin(-m20), roll = atan2(m21, m22), yaw = atan2(m10, m00);
  // the m-terms below are those elements written in quaternion form. Verified
  // against scripts/geometry.py euler_to_quat for a spread of angles — do not
  // swap these for the m02 form, which belongs to the opposite (Rx*Ry*Rz)
  // order and silently returns a DIFFERENT rotation for any mixed-axis angle.
  const m20 = 2 * (x * z - w * y);
  const m21 = 2 * (y * z + w * x);
  const m22 = 1 - 2 * (x * x + y * y);
  const m10 = 2 * (x * y + w * z);
  const m00 = 1 - 2 * (y * y + z * z);
  const s = Math.max(-1, Math.min(1, -m20));
  if (Math.abs(s) >= 0.999999) {
    // Gimbal lock: at pitch = +-90 roll and yaw are the same rotation, so pin
    // roll to 0 and report the whole angle as yaw. Without this branch both
    // atan2s go to 0/0 and NaN reaches the JSON.
    return [0, s > 0 ? 90 : -90, deg(Math.atan2(-2 * (x * y - w * z), 1 - 2 * (x * x + z * z)))];
  }
  return [
    deg(Math.atan2(m21, m22)),
    deg(Math.asin(s)),
    deg(Math.atan2(m10, m00)),
  ];
}

/**
 * Where the socket sits in PREFAB-local space right now, and how it is turned.
 *
 * The socket offset is authored against the hand's rest pose, so under the gait
 * / clip preview it has to ride the hand's animated transform — otherwise the
 * sword hangs in space while the arm swings past it. modelTransform() already
 * returns exactly the delta the viewport applies to that limb's voxels, so the
 * socket goes through the same one and cannot drift from the art.
 */
function socketFrame(sock) {
  const p = { x: +sock.offset[0] || 0, y: +sock.offset[1] || 0, z: +sock.offset[2] || 0 };
  let q = { x: 0, y: 0, z: 0, w: 1 };
  const mi = modelIndexByName(sock.part);
  const xf = mi >= 0 ? modelTransform(mi) : null;
  if (xf) {
    // Same composition drawModel() applies: rotate about the pivot, then move.
    const rel = { x: p.x - xf.pivot.x, y: p.y - xf.pivot.y, z: p.z - xf.pivot.z };
    const r = qrot(xf.quat, rel);
    p.x = r.x + xf.pivot.x + xf.pos.x;
    p.y = r.y + xf.pivot.y + xf.pos.y;
    p.z = r.z + xf.pivot.z + xf.pos.z;
    q = xf.quat;
  }
  return { pos: p, quat: q };
}

/**
 * The item's placement for the viewport: a pivot/quat/pos triple in the same
 * shape modelTransform() returns, so rebuildInstances() draws it with no new
 * code path.
 *
 * Returns null when there is nothing to place.
 */
function heldItemTransform() {
  // THE SOCKET DOES NOT HAVE TO BE SELECTED. This used to be `activeSocket()`,
  // which is the socket the GIZMO is on — so loading an item and not clicking
  // its socket in the rig list drew nothing at all, and the Attacks lane's
  // readout said "no item held" on a rig that was holding one. Selection is
  // about what you are dragging; where the sword hangs is a fact about the
  // grip context. `weaponSocket()` prefers the socket named for that context
  // and still falls back to the selection.
  const sock = weaponSocket();
  if (!sock || !itemDoc || !itemSc) return null;
  const grip = itemSc.grip?.[gripCtx];
  if (!grip) return null;                 // item cannot be held this way

  // DERIVED FROM `artVoxelsPerMetre`, not read off a `scale` key. Both are
  // honoured (skinScaleOf), but no shipped asset states `scale` any more: the
  // human is 80 vox/m and the sword 40, so the true ratio is 8/4 = 2 and the
  // old `+itemSc.scale || 1` read 1/1 and drew every blade at half length.
  // See skinScaleOf for why a wrong blade length is not cosmetic.
  const itemScale = skinScaleOf(itemSc);
  const rs = rigScale();
  // The viewport unit is one RIG file voxel. One item file voxel covers
  // (rigScale / itemScale) viewport units.
  const voxRatio = rs / itemScale * (+grip.scale > 0 ? +grip.scale : 1);

  const frame = socketFrame(sock);
  const sockRot = Array.isArray(sock.rotation) && sock.rotation.length === 3
    ? eulerToQuat(sock.rotation.map(Number)) : { x: 0, y: 0, z: 0, w: 1 };
  const gripRot = eulerToQuat((grip.rotation || [0, 0, 0]).map(Number));
  const q = AN.qnorm(AN.qmul(AN.qmul(frame.quat, sockRot), gripRot));

  // Grip translation and hilt are in item micro units; convert to viewport
  // units (rig micro) by the same ratio.
  const t = (grip.translation || [0, 0, 0]).map(Number);
  const tr = { x: t[0] * voxRatio, y: t[1] * voxRatio, z: t[2] * voxRatio };

  let gl;
  const hilt = hiltEngine();
  if (hilt) {
    const c = {
      x: hilt.center.x * voxRatio,
      y: hilt.center.y * voxRatio,
      z: hilt.center.z * voxRatio,
    };
    gl = { x: c.x - tr.x, y: c.y - tr.y, z: c.z - tr.z };
  } else {
    gl = { x: -tr.x, y: -tr.y, z: -tr.z };
  }

  const rg = qrot(q, gl);
  return {
    pivot: { x: 0, y: 0, z: 0 },
    quat: q,
    pos: { x: frame.pos.x - rg.x, y: frame.pos.y - rg.y, z: frame.pos.z - rg.z },
    scale: voxRatio,
    socket: frame.pos,
  };
}

/**
 * Point the viewport's three socket axis lines at the selected socket, turned
 * by the frame a held item would actually be placed in (socket x grip). With
 * no item loaded they show the bare socket frame, which is the hand's.
 */
function updateSocketAxes() {
  const sock = activeSocket();
  if (!sock) { ed.setSocketAxes(null); ed.setHiltBox?.(null); return; }
  const frame = socketFrame(sock);
  const sockRot = Array.isArray(sock.rotation) && sock.rotation.length === 3
    ? eulerToQuat(sock.rotation.map(Number)) : { x: 0, y: 0, z: 0, w: 1 };
  const grip = itemSc?.grip?.[gripCtx];
  const gripRot = grip ? eulerToQuat((grip.rotation || [0, 0, 0]).map(Number))
                       : { x: 0, y: 0, z: 0, w: 1 };
  ed.setSocketAxes({
    pos: [frame.pos.x, frame.pos.y, frame.pos.z],
    quat: AN.qnorm(AN.qmul(AN.qmul(frame.quat, sockRot), gripRot)),
  });
  updateHiltBox();
}

/**
 * Position the hilt-box wireframe over the held item so the author can see
 * exactly which voxels the fist closes around.
 */
function updateHiltBox() {
  const xf = heldItemTransform();
  const hilt = hiltEngine();
  if (!xf || !hilt) {
    ed.setHiltBox?.(null);
    return;
  }
  const s = xf.scale;
  const r = qrot(xf.quat, { x: hilt.center.x * s, y: hilt.center.y * s, z: hilt.center.z * s });
  ed.setHiltBox?.({
    pos: [r.x + xf.pos.x, r.y + xf.pos.y, r.z + xf.pos.z],
    size: [hilt.size.x * s, hilt.size.y * s, hilt.size.z * s],
    quat: xf.quat,
  });
}

/**
 * Draw the held item's voxels as extra instances on the editor's shared mesh.
 * Called by editor.js's rebuildInstances via the appendExtraInstances hook.
 *
 * The item is a DIFFERENT document (its own .vox, its own origin), so it
 * cannot ride the normal model loop — that is exactly the separation the
 * item/rig split buys, and this is the one place the two are drawn together.
 */
function appendExtraInstances(cubes, n, cap, m4, col) {
  const xf = heldItemTransform();
  if (!xf || !itemDoc) return n;
  const pal = itemDoc.palette || null;
  for (const m of itemDoc.models) {
    const d = m.dim, data = m.grid.data;
    for (let z = 0; z < d.z; z++) {
      for (let y = 0; y < d.y; y++) {
        const row = y * d.x + z * d.x * d.y;
        for (let x = 0; x < d.x; x++) {
          const v = data[row + x];
          if (!v || n >= cap) continue;
          // Item voxels are authored at `scale` micro units per world voxel,
          // so the whole model shrinks by `xf.scale` about the item's origin
          // before it is rotated into the socket frame.
          const p = qrot(xf.quat, {
            x: (x + 0.5 + m.offset.x) * xf.scale,
            y: (y + 0.5 + m.offset.y) * xf.scale,
            z: (z + 0.5 + m.offset.z) * xf.scale,
          });
          m4.makeScale(xf.scale, xf.scale, xf.scale);
          m4.setPosition(p.x + xf.pos.x, p.y + xf.pos.y, p.z + xf.pos.z);
          cubes.setMatrixAt(n, m4);
          // The item's palette is its own file's, NOT the rig's material list:
          // an index means different things in the two documents.
          col.set(itemVoxColor(pal, v));
          cubes.setColorAt(n, col);
          n++;
        }
      }
    }
  }
  return n;
}

/**
 * Colour for one item voxel. The .vox palette index IS the material id by
 * convention (assets/prefabs, scripts/gen_palette.py), so ask the editor's
 * material table first and fall back to the file's own palette.
 */
function itemVoxColor(pal, v) {
  // The material table is the normal path. NOTE this preview draws MATERIAL
  // colour only: the blades (scripts/bladesmith.py) and the armour paint their
  // look in a "<name>.col" art layer, which is filtered out above, so a sword
  // shows here as plain steel and leather rather than as it renders in game.
  // The palette branch below only fires for a hand-made file.
  const mats = ed.getMaterials?.();
  if (mats && mats.length && v < mats.length) return ed.matColorOf(v);
  const c = pal && pal[v];
  if (c) return (c.r << 16) | (c.g << 8) | c.b;
  return 0xc0c0c0;
}

/** The grip block for the active context, created lazily. */
function activeGrip() {
  if (!itemSc) return null;
  const g = itemSc.grip || (itemSc.grip = {});
  const b = g[gripCtx] || (g[gripCtx] = { translation: [0, 0, 0], rotation: [0, 0, 0], scale: 1 });
  if (!Array.isArray(b.translation) || b.translation.length !== 3) b.translation = [0, 0, 0];
  if (!Array.isArray(b.rotation) || b.rotation.length !== 3) b.rotation = [0, 0, 0];
  if (!Number.isFinite(+b.scale)) b.scale = 1;
  return b;
}

// Mutating the sidecar invalidates the preview skeleton — rebuild it so the
// preview never runs against a stale rig. This is the single choke point for
// "the data changed", which is why every edit path calls it.
let _touchTimer = 0;
function touched() {
  ed.touchSidecar();
  rebuildSkeleton();
  clearTimeout(_touchTimer);
  _touchTimer = setTimeout(() => { ed.commitSidecarUndo(); }, 120);
}

// ---- WHICH KEY SAYS HOW DENSE THE ART IS, AND HOW TO MOVE IT ----------------
//
// The three size/detail buttons below all have to agree with `skinScaleOf`
// about where a document states its resolution: the legacy `skinScale` /
// `scale`, or the modern `artVoxelsPerMetre` that every shipped asset uses.
// Until 2026-09-16 they did not — each one read `s.skinScale ?? s.scale`
// directly and then branched on `else if (s.scale != null)`, which NO SHIPPED
// ITEM has. Every failure was silent:
//
//   * "2× size" and "/2 size" changed NOTHING on a modern item and toasted a
//     scale move that had not happened.
//   * "2× detail" doubled the GEOMETRY and left `hilt`, `edge` and
//     `grip.translation` behind on the old lattice — and the runtime puts the
//     hilt box's CENTRE on the rig's socket, so the weapon came back twice the
//     size and floating beside the fist. That is what happened to the mace
//     (owner report 2026-09-16), and it is why all three now go through here.
//
// Returns { get, set, kind } where the value is ART VOXELS PER WORLD VOXEL —
// the same number `skinScaleOf` returns, whichever key the file uses — or null
// for a document that declares no resolution at all (a bare .vox with no
// sidecar key), for which "same world size, finer voxels" is not expressible
// and the caller must say so rather than pretend.
function docDensity(s) {
  const legacy = +(s?.skinScale ?? s?.scale);
  if (Number.isFinite(legacy) && legacy > 0) {
    const key = s.skinScale != null ? 'skinScale' : 'scale';
    return {
      kind: key,
      get: () => legacy,
      set: (n) => {
        // One key wins. A document that carried both would be read one way by
        // `skinScaleOf` and another by whatever touched it last.
        delete s.skinScale; delete s.scale;
        if (n > 1 || key === 'scale') s[key] = n;
      },
    };
  }
  const a = +s?.artVoxelsPerMetre;
  if (Number.isFinite(a) && a > 0)
    return {
      kind: 'artVoxelsPerMetre',
      get: () => a / kVoxelsPerMetre,
      // The authored fact is voxels per METRE; the world scale is derived from
      // it. Halving the physical size means doubling the density, and the art
      // grid is not touched at all — which is why this direction can never
      // detach a hilt from its geometry.
      set: (n) => { s.artVoxelsPerMetre = n * kVoxelsPerMetre; },
    };
  return null;
}

// EVERY SIDECAR LENGTH MEASURED IN THE ART LATTICE, in one place. This list is
// a contract with `LoadItemDefs` (game/melee.cpp): each of these is multiplied
// by `ItemDef::ArtToWorld()` at load, so each of them must move when the art
// grid does. Anything new that is a length in file voxels belongs here, or the
// next "2× detail" silently detaches it from the art it describes.
//
// NOT here, deliberately: `spring.*` (seconds and radians), `hp`,
// `severImpactSpeed` (m/s) and `grip[*].rotation` (degrees) — none of them is
// a length, and scaling them would be a different bug with the same shape.
function scaleItemArtUnits(s, f) {
  if (s.hilt) {
    if (Array.isArray(s.hilt.min)) s.hilt.min = s.hilt.min.map(v => v * f);
    if (Array.isArray(s.hilt.size)) s.hilt.size = s.hilt.size.map(v => v * f);
  }
  if (s.edge)
    for (const k of ['from', 'to', 'halfWidth'])
      if (s.edge[k] != null) s.edge[k] = +s.edge[k] * f;
  for (const ctx of Object.values(s.grip || {}))
    if (Array.isArray(ctx.translation))
      ctx.translation = ctx.translation.map(v => v * f);
  // A worn piece (robe, cuirass) measures its shells against the WEARER's limb
  // box, but in the ITEM's art units — melee.cpp multiplies both by the same
  // `invScale`, so both scale here too.
  for (const c of (Array.isArray(s.cover) ? s.cover : []))
    for (const k of ['offset', 'fitBox'])
      if (Array.isArray(c[k])) c[k] = c[k].map(v => v * f);
}

/**
 * Upscale the whole document 2×: geometry (editor.js doubles every model and
 * offset), then everything in the sidecar that is measured in voxels —
 * anchors, clip pos keys and an item's hilt/edge/grip — and finally the
 * density key, so the asset keeps its world size and gains detail. This is THE
 * way to give an existing mob finer microvoxels: 40 → 80 art voxels/metre
 * halves the voxel size without moving a joint or changing a clip's meaning.
 *
 * This function is also the authoritative INVENTORY of which sidecar fields
 * are measured in micro units: limbs[].anchor, clips[*].tracks[*].pos[].v,
 * editor.parts[].box, everything in `scaleItemArtUnits`, plus the model grids
 * and offsets editor.js handles. gait.rideHeight, gait.stepHeight and
 * states[].bodyYOffset are WORLD units and are deliberately untouched —
 * doubling them would raise the creature off the ground by exactly the amount
 * it just gained in detail.
 */
function upscale2x() {
  const s = sc();
  const den = docDensity(s);
  const scl = den ? den.get() : 1;
  if (den && scl >= 8) {
    toast('already at scale 8 — the finest the engine accepts', true);
    return;
  }
  if (!den && !confirm(
      'This file declares no artVoxelsPerMetre, so doubling the geometry also ' +
      'doubles its WORLD SIZE — there is no density key to compensate.\n\n' +
      'Upscale anyway?')) return;
  const scaleNote = den
    ? `, lengths double, and ${den.kind} follows (same world size, finer detail)`
    : '';
  if (!confirm('Upscale 2×? Every voxel becomes a 2×2×2 block' + scaleNote +
      '.')) return;
  // Geometry and sidecar are two halves of ONE edit — undoing the doubled
  // grids while the anchors stayed doubled would detach every joint from the
  // art by exactly a factor of two.
  ed.beginStructural();
  let ok = false;
  try { ok = upscale2xInner(s, den, scl); }
  finally { ed.endStructural('2× detail', ok); }
}

function upscale2xInner(s, den, scl) {
  if (!ed.upscaleDoc()) return false;    // toasts its own reason on failure
  for (const l of limbs())
    if (Array.isArray(l.anchor) && l.anchor.length === 3)
      l.anchor = l.anchor.map(v => v * 2);
  for (const c of Object.values(clips()))
    for (const tr of Object.values(c.tracks || {}))
      for (const k of (tr.pos || []))
        if (Array.isArray(k.v)) k.v = k.v.map(v => v * 2);
  const parts = s.editor?.parts;
  if (parts)
    for (const p of Object.values(parts))
      if (Array.isArray(p.box))
        // lo doubles; hi is an inclusive cell index, so its block ends at 2h+1.
        p.box = [p.box[0].map(v => v * 2), p.box[1].map(v => v * 2 + 1)];
  // UNCONDITIONAL, and that is the fix. These fields are art units whether the
  // document is a rigged mob or a standalone weapon, and whether it states a
  // `scale` or an `artVoxelsPerMetre`; the old code only reached them down the
  // one branch no shipped file takes.
  scaleItemArtUnits(s, 2);
  if (den) den.set(scl * 2);
  touched();
  bindGizmo();
  ed.refreshMicroGhost?.();
  ed.invalidate();
  renderAllPanels();
  toast('upscaled 2×' + (den
    ? ` — ${den.kind} follows: same world size, ${scl * 2}× voxel density`
    : ' — the model is twice the resolution (and twice the world size)'));
  return true;
}

// ---- THE TWO SIZE BUTTONS ---------------------------------------------------
//
// Neither touches geometry OR any art-unit length: they move the density key
// alone, which is exactly why they cannot move a hilt off its grip. What
// changes is how big one art voxel is in the world, and every authored length
// is in art voxels, so the whole weapon — grip included — scales as one piece.
//
// `resizeDoc(f)` is "multiply the WORLD size by f".
function resizeDoc(f, what) {
  const s = sc();
  const den = docDensity(s);
  if (!den) {
    toast('this file declares no artVoxelsPerMetre (or scale) — nothing to ' +
          'resize against; add one on the Items panel first', true);
    return;
  }
  const scl = den.get();
  const next = scl / f;          // bigger world size = coarser = lower density
  // The engine's legal ladder: `SkinScaleFor` takes a whole number of art
  // voxels per world voxel and `MicroBodyPack` refuses past 8. Below 1 the art
  // would have to be downsampled, which sim/scale.h declines to define.
  if (next < 1 || next > 8 || !Number.isInteger(next)) {
    toast(next > 8
      ? 'already at scale 8 — the finest the engine accepts'
      : 'already at scale 1 — the coarsest the engine accepts', true);
    return;
  }
  den.set(next);
  touched(); ed.refreshMicroGhost?.(); ed.invalidate();
  renderAllPanels();
  toast(`${what}: ${den.kind} ${den.kind === 'artVoxelsPerMetre'
    ? `${scl * kVoxelsPerMetre} → ${next * kVoxelsPerMetre}/m`
    : `${scl} → ${next}`} — same voxels, grip and edge move with them`);
}

function growSize2x() { resizeDoc(2, 'twice the world size'); }
function shrinkSize2x() { resizeDoc(0.5, 'half the world size'); }

// A limb entry for every model that does not have one yet. This is how a
// freshly split model becomes riggable without hand-editing JSON.
function syncLimbsToModels() {
  const models = ed.getModels();
  const L = limbs();
  let added = 0;
  for (const m of models) {
    if (!m.name || L.some(l => l.name === m.name)) continue;
    L.push({ name: m.name, hp: 10, severable: true });
    added++;
  }
  if (added) { touched(); toast(`added ${added} limb entr${added === 1 ? 'y' : 'ies'}`); }
  return added;
}

/* ==========================================================================
   3. parts / rig panel
   ========================================================================== */

const JOINTS = ['ball', 'hinge', 'fixed'];

// Duplicate limb names silently break the engine (mob.cpp resolves `parent` by
// name and FindModel matches on it), so this is validated inline and loudly.
function nameError(limb, value) {
  const v = String(value || '').trim();
  if (!v) return 'name cannot be empty';
  if (limbs().some(l => l !== limb && l.name === v)) return 'duplicate name';
  if (!ed.getModels().some(m => m.name === v))
    return 'no model named "' + v + '" in this file';
  return null;
}

function field(label, input, hint) {
  return el('div', { class: 'rigf' },
    el('span', {}, label), input,
    hint ? el('i', {}, hint) : null);
}

function numInput(obj, key, opts = {}) {
  const i = el('input', {
    class: 'cell num', type: 'number',
    step: opts.step ?? 'any', placeholder: opts.ph ?? '',
  });
  i.value = obj[key] ?? '';
  i.addEventListener('change', () => {
    const v = i.value.trim();
    if (v === '') delete obj[key];
    else obj[key] = opts.int ? Math.round(+v) : +v;
    touched();
    opts.after?.();
  });
  return i;
}

function checkInput(obj, key) {
  const c = el('input', { type: 'checkbox' });
  c.checked = !!obj[key];
  c.addEventListener('change', () => {
    // Write the value explicitly rather than deleting on false: `severable`
    // and `vital` both default differently in mob.cpp, so an absent key is
    // not the same as false.
    obj[key] = c.checked;
    touched();
  });
  return c;
}

/**
 * Editable x×y×z box size, in the model list row. Typing a bigger number grows
 * that limb's box so there is empty space to paint into; a smaller one crops it.
 *
 * Growth goes on the HIGH side by default, which keeps the model's min corner
 * (and therefore every voxel's position and the limb's anchor) exactly where it
 * is — the common case is "make room above/ahead". Hold Shift while committing
 * to grow from the LOW side instead, for when the room is needed below/behind.
 *
 * Cropping cannot be undone by re-growing, because the voxels outside the new
 * box are gone, so it asks first.
 */
function dimInput(m, mi) {
  const wrap = el('span', { class: 'rigdim rigdimedit' });
  const boxes = ['x', 'y', 'z'].map(axis => {
    const b = el('input', {
      class: 'cell num', type: 'number', min: '1', step: '1',
      title: `${axis} size — grows on the high side, Shift+Enter grows the low side`,
    });
    b.value = m.dim[axis];
    b.addEventListener('click', e => e.stopPropagation());
    const commit = shift => {
      const want = Math.round(+b.value);
      if (!Number.isFinite(want) || want < 1) { b.value = m.dim[axis]; return; }
      const d = want - m.dim[axis];
      if (!d) return;
      if (d < 0 && !confirm(
        `Shrink ${m.name} ${axis} from ${m.dim[axis]} to ${want}? ` +
        'Voxels outside the new box are deleted.')) { b.value = m.dim[axis]; return; }
      const zero = { x: 0, y: 0, z: 0 };
      const side = shift ? 'lo' : 'hi';
      const pad = { lo: { ...zero }, hi: { ...zero } };
      pad[side][axis] = d;
      if (!ed.growModel(mi, pad)) { b.value = m.dim[axis]; return; }
      bindGizmo();
      renderAllPanels();
      toast(`${m.name} ${axis} ${want} (${d > 0 ? '+' : ''}${d} on the ` +
        `${shift ? 'low' : 'high'} side)`);
    };
    b.addEventListener('keydown', e => {
      if (e.key === 'Enter') { e.preventDefault(); commit(e.shiftKey); }
    });
    b.addEventListener('change', () => commit(false));
    return b;
  });
  wrap.append(boxes[0], el('i', {}, '×'), boxes[1], el('i', {}, '×'), boxes[2]);
  return wrap;
}

/**
 * Relative move for a whole limb: three deltas plus Apply. Deltas rather than
 * an absolute position, because a limb's position is its model offset, which
 * is not otherwise surfaced — "up 5" is the question actually being asked, and
 * an absolute field would force reading the offset out of the .vox first.
 *
 * Applies to the MODEL of the same name; ed.moveModel carries the anchor so
 * the joint stays inside the mesh. Units are file voxels — at scale 4 that is
 * micro-voxels, so one world voxel is 4 here (the hint says so live).
 */
function moveInput(limb) {
  const wrap = el('div', { class: 'rigvec' });
  const boxes = [0, 1, 2].map(() => {
    const b = el('input', { class: 'cell num', type: 'number', step: '1', value: '0' });
    return b;
  });
  const apply = () => {
    const d = { x: num(boxes[0].value, 0), y: num(boxes[1].value, 0),
                z: num(boxes[2].value, 0) };
    if (!d.x && !d.y && !d.z) return;
    const mi = ed.getModels().findIndex(m => m.name === limb.name);
    if (mi < 0) { toast(`no model named "${limb.name}"`, true); return; }
    ed.pushMoveUndo([mi]);
    if (!ed.moveModel(mi, d)) return;
    ed.finishMoveUndo([mi]);
    boxes.forEach(b => { b.value = '0'; });
    rebuildSkeleton();
    bindGizmo();
    ed.invalidate();
    renderAllPanels();
    const scl = +(sc().skinScale ?? sc().scale) || 1;
    const w = a => (a / scl).toFixed(scl > 1 ? 2 : 0);
    toast(`moved ${limb.name} by ${d.x},${d.y},${d.z}` +
      (scl > 1 ? ` (${w(d.x)},${w(d.y)},${w(d.z)} world voxels)` : ''));
  };
  boxes.forEach(b => {
    b.addEventListener('keydown', e => { if (e.key === 'Enter') apply(); });
    wrap.append(b);
  });
  wrap.append(el('button', { class: 'small', title: 'apply the delta', onclick: apply }, '→'));
  return wrap;
}

// [x,y,z] triple bound to a limb key, e.g. anchor or axis.
function vecInput(obj, key, dflt, after) {
  const wrap = el('div', { class: 'rigvec' });
  const cur = () => (Array.isArray(obj[key]) && obj[key].length === 3)
    ? obj[key] : dflt.slice();
  [0, 1, 2].forEach(i => {
    const b = el('input', { class: 'cell num', type: 'number', step: '0.5' });
    b.value = cur()[i];
    b.addEventListener('change', () => {
      const v = cur().slice();
      v[i] = num(b.value, dflt[i]);
      obj[key] = v;
      touched();
      after?.();
    });
    wrap.append(b);
  });
  return wrap;
}

function renderRigPanel() {
  if (!sideEl) return;
  sideEl.innerHTML = '';

  const models = ed.getModels();

  /* ---- model list ----
     This IS the limb list: one model per limb is the engine's format, so a
     row selects the model AND opens that limb's editor inline below it.
     They used to be two lists at opposite ends of the panel, which meant
     clicking a model did nothing visible and the limb had to be found again
     further down. Grouped by skeleton chain when a sidecar is present. */
  const list = el('div', { class: 'riglist' });

  function buildModelRow(m, i) {
    const limbHere = limbByName(m.name);
    const openHere = !!limbHere && limbHere.name === selectedPart;
    const isSelected = selectedParts.has(m.name);
    const row = el('div', {
      class: 'rigrow' + (i === ed.getActiveModel() ? ' on' : '')
                       + (isSelected ? ' multi' : ''),
      onclick: (ev) => {
        if (ev.shiftKey) {
          if (selectedParts.has(m.name)) selectedParts.delete(m.name);
          else selectedParts.add(m.name);
          ed.invalidate();
          renderAllPanels();
          return;
        }
        selectedParts.clear();
        ed.setActiveModel(i);
        ed.setSelection({
          lo: [0, 0, 0],
          hi: [m.dim.x - 1, m.dim.y - 1, m.dim.z - 1],
          model: i,
        });
        selectedPart = (limbHere && !openHere) ? m.name : null;
        selectedSocket = null;
        bindGizmo();
        ed.invalidate();
        renderAllPanels();
      },
    });
    const nm = el('input', { class: 'cell id', value: m.name });
    nm.addEventListener('click', e => e.stopPropagation());
    nm.addEventListener('change', () => {
      const old = m.name;
      // The model name and every sidecar reference to it move together, so
      // they are one undo step: a rename that came back on the model but not
      // on its limb entry would orphan the limb (mob.cpp joins them by name
      // and by nothing else).
      ed.beginStructural();
      let ok = false;
      try {
        if (!ed.renameModel(i, nm.value)) { nm.value = old; return; }
        const l = limbByName(old);
        if (l) { l.name = m.name; touched(); }
        for (const o of limbs()) if (o.parent === old) { o.parent = m.name; touched(); }
        ok = true;
      } finally { ed.endStructural(`rename ${old}`, ok); }
      renderAllPanels();
    });
    row.append(
      dimInput(m, i),
      nm,
      // The title FLIPS when lit, like the hide button next to it. A control
      // whose tooltip still says "solo" while it is already soloing does not
      // tell you it is also the way back.
      el('button', {
        class: 'icon' + (ed.isModelSolo(m.name) ? ' on' : ''),
        title: ed.isModelSolo(m.name)
          ? 'soloed — click to show the whole body again ' +
            '(shift-click to drop just this one from the solo)'
          : 'solo — show only this limb (shift-click to add it to the solo)',
        onclick: e => {
          e.stopPropagation();
          ed.toggleModelSolo(m.name, e.shiftKey);
          renderAllPanels();
        },
      }, 'S'),
      el('button', {
        class: 'icon' + (ed.isModelHidden(m.name) ? ' on' : ''),
        title: ed.isModelHidden(m.name) ? 'hidden — click to show'
                                        : 'hide this limb (also unpickable)',
        onclick: e => { e.stopPropagation(); ed.toggleModelHidden(m.name); renderAllPanels(); },
      }, ed.isModelHidden(m.name) ? '◌' : '◉'),
      el('button', {
        class: 'icon', title: 'duplicate',
        onclick: e => { e.stopPropagation(); ed.duplicateModel(i); renderAllPanels(); },
      }, '⧉'),
      el('button', {
        class: 'icon danger', title: 'delete model',
        onclick: e => {
          e.stopPropagation();
          if (!confirm(`Delete model "${m.name}"?`)) return;
          ed.removeModel(i);
          renderAllPanels();
        },
      }, '✕'));
    return { row, openHere, limbHere };
  }

  // Group models by skeleton chain when a sidecar with limbs is present.
  const allLimbs = limbs();
  const limbMap = new Map();
  for (const l of allLimbs) limbMap.set(l.name, l);
  const modelNames = new Set(models.map(m => m.name));

  const spineNames = new Set();
  if (allLimbs.length >= 2) {
    const isSided = name => /\.[A-Z]*[LR]$/i.test(name);
    const childrenOf = name =>
      allLimbs.filter(l => l.parent === name && modelNames.has(l.name));
    const roots = allLimbs.filter(l => !l.parent || !limbMap.has(l.parent));
    function walkSpine(name) {
      spineNames.add(name);
      const kids = childrenOf(name);
      // Continue along unsided children only (sided = branch).
      const unsided = kids.filter(k => !isSided(k.name));
      for (const k of unsided) walkSpine(k.name);
    }
    for (const r of roots) walkSpine(r.name);
  }

  function descendants(rootName) {
    const result = [rootName];
    const queue = [rootName];
    while (queue.length) {
      const p = queue.shift();
      for (const l of allLimbs) {
        if (l.parent === p && l.name !== rootName && modelNames.has(l.name)) {
          result.push(l.name);
          queue.push(l.name);
        }
      }
    }
    return result;
  }

  const groups = [];
  const placed = new Set();
  if (spineNames.size > 0) {
    const bodyMembers = [];
    for (const m of models)
      if (spineNames.has(m.name)) bodyMembers.push(m.name);
    if (bodyMembers.length) {
      groups.push({ label: 'body', members: bodyMembers });
      bodyMembers.forEach(n => placed.add(n));
    }
    for (const sn of spineNames) {
      const branches = allLimbs.filter(l =>
        l.parent === sn && modelNames.has(l.name) && !spineNames.has(l.name));
      for (const br of branches) {
        const chain = descendants(br.name);
        let label = br.name;
        const dot = label.indexOf('.');
        if (dot > 0) {
          const side = label.slice(dot);
          const base = label.slice(0, dot).replace(/[UL]$/, '');
          label = base + side;
        }
        groups.push({ label, members: chain });
        chain.forEach(n => placed.add(n));
      }
    }
  }
  const ungrouped = models.filter(m => !placed.has(m.name)).map(m => m.name);
  if (ungrouped.length) {
    if (groups.length) groups.push({ label: 'other', members: ungrouped });
    else ungrouped.forEach(n => placed.add(n));
  }

  const modelIndex = new Map();
  models.forEach((m, i) => modelIndex.set(m.name, i));

  if (groups.length >= 2) {
    for (const g of groups) {
      const collapsed = collapsedGroups.has(g.label);
      const allNames = g.members;
      const groupHead = el('div', {
        class: 'riggroup' + (collapsed ? ' collapsed' : ''),
        onclick: () => {
          if (collapsed) collapsedGroups.delete(g.label);
          else collapsedGroups.add(g.label);
          renderAllPanels();
        },
      });
      const arrow = el('span', { class: 'rigarrow' }, collapsed ? '▸' : '▾');
      const selectAll = el('button', {
        class: 'icon', title: 'shift-select all in this group',
        onclick: e => {
          e.stopPropagation();
          const allIn = allNames.every(n => selectedParts.has(n));
          for (const n of allNames) {
            if (allIn) selectedParts.delete(n);
            else selectedParts.add(n);
          }
          ed.invalidate();
          renderAllPanels();
        },
      }, '☰');
      groupHead.append(arrow, el('span', { class: 'rigglabel' }, g.label), selectAll);
      list.append(groupHead);
      if (!collapsed) {
        for (const name of allNames) {
          const idx = modelIndex.get(name);
          if (idx === undefined) continue;
          const { row, openHere, limbHere } = buildModelRow(models[idx], idx);
          list.append(row);
          if (openHere) list.append(limbBody(limbHere));
        }
      }
    }
  } else {
    // No meaningful grouping — flat list as before.
    models.forEach((m, i) => {
      const { row, openHere, limbHere } = buildModelRow(m, i);
      list.append(row);
      if (openHere) list.append(limbBody(limbHere));
    });
  }

  const anyHidden = models.some(m => ed.isModelHidden(m.name)) || ed.anySolo();
  sideEl.append(
    el('div', { class: 'righdr' }, 'Models',
      el('span', { class: 'spacer' }),
      // Only offered when something IS hidden — otherwise a limb left hidden
      // from an earlier session reads as missing geometry with no way back.
      anyHidden ? el('button', {
        class: 'small', title: 'clear every hide/solo',
        onclick: () => { ed.showAllModels(); renderAllPanels(); },
      }, 'show all') : null,
      el('button', {
        class: 'small',
        title: 'double the resolution: every voxel becomes 2×2×2, and every ' +
          'authored length doubles with it — anchors, pos keys, and an item’s ' +
          'hilt, edge and grip. Same world size, finer voxels',
        onclick: upscale2x,
      }, '2× detail'),
      el('button', {
        class: 'small',
        title: 'double world size: halve the density (artVoxelsPerMetre 80 → 40) ' +
          'so each voxel is bigger. No geometry and no authored length changes, ' +
          'so the grip stays exactly where it is. Fully reversible',
        onclick: growSize2x,
      }, '2× size'),
      el('button', {
        class: 'small',
        title: 'halve world size: double the density (artVoxelsPerMetre 40 → 80) ' +
          'so each voxel is smaller. No geometry and no authored length changes, ' +
          'so the grip stays exactly where it is. Fully reversible',
        onclick: shrinkSize2x,
      }, '/2 size'),
      el('button', {
        class: 'small', title: 'add an empty model to this file',
        onclick: () => {
          const s = prompt('new model dimensions, "x y z" or one number', '8 8 8');
          if (!s) return;
          const p = s.trim().split(/[\s,x×]+/).map(Number).filter(n => n > 0);
          if (!p.length) return toast('could not parse dimensions', true);
          const [a, b, c] = p.length === 1 ? [p[0], p[0], p[0]]
            : [p[0], p[1] ?? p[0], p[2] ?? p[0]];
          ed.addModel(a, b, c, 'model');
          renderAllPanels();
        },
      }, '+ model')),
    list);

  /* ---- selection actions ---- */
  const selBox = ed.getSelection();
  sideEl.append(el('div', { class: 'righdr' }, 'Selection'));
  if (!selBox) {
    sideEl.append(el('div', { class: 'rignote' },
      'Pick the Select [V] brush and drag a box to define a part.'));
  } else {
    const n = (selBox.hi[0] - selBox.lo[0] + 1) * (selBox.hi[1] - selBox.lo[1] + 1) *
              (selBox.hi[2] - selBox.lo[2] + 1);
    sideEl.append(
      el('div', { class: 'rignote' },
        `box ${selBox.lo.join(',')} → ${selBox.hi.join(',')} (${n} cells)`),
      el('div', { class: 'rigbtns' },
        el('button', {
          class: 'small',
          title: 'record the box in editor.parts (engine ignores it)',
          onclick: () => {
            const name = prompt('part name for this selection', selectedPart || 'part');
            if (!name) return;
            const s = sc();
            s.editor = s.editor || {};
            s.editor.parts = s.editor.parts || {};
            s.editor.parts[name] = { box: [selBox.lo.slice(), selBox.hi.slice()] };
            touched();
            toast(`recorded editor.parts["${name}"]`);
            renderAllPanels();
          },
        }, 'Make Part'),
        el('button', {
          class: 'small primary',
          title: 'extract the selection into its own model (one model per limb)',
          onclick: () => {
            const name = prompt('name for the new model / limb', 'limb');
            if (!name) return;
            if (!ed.splitSelectionToModel(name)) return;
            syncLimbsToModels();
            renderAllPanels();
          },
        }, 'Split to model')));
  }

  /* ---- rig-wide settings (the per-limb editors are inline above) ---- */
  sideEl.append(
    el('div', { class: 'righdr' }, 'Rig',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small', title: 'create a limb entry for every model that lacks one',
        onclick: () => { if (!syncLimbsToModels()) toast('every model already has a limb'); renderAllPanels(); },
      }, 'sync')));

  // A CREATURE THAT WEARS ANOTHER FILE'S ART (mobs/zombie.json). Everything
  // below is the MERGED rig — the base's, with this file's overrides poured
  // over it — so it previews, swings and reports exactly as the engine's does,
  // and nothing here can be written back. Said up front rather than letting a
  // refused save be the first news of it.
  {
    const d = ed.derivedInfo?.();
    if (d)
      sideEl.append(el('div', { class: 'rignote atkwarn' },
        'READ-ONLY: ' + d.sidecarPath + ' has no art of its own — it wears ' +
        d.voxPath + ' and overrides that rig by RFC 7396 merge-patch. What is ' +
        'shown is the MERGE; the file on disk is the override alone, so ' +
        'saving would flatten the base rig into it. Edit ' + d.sidecarPath +
        ' by hand for an override, or open ' + d.voxPath + ' to change the ' +
        'art every creature extending it wears.'));
  }

  const L = limbs();
  if (!L.length) {
    sideEl.append(el('div', { class: 'rignote' },
      'No limbs yet. "sync" creates one entry per model; a mob also needs ' +
      '"root" set to the limb everything hangs off.'));
  }

  // root selector, once there is anything to point at
  if (L.length) {
    const s = sc();
    const rootSel = el('select', { class: 'cell' });
    rootSel.append(el('option', { value: '' }, '(no root)'));
    for (const l of L) rootSel.append(el('option', { value: l.name }, l.name));
    rootSel.value = s.root || '';
    rootSel.addEventListener('change', () => {
      if (rootSel.value) s.root = rootSel.value; else delete s.root;
      touched();
    });
    sideEl.append(field('root', rootSel, 'the limb the rest hang off'));

    const spd = numInput(s, 'speed', { ph: '5.0' });
    sideEl.append(field('speed', spd, 'm/s; also drives preview cadence'));

    // Micro authoring scale (validated in mob.cpp LoadMobDefs, ~line 229):
    // voxels in this file are SKIN units, `skinScale` of them per world voxel.
    // This is how a mob gets more detail than one world voxel per cube — model
    // it big here, set the scale, and the engine shrinks it. Rig, anchors and
    // clips are all authored in the same units, so nothing else in this panel
    // changes.
    //
    // Writes "skinScale". The engine derives the COLLIDER resolution from the
    // art (the finest of {8,4,2,1} whose limbs still fit the DebrisVoxel int8
    // bound) and logs what it picked at load — it is not authored here, because
    // the bound it satisfies is a property of how big the limbs are. The older
    // "scale" key is still read, and means both lattices are equal.
    const sclSel = el('select', { class: 'cell' });
    for (const v of [1, 2, 4, 8]) sclSel.append(el('option', { value: v }, String(v)));
    sclSel.value = String(+(s.skinScale ?? s.scale) || 1);
    sclSel.addEventListener('change', () => {
      const v = +sclSel.value;
      // One key wins: never leave both behind for the loader to choose between.
      delete s.scale;
      if (v > 1) s.skinScale = v; else delete s.skinScale;
      touched();
      ed.refreshMicroGhost?.();
      ed.invalidate();
    });
    const scl = +(s.skinScale ?? s.scale) || 1;
    const wsz = ed.getDoc()?.size || { x: 0, y: 0, z: 0 };
    sideEl.append(field('skinScale', sclSel,
      scl > 1
        ? `skin voxels/world voxel — draws ${Math.ceil(wsz.x / scl)}×` +
          `${Math.ceil(wsz.y / scl)}×${Math.ceil(wsz.z / scl)} world voxels ` +
          '(the collider is derived coarser; the engine logs which)'
        : 'skin voxels per world voxel (2/4/8 = finer-than-terrain detail)'));
  }

  // Limbs with no model of the same name cannot be reached from the model
  // list, so they still get a row here — otherwise a typo'd name would make
  // the limb uneditable and invisible.
  const orphans = L.filter(l => !models.some(m => m.name === l.name));
  if (orphans.length)
    sideEl.append(el('div', { class: 'rignote' },
      'These limb entries name no model in this file — fix the name or ' +
      'remove them; the engine matches limb to model BY NAME.'));
  for (const limb of orphans) {
    const open = limb.name === selectedPart;
    const head = el('div', {
      class: 'rigrow' + (open ? ' on' : ''),
      onclick: () => {
        selectedPart = open ? null : limb.name;
        selectedSocket = null;   // picking a limb drops a socket drag
        bindGizmo();
        ed.invalidate();
        renderAllPanels();
      },
    }, el('span', { class: 'rigdot' }), limb.name || '(unnamed)');
    sideEl.append(head);
    if (open) sideEl.append(limbBody(limb));
  }

  renderRigTail();
}

/** The per-limb editor, rendered inline under its row in the model list. */
function limbBody(limb) {
  {
    const body = el('div', { class: 'rigbody' });

    // name, with inline validation
    const nameIn = el('input', { class: 'cell id', value: limb.name || '' });
    const nameErr = el('i', { class: 'rigerr' }, '');
    const validateName = () => {
      const e = nameError(limb, nameIn.value);
      nameIn.classList.toggle('bad', !!e);
      nameErr.textContent = e || '';
      return !e;
    };
    nameIn.addEventListener('input', validateName);
    nameIn.addEventListener('change', () => {
      if (!validateName()) return;
      const old = limb.name;
      limb.name = nameIn.value.trim();
      for (const o of limbs()) if (o.parent === old) o.parent = limb.name;
      if (sc().root === old) sc().root = limb.name;
      if (selectedPart === old) selectedPart = limb.name;
      touched();
      renderAllPanels();
    });
    validateName();
    body.append(field('name', nameIn), nameErr);

    // parent
    const par = el('select', { class: 'cell' });
    par.append(el('option', { value: '' }, '(none — root)'));
    // limbs() rather than a captured list: limbBody is called from two places
    // (inline under a model row, and under an orphan row) and neither is inside
    // renderRigPanel's scope, where the old `L` lived.
    for (const o of limbs()) if (o !== limb) par.append(el('option', { value: o.name }, o.name));
    par.value = limb.parent || '';
    par.addEventListener('change', () => {
      if (par.value) limb.parent = par.value; else delete limb.parent;
      touched();
      bindGizmo();
      ed.invalidate();
    });
    body.append(field('parent', par));

    // joint
    const jt = el('select', { class: 'cell' });
    for (const j of JOINTS) jt.append(el('option', { value: j }, j));
    jt.value = limb.joint || 'ball';
    jt.addEventListener('change', () => { limb.joint = jt.value; touched(); });
    body.append(field('joint', jt));

    body.append(field('hp', numInput(limb, 'hp', { int: true, ph: '10' })));
    body.append(field('severable', checkInput(limb, 'severable')));
    body.append(field('vital', checkInput(limb, 'vital'),
      'losing a vital limb kills the mob'));
    body.append(field('tag', (() => {
      const t = el('input', { class: 'cell id', value: limb.tag || '', placeholder: 'leg' });
      t.addEventListener('change', () => {
        if (t.value.trim()) limb.tag = t.value.trim(); else delete limb.tag;
        touched();
      });
      return t;
    })(), 'gait/chain queries go by tag'));

    body.append(field('move', moveInput(limb),
      'shift this limb\'s voxels AND its anchor — or drag it with Move [X]'));

    body.append(field('anchor',
      vecInput(limb, 'anchor', [0, 0, 0], () => { bindGizmo(); ed.invalidate(); }),
      'joint position in prefab coords — drag the orange ball'));
    body.append(field('axis',
      vecInput(limb, 'axis', [1, 0, 0], () => { bindGizmo(); ed.invalidate(); }),
      'hinge/swing axis (engine default 1,0,0)'));

    body.append(field('swingAmp', numInput(limb, 'swingAmp', { ph: '0', after: () => ed.invalidate() }),
      'radians'));
    body.append(field('swingPhase', numInput(limb, 'swingPhase', { ph: '0', after: () => ed.invalidate() }),
      'in units of π'));
    body.append(field('minAngle', numInput(limb, 'minAngle', { ph: '' })));
    body.append(field('maxAngle', numInput(limb, 'maxAngle', { ph: '' })));
    body.append(field('severImpactSpeed', numInput(limb, 'severImpactSpeed', { ph: '' }),
      'fast hit severs regardless of hp'));

    body.append(el('div', { class: 'rigbtns' },
      el('button', {
        class: 'small danger',
        onclick: () => {
          if (!confirm(`Remove limb entry "${limb.name}"? (the model stays)`)) return;
          const i = limbs().indexOf(limb);
          if (i >= 0) limbs().splice(i, 1);
          if (selectedPart === limb.name) selectedPart = null;
          touched();
          bindGizmo();
          renderAllPanels();
        },
      }, 'remove limb')));

    return body;
  }
}

/** Everything below the per-limb editors: chains, gait, clips. */
/* ---- limb library shelf -------------------------------------------------
 *
 * Sits directly under the model list because that is where it is used: you
 * pick a limb up there, then save it or swap it down here, and both buttons
 * name the limb they will act on rather than "the selection". A part that
 * cannot be applied says why on the button rather than being hidden — an
 * absent control reads as a broken feature.
 */
function renderLibrary() {
  const onCreatures = libSource === 'creatures';
  const hdr = el('div', { class: 'righdr' }, 'Limb library',
    el('span', { class: 'spacer' }),
    el('button', {
      class: 'icon', title: libOpen ? 'collapse' : 'expand',
      onclick: () => { libOpen = !libOpen; renderAllPanels(); },
    }, libOpen ? '▾' : '▸'),
    el('button', {
      class: 'icon',
      title: onCreatures ? 'rescan the other creatures' : 'rescan assets/limbs/',
      onclick: async () => {
        if (onCreatures) { crVoxCache.clear(); await refreshCreatures(); }
        else await refreshLibrary();
        renderAllPanels();
      },
    }, '↻'));
  sideEl.append(hdr);
  if (!libOpen) return;

  /* --- where the parts come from ---
     Two shelves, one set of verbs. Saved parts are the curated ones; the other
     creatures are every arm you have ever drawn, with no curation step — which
     is the one you want when the question is "what would HER arm look like on
     him?" rather than "fetch the arm I put aside". */
  const srcRow = el('div', { class: 'rigbtns' });
  const srcBtn = (id, label, title) => el('button', {
    class: 'small' + (libSource === id ? ' on' : ''), title,
    onclick: () => {
      if (libSource === id) return;
      libSource = id;
      // First visit to the creature shelf scans lazily: the sidecars are only
      // worth reading if you actually open it.
      if (id === 'creatures' && crParts === null && !crErr)
        refreshCreatures().then(renderAllPanels);
      renderAllPanels();
    },
  }, label);
  srcRow.append(
    srcBtn('saved', 'saved parts', 'parts you have saved to assets/limbs/'),
    srcBtn('creatures', 'other creatures',
           'limbs lifted straight out of the other mob files — nothing to ' +
           'save first'));
  sideEl.append(srcRow);

  if (onCreatures) return renderCreatureShelf();

  /* --- save the selected limb --- */
  const sel = selectedPart ? limbByName(selectedPart) : null;
  const kin = sel ? LIB.subtreeNames(limbs(), sel.name) : [];
  const saveRow = el('div', { class: 'rigbtns' });
  saveRow.append(el('button', {
    class: 'small primary', disabled: !sel,
    title: sel
      ? `save "${sel.name}"${kin.length > 1 ? ` and its ${kin.length - 1} ` +
          'descendant' + (kin.length > 2 ? 's' : '') : ''} to assets/limbs/`
      : 'select a limb in the list above first',
    onclick: () => promptSaveLimb(sel, false),
  }, kin.length > 1 ? `save ${sel.name} + ${kin.length - 1}` :
                      (sel ? `save ${sel.name}` : 'save limb')));
  // Offered only when it differs from the group save, so the common case is
  // one obvious button rather than two that look the same.
  if (kin.length > 1)
    saveRow.append(el('button', {
      class: 'small', title: `save ONLY "${sel.name}", without its descendants`,
      onclick: () => promptSaveLimb(sel, true),
    }, 'this limb only'));
  sideEl.append(saveRow);

  /* --- the shelf --- */
  if (libErr) return sideEl.append(el('div', { class: 'rignote' }, libErr));
  if (libParts === null)
    return sideEl.append(el('div', { class: 'rignote' }, 'scanning…'));
  if (!libParts.length)
    return sideEl.append(el('div', { class: 'rignote' },
      'No saved limbs yet. Select a limb above and save it — then it can be ' +
      'dropped onto any other creature, joint and all.'));

  const listEl = el('div', { class: 'riglist' });
  if (libParts.length > 6) {
    const box = el('input', { class: 'cell riglibsearch', value: libFilter,
                              placeholder: 'filter by name, tag or creature' });
    box.addEventListener('input', () => {
      libFilter = box.value;
      // Re-render only the shelf; a full renderAllPanels() would rebuild the
      // input and blur it on every keystroke.
      renderLibraryList(listEl);
    });
    sideEl.append(el('div', { class: 'rigf' }, box));
  }
  sideEl.append(listEl);
  renderLibraryList(listEl);
}

/* ---- the other-creature shelf -------------------------------------------
 *
 * Same two verbs as the saved shelf (swap / add) over a different catalogue,
 * so nothing new has to be learned. What it adds is the TAG FILTER, on by
 * default: the question this shelf answers is "show me the other arms", and a
 * flat list of every limb of every creature — fifteen per mob, five mobs — is
 * not that list. The filter is a checkbox rather than a hidden rule, because a
 * limb with no tag, or one you want to graft somewhere unlike it, still has to
 * be reachable.
 */
function renderCreatureShelf() {
  if (crErr) return sideEl.append(el('div', { class: 'rignote' }, crErr));
  if (crParts === null)
    return sideEl.append(el('div', { class: 'rignote' }, 'scanning creatures…'));

  const sel = selectedPart ? limbByName(selectedPart) : null;
  const selTag = (sel && sel.tag) || '';

  if (!sel)
    sideEl.append(el('div', { class: 'rignote' },
      'Select a limb in the list above — the shelf then offers the same kind ' +
      'of limb from every other creature, and swapping one in lands its joint ' +
      'on the one it replaces.'));

  const opts = el('div', { class: 'rigf' });
  if (selTag) {
    const cb = el('input', { type: 'checkbox' });
    cb.checked = crSameTagOnly;
    cb.addEventListener('change', () => {
      crSameTagOnly = cb.checked; renderAllPanels();
    });
    opts.append(el('span', {}, 'only "' + selTag + '"'), cb,
      el('i', {}, 'limbs tagged the same as the one you have selected'));
  }
  const box = el('input', { class: 'cell riglibsearch', value: crFilter,
                            placeholder: 'filter by creature, limb or tag' });
  const listEl = el('div', { class: 'riglist' });
  box.addEventListener('input', () => {
    crFilter = box.value;
    // Shelf only: a full renderAllPanels() rebuilds this input and blurs it on
    // every keystroke (the same trap the saved shelf's filter documents).
    renderCreatureList(listEl, sel, selTag);
  });
  if (selTag) sideEl.append(opts);
  sideEl.append(el('div', { class: 'rigf' }, box), listEl);
  renderCreatureList(listEl, sel, selTag);
}

function renderCreatureList(host, sel, selTag) {
  host.innerHTML = '';
  const here = ed.getDocPath();
  const q = crFilter.trim().toLowerCase();
  const tagFilter = crSameTagOnly && selTag;
  const shown = (crParts || [])
    .filter(p => !tagFilter || p.tag === selTag)
    .filter(p => !q || p.from.toLowerCase().includes(q) ||
                 p.root.toLowerCase().includes(q) ||
                 p.limbs.some(l => (l.tag || '').toLowerCase().includes(q)));

  if (!shown.length)
    return host.append(el('div', { class: 'rignote' },
      tagFilter ? `no other creature has a limb tagged "${selTag}" — ` +
                  'untick the filter to see every limb'
                : 'nothing matches'));

  let lastFrom = null;
  for (const p of shown) {
    const self = p.path === here;
    // One heading per creature: the list is "whose arm", and repeating the
    // creature name on fifteen consecutive rows would crowd out the limb name.
    if (p.from !== lastFrom) {
      lastFrom = p.from;
      host.append(el('div', { class: 'riggroup', style: 'cursor:default' },
        el('span', { class: 'rigglabel' },
           self ? p.from + ' — as last saved' : p.from)));
    }
    const tags = [...new Set(p.limbs.map(l => l.tag).filter(Boolean))];
    const meta = (p.limbs.length > 1 ? `${p.limbs.length} limbs` : '1 limb') +
      (tags.length ? ' · ' + tags.join(' ') : '') +
      // Say it on the row too: the heading scrolls away, and "swap in the disk
      // copy" and "swap in another creature's" are different enough edits that
      // guessing from position would be a mistake worth making loudly.
      (self ? ' · from the saved file' : '');
    host.append(el('div', { class: 'rigrow riglib' },
      el('div', { class: 'riglibtext' },
        el('span', { class: 'riglibname' }, p.root),
        el('span', { class: 'riglibmeta', title: meta + '\nfrom ' + p.path },
           meta)),
      el('button', {
        class: 'small', disabled: !sel,
        title: sel ? `replace "${sel.name}" (and its descendants) with ` +
                     `${p.from}'s ${p.root} — the incoming joint lands on the ` +
                     'old one\'s, so the skeleton keeps working'
                   : 'select the limb to replace, above',
        onclick: () => loadLimbFromCreature(p, sel.name),
      }, 'swap'),
      el('button', {
        class: 'small', disabled: !sel,
        title: sel ? `attach ${p.from}'s ${p.root} as a new child of ` +
                     `"${sel.name}"`
                   : 'select the parent limb, above',
        onclick: () => loadLimbFromCreature(p, null),
      }, 'add')));
  }
}

function renderLibraryList(host) {
  host.innerHTML = '';
  const q = libFilter.trim().toLowerCase();
  const shown = (libParts || []).filter(p => !q ||
    p.name.toLowerCase().includes(q) || p.from.toLowerCase().includes(q) ||
    p.limbs.some(l => (l.tag || '').toLowerCase().includes(q)));
  if (!shown.length)
    return host.append(el('div', { class: 'rignote' }, 'nothing matches'));

  const sel = selectedPart ? limbByName(selectedPart) : null;
  for (const p of shown) {
    const tags = [...new Set(p.limbs.map(l => l.tag).filter(Boolean))];
    // Shown: what distinguishes two saved arms at a glance — how many limbs,
    // what they are, how big. The source creature goes in the tooltip only:
    // it is the field you want when you have forgotten where a part came
    // from, not when you are picking between two, and including it pushed the
    // BOX SIZE off the end of the line.
    const meta = (p.limbs.length > 1 ? `${p.limbs.length} limbs` : '1 limb') +
      (tags.length ? ' · ' + tags.join(' ') : '') +
      (p.dim ? ` · ${p.dim[0]}×${p.dim[1]}×${p.dim[2]}` : '');
    const row = el('div', { class: 'rigrow riglib' },
      el('div', { class: 'riglibtext' },
        el('span', { class: 'riglibname' }, p.name),
        el('span', { class: 'riglibmeta',
                     title: meta + (p.from ? '\nfrom ' + p.from : '') },
           meta)),
      // SWAP replaces the selected limb (and its subtree); ADD hangs the part
      // off it as a new child. Two verbs because they are genuinely different
      // edits, and guessing between them by context would be worse.
      el('button', {
        class: 'small', disabled: !sel,
        title: sel ? `replace "${sel.name}" (and its descendants) with ${p.name}` +
                     ' — the new part\'s joint lands on the old one\'s'
                   : 'select the limb to replace, above',
        onclick: () => loadLimbFromLibrary(p.name, sel.name),
      }, 'swap'),
      el('button', {
        class: 'small', disabled: !sel,
        title: sel ? `attach ${p.name} as a new child of "${sel.name}"`
                   : 'select the parent limb, above',
        onclick: () => loadLimbFromLibrary(p.name, null),
      }, 'add'),
      el('button', {
        class: 'icon danger', title: 'delete this saved part',
        onclick: async () => {
          if (!confirm(`Delete the saved part "${p.name}"?\n\n` +
                       'This removes it from the library only; creatures ' +
                       'already wearing it keep their copy.')) return;
          await deleteLibraryPart(p.name);
        },
      }, '✕'));
    host.append(row);
  }
}

/** Ask for a name, defaulting to the limb's own, and save. */
function promptSaveLimb(limb, single) {
  const suggest = limb.name.replace(/[^A-Za-z0-9._-]/g, '_');
  const name = prompt(
    single ? `Save "${limb.name}" alone to the limb library as:`
           : `Save "${limb.name}" and its descendants to the limb library as:`,
    suggest);
  if (name === null) return;
  const n = name.trim();
  if (!LIB.nameOk(n))
    return toast('name must be letters, digits, . _ - (max 64)', true);
  const clash = (libParts || []).some(p => p.name === n);
  if (clash && !confirm(`"${n}" already exists in the library. Overwrite it?`))
    return;
  saveLimbToLibrary(limb.name, n, single);
}

/** Remove a saved part. The server moves it to assets/limbs/.trash/ rather
 *  than unlinking, so a mis-click costs a file-manager trip, not the art. */
async function deleteLibraryPart(name) {
  try {
    const r = await fetch('/api/limb/delete',
      { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name }) });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || !j.ok) throw new Error(j.error || 'HTTP ' + r.status);
    toast(`${name} moved to limbs/.trash/`);
    await refreshLibrary();
  } catch (e) {
    toast('delete failed: ' + (e.message || e), true);
  }
  renderAllPanels();
}

function renderRigTail() {
  renderLibrary();

  /* ---- sockets: where a held ITEM attaches ---- */
  //
  // Visible and draggable because the alternative is authoring a grip by
  // trial and error against a running game, which is exactly how the sword
  // ended up parked at the character's feet: the socket is the centre of the
  // fist, the item's own offset is measured from it, and nothing in the
  // pipeline showed either one until it was already wrong on screen.
  sideEl.append(
    el('div', { class: 'righdr' }, 'Item sockets',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small',
        title: selectedPart
          ? 'add a socket on the selected limb'
          : 'select a limb first',
        onclick: () => addSocketFromSelection(),
      }, '+ socket')));

  const SK = sockets();
  if (!SK.length) {
    sideEl.append(el('div', { class: 'rignote' },
      'No sockets. A rig that publishes one (conventionally "held_right" on ' +
      'the hand) can hold any item that declares a grip for that context; ' +
      'without one, EquipItem refuses and nothing appears in the hand.'));
  }
  SK.forEach((s, i) => {
    const on = selectedSocket === i;
    const a = Array.isArray(s.offset) && s.offset.length === 3
      ? s.offset : [0, 0, 0];
    sideEl.append(el('div', { class: 'rigrow' + (on ? ' on' : '') },
      el('button', {
        class: 'small' + (on ? ' on' : ''),
        title: 'drag this socket in the viewport',
        onclick: () => {
          selectedSocket = on ? null : i;
          // A socket lives on its part, so selecting one also selects that
          // limb — otherwise the gizmo would sit in space with no context.
          if (!on && s.part) selectedPart = s.part;
          renderAllPanels();
        },
      }, on ? '◉' : '○'),
      el('span', { style: 'flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis' },
        (s.name || '—') + '  on ' + (s.part || '—')),
      el('span', { class: 'rigdim' },
        a.map(v => (+v).toFixed(1)).join(', ')),
      el('button', {
        class: 'icon danger', title: 'remove socket',
        onclick: () => {
          sockets().splice(i, 1);
          if (selectedSocket === i) selectedSocket = null;
          else if (selectedSocket > i) selectedSocket--;
          touched(); renderAllPanels();
        },
      }, '✕')));

    if (!on) return;
    // Name + part are editable inline: "which context does this rig serve"
    // and "which limb carries it" are the two things a rig actually decides.
    sideEl.append(el('div', { class: 'rigf' },
      el('span', {}, 'context'),
      (() => {
        const inp = el('input', { type: 'text', value: s.name || '' });
        inp.addEventListener('change', () => {
          s.name = inp.value.trim();
          touched(); renderAllPanels();
        });
        return inp;
      })()));
    sideEl.append(el('div', { class: 'rigf' },
      el('span', {}, 'part'),
      (() => {
        const sel = el('select');
        limbs().forEach(l => {
          const o = el('option', { value: l.name }, l.name);
          if (l.name === s.part) o.selected = true;
          sel.append(o);
        });
        sel.addEventListener('change', () => {
          s.part = sel.value;
          selectedPart = sel.value;
          touched(); renderAllPanels();
        });
        return sel;
      })()));
    sideEl.append(el('div', { class: 'rignote' },
      'Drag the gizmo to move the socket, or type the offset below. This is ' +
      'the point the fist closes on, in the same prefab-local voxels as a ' +
      'joint anchor.'));
    ['x', 'y', 'z'].forEach((ax, k) => {
      sideEl.append(el('div', { class: 'rigf' },
        el('span', {}, 'offset ' + ax),
        (() => {
          const inp = el('input', { type: 'number', step: '0.5', value: String(a[k]) });
          inp.addEventListener('change', () => {
            const v = Array.isArray(s.offset) && s.offset.length === 3
              ? s.offset.slice() : [0, 0, 0];
            v[k] = num(inp.value, 0);
            s.offset = v;
            touched(); bindGizmo(); ed.invalidate();
          });
          return inp;
        })()));
    });
    renderHeldItem();
  });

  renderNaturalWeapons();

  /* ---- chains (read-only summary; authored by Split-to-model + this) ---- */
  sideEl.append(
    el('div', { class: 'righdr' }, 'IK chains',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small',
        title: 'add a two-bone chain from the selected limb and its parent',
        onclick: () => addChainFromSelection(),
      }, '+ chain')));
  const CH = chains();
  if (!CH.length) {
    sideEl.append(el('div', { class: 'rignote' },
      'No chains. A leg needs a two-bone chain (upper, lower) for the gait to ' +
      'place its foot; without one the limb falls back to swingAmp.'));
  }
  CH.forEach((c, i) => {
    const parts = (c.parts || []).join(' → ');
    sideEl.append(el('div', { class: 'rigrow' },
      el('span', { class: 'rigdim' }, c.tag || '—'),
      el('span', { style: 'flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis' },
        parts + (c.effector ? '  ⇒ ' + c.effector : '')),
      el('button', {
        class: 'icon danger', title: 'remove chain',
        onclick: () => { chains().splice(i, 1); touched(); renderAllPanels(); },
      }, '✕')));
  });

  /* ---- gait ---- */
  sideEl.append(
    el('div', { class: 'righdr' }, 'Gait preview',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small' + (gaitOn ? ' on' : ''),
        onclick: () => { setGait(!gaitOn); renderAllPanels(); },
      }, gaitOn ? 'stop [K]' : 'walk [K]'),
      // AUTO-LOCO is what makes the preview match the game, and it is a toggle
      // rather than a hardcoded behaviour for one reason: while AUTHORING a
      // clip you want to see THAT clip, not whichever one the family picked.
      // On for the walk preview (where "does this match" is the question), off
      // the moment the clip lane is driving.
      el('button', {
        class: 'small' + (autoLoco ? ' on' : ''),
        title: 'play the locomotion CLIP FAMILY while walking (idle / walk / ' +
          'run, exclusive, stride-rate locked) exactly as avatar.cpp does. ' +
          'This is the arm swing: the gait layer only places FEET. Off pins ' +
          'the clip you have open in the clip lane instead.',
        onclick: () => { autoLoco = !autoLoco; ed.invalidate(); renderAllPanels(); },
      }, 'auto-loco')));

  const g = gait();
  const hasGait = !!(ed.getSidecar()?.gait);
  if (!hasGait) {
    sideEl.append(el('div', { class: 'rignote' },
      'No gait block — the preview falls back to the legacy swingAmp/swingPhase ' +
      'sine, exactly as the engine does (mob.cpp:784). Add one to drive the ' +
      'foot-planting runtime.'),
      el('div', { class: 'rigbtns' }, el('button', {
        class: 'small',
        onclick: () => { gait(); touched(); renderAllPanels(); },
      }, '+ gait block')));
  }

  sideEl.append(el('div', { class: 'rigf' },
    el('span', {}, 'preview speed'),
    (() => {
      const r = el('input', { type: 'range', min: '0', max: '2', step: '0.05' });
      r.value = String(gaitSpeedScale);
      r.addEventListener('input', () => { gaitSpeedScale = +r.value; renderGaitReadout(); });
      return r;
    })(),
    el('i', { id: 'gaitReadout' }, '')));

  if (hasGait) {
    // Full param set, in the engine's own grouping. Inline docs are lifted
    // from anim.h's comments so the editor and the header cannot drift.
    const G = [
      ['cadence', '2.2', 'stride frequency multiplier'],
      ['strideBias', '0.35', 'forward foot lead, in LEG LENGTHS'],
      ['leadTime', '0.2', 'seconds of velocity lookahead'],
      ['stepThreshold', '0.6', 'drift (leg lengths) that unplants a foot'],
      ['stepDuration', '0.22', 'seconds of swing'],
      ['stepHeight', '0.25', 'arc peak, in leg lengths'],
      ['rideHeight', '0.9', 'body above the foot plane, in leg lengths'],
      ['bobAmp', '0.06', 'pelvis bob amplitude'],
      ['bobFreqMul', '2.0', 'bob runs at Nx step frequency (2 = per footfall)'],
      ['swayAmp', '0.05', 'lateral sway at 1x stride'],
      ['rollAmp', '0.09', 'body roll at 1x stride'],
      ['spineCounter', '0.7', 'chest counter-rotation vs hips (needs tag "spine")'],
      ['phaseLag', '0.05', 'seconds of lag per hierarchy level'],
    ];
    for (const [k, ph, hint] of G)
      sideEl.append(field(k, numInput(g, k, { ph, after: () => ed.invalidate() }), hint));

    /* ---- leg groups: the gait state machine ---- */
    sideEl.append(el('div', { class: 'righdr' }, 'Leg groups',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small',
        onclick: () => {
          if (!Array.isArray(g.groups)) g.groups = [];
          g.groups.push([]);
          touched(); renderAllPanels();
        },
      }, '+ group')));
    sideEl.append(el('div', { class: 'rignote' },
      'Exactly ONE group may swing at a time — that single rule IS the gait ' +
      'state machine (anim.h:104). Two singleton groups = a biped alternating; ' +
      'diagonal pairs = a quadruped trot.'));
    (g.groups || []).forEach((grp, gi) => {
      const row = el('div', { class: 'rigrow' },
        el('span', { class: 'rigdim' }, 'grp ' + gi));
      const sel = el('input', {
        class: 'cell id', style: 'flex:1',
        value: (grp || []).join(', '),
        placeholder: 'legU.FL, legU.BR',
      });
      sel.addEventListener('change', () => {
        g.groups[gi] = sel.value.split(',').map(s => s.trim()).filter(Boolean);
        touched();
      });
      row.append(sel, el('button', {
        class: 'icon danger',
        onclick: () => { g.groups.splice(gi, 1); touched(); renderAllPanels(); },
      }, '✕'));
      sideEl.append(row);
    });
  }

  // Live gait readout, so the author can see the state machine working.
  sideEl.append(el('div', { class: 'rignote', id: 'gaitState' }, ''));
  renderGaitReadout();
}

/* ==========================================================================
   NATURAL WEAPONS — the parts of this rig that ARE weapons.

   THE ANATOMY ANSWER TO THE ITEM SOCKET ABOVE, and it sits beside it for that
   reason: a socket says where a thing the creature was HANDED attaches, and
   this says which parts of it are already weapons. A fist and a set of jaws
   are exactly what an item is — AN EDGE SEGMENT AND A StrikeProfile
   (game/impact.h) — with the segment living on a part of the rig instead of on
   a borrowed slot. mob.h MobNaturalWeaponDef says why it is not just an
   invisible held item: the slot would outlive the part, so a headless zombie
   could still bite.

   WHAT AN AUTHOR IS ACTUALLY DOING HERE is drawing a line through a fist. The
   numbers are the same voxels the rest of this panel speaks, and the segment
   is drawn in the viewport while its row is open — `from: [4, 9, 4]` means
   nothing typed against a text field and everything typed against a picture.
   ========================================================================== */

// Which natural-weapon row is open, or -1. Panel state, not document state.
let selectedNatural = -1;

function renderNaturalWeapons() {
  const NW = naturalList();
  sideEl.append(
    el('div', { class: 'righdr' }, 'Natural weapons',
      el('span', { class: 'spacer' }),
      el('button', {
        class: 'small',
        title: selectedPart
          ? 'make the selected limb a weapon'
          : 'select a limb first — a natural weapon lives ON a part',
        onclick: () => addNaturalFromSelection(),
      }, '+ weapon')));

  if (!NW.length) {
    sideEl.append(el('div', { class: 'rignote' },
      'None. A rig with no `natural` block can only fight with something in ' +
      'its fist: every unarmed style names a weapon by name ("fist.R", ' +
      '"jaws"), and StyleUsable refuses a style whose weapon this creature ' +
      'has not got — so a disarmed creature with none of these simply never ' +
      'attacks. Select a hand and press "+ weapon".'));
  }

  NW.forEach((nw, i) => {
    const on = selectedNatural === i;
    const pi = skel ? skel.findPart(nw.part || '') : -1;
    const mode = pi < 0 ? '—' : (chainForEffector(pi, {}) >= 0 ? 'chain' : 'aim');
    sideEl.append(el('div', { class: 'rigrow' + (on ? ' on' : '') },
      el('button', {
        class: 'small' + (on ? ' on' : ''),
        title: 'edit this weapon and draw its edge in the viewport',
        onclick: () => {
          selectedNatural = on ? -1 : i;
          if (!on && nw.part) selectedPart = nw.part;
          renderAllPanels();
          ed.invalidate();
        },
      }, on ? '◉' : '○'),
      el('span', { style: 'flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis' },
        (nw.name || '—') + '  on ' + (nw.part || '—')),
      el('span', { class: 'rigdim', title: mode === 'aim'
        ? 'no IK chain serves this part, so the driver AIMS it: the part and a '
          + 'share of the spine rotate until its forward lies along the stroke'
        : mode === 'chain'
          ? 'an IK chain serves this part, so the driver solves the arm to it — '
            + 'the same solve a sword gets, with no wrist steering'
          : 'this part is not a limb of this rig' }, mode),
      el('button', {
        class: 'icon danger', title: 'remove this natural weapon',
        onclick: () => {
          naturalList().splice(i, 1);
          if (selectedNatural === i) selectedNatural = -1;
          else if (selectedNatural > i) selectedNatural--;
          touched(); renderAllPanels(); ed.invalidate();
        },
      }, '✕')));
    if (on) sideEl.append(naturalBody(nw));
  });

  // A STILL RIG STILL DRAWS ITS EDGE. stepPreviewFixed refreshes the overlay
  // every tick, but it only runs while something wants a posed rig — and the
  // common case for typing these numbers is a rig standing at rest.
  renderNaturalEdgeOverlay();

  if (skel && NW.some(w => !w || !w.part || skel.findPart(w.part) < 0))
    sideEl.append(el('div', { class: 'rignote atkwarn' },
      'A weapon above names a part this rig has not got — the engine skips it ' +
      'LOUDLY at load, and every style naming it becomes unusable.'));
}

function addNaturalFromSelection() {
  if (!selectedPart) {
    toast('select a limb first (the fist, or the head)', true);
    return;
  }
  const doc = sc();
  if (!Array.isArray(doc.natural)) doc.natural = [];
  // SEEDED FROM THE PART'S OWN BOX, not from zeros. An all-zero edge is
  // degenerate and the engine refuses one at load ("from == to"), so the first
  // thing a new entry would do is not exist. The default runs down the part's
  // own box through its centre, which is roughly a knuckle line and is at
  // least visible.
  const m = ed.getModels().find(x => x.name === selectedPart);
  const d = m ? m.dim : { x: 4, y: 4, z: 4 };
  const o = m ? m.offset : { x: 0, y: 0, z: 0 };
  const cx = o.x + d.x * 0.5, cz = o.z + d.z * 0.5;
  let name = selectedPart;
  for (let k = 2; naturalList().some(w => w && w.name === name); k++)
    name = selectedPart + '_' + k;
  doc.natural.push({
    name, part: selectedPart,
    edge: { from: [cx, o.y + d.y, cz], to: [cx, o.y, o.z + d.z],
            halfWidth: Math.max(1, Math.round(Math.min(d.x, d.z) * 0.5)) },
    strike: { blunt: 4.0 },
  });
  selectedNatural = naturalList().length - 1;
  touched(); renderAllPanels(); ed.invalidate();
}

/** One natural weapon's editor, rendered inline under its row. */
function naturalBody(nw) {
  const body = el('div', { class: 'rigbody' });

  const nameIn = el('input', { class: 'cell id', value: nw.name || '' });
  nameIn.addEventListener('change', () => {
    nw.name = nameIn.value.trim();
    touched(); renderAllPanels();
  });
  body.append(field('name', nameIn,
    'what a style\'s `weapon` names — "fist.R", "jaws"'));

  const partSel = el('select', { class: 'cell' });
  partSel.append(el('option', { value: '' }, '(none)'));
  for (const l of limbs()) partSel.append(el('option', { value: l.name }, l.name));
  partSel.value = nw.part || '';
  partSel.addEventListener('change', () => {
    nw.part = partSel.value;
    if (nw.part) selectedPart = nw.part;
    touched(); renderAllPanels(); ed.invalidate();
  });
  body.append(field('part', partSel,
    'the rig part that IS the weapon; severing it makes every style naming ' +
    'this weapon unusable'));

  if (!nw.edge || typeof nw.edge !== 'object') nw.edge = {};
  const e = nw.edge;
  body.append(el('div', { class: 'rignote' },
    'THE EDGE IS TWO POINTS IN THE PART\'S OWN ART FRAME — the same Y-up, ' +
    'origin-at-the-MODEL\'S-MIN-CORNER voxels this file is drawn in; not ' +
    'world voxels, and not an offset along an axis (a part has no hilt to ' +
    'measure one from). A fist\'s runs from the wrist to the knuckles, a ' +
    'jaw\'s from the throat to the teeth. The engine scales them by ' +
    'artVoxelsPerMetre at load, exactly as it scales a limb\'s own edge. ' +
    'from == to is refused at load: there would be no segment to sweep.'));
  const redraw = () => { touched(); ed.invalidate(); renderNaturalEdgeOverlay(); };
  body.append(field('from', vecInput(e, 'from', [0, 0, 0], redraw),
    'the base of the segment (the wrist)'));
  body.append(field('to', vecInput(e, 'to', [0, 0, 0], redraw),
    'its far end (the knuckles, the teeth)'));
  body.append(field('halfWidth',
    numInput(e, 'halfWidth', { ph: '2.5', after: redraw }),
    'carve radius about the segment, in the same voxels'));

  // ---- the StrikeProfile (game/impact.h) --------------------------------
  if (!nw.strike || typeof nw.strike !== 'object') nw.strike = {};
  const st = nw.strike;
  body.append(el('div', { class: 'rignote' },
    'WHAT IT DOES WHEN IT LANDS. A strike is three numbers and not a KIND: hp ' +
    'at full swing speed arriving as a kerf (cut), as trauma (blunt) and as a ' +
    'tear (bite), all three resolved by one sweep against whatever it hit. A ' +
    'bare fist is blunt only with bluntCarve 0 — which is what makes a punch ' +
    'bruise without ever taking an arm off. A worn piece carrying its own ' +
    '`strike` block REPLACES this profile when the fist under it swings (the ' +
    'gauntlet override), and the creature\'s own `bite` block is what adds ' +
    'the rot.'));
  for (const [k, ph, hint] of [
    ['cut', '0', 'hp arriving as a KERF — the blade wound model'],
    ['blunt', '4', 'hp arriving as TRAUMA: a bruise, next to no bleeding, and ' +
                   'never a sever however many land'],
    ['bluntCarve', '0', '0..1 of gore.bluntCarveRadius dented out of FLESH at ' +
                        'full power. 0 for a bare fist; a gauntlet is ~0.35'],
    ['armorBreak', '0', '0..1 of gear.bluntDentRadius broken out of a WORN ' +
                        'SHELL at full power. 0 for a fist, 0.8 for a mace'],
    ['bite', '0', 'hp arriving as a TEAR: a rot-blob carve that bleeds like a ' +
                  'cut and severs only by collapse'],
  ])
    body.append(field(k, numInput(st, k, { ph, after: () => touched() }), hint));

  return body;
}

/**
 * Draw the open natural weapon's edge in the viewport, on the LIVE pose, so
 * the segment moves with the part as the rig walks or swings — the same
 * derivation the damage sweep uses (naturalEdgeModel), which is what stops the
 * picture and the hitbox drifting apart.
 */
function renderNaturalEdgeOverlay() {
  const NW = naturalList();
  const nw = selectedNatural >= 0 ? NW[selectedNatural] : null;
  const seg = nw ? naturalEdgeModel(nw) : null;
  if (!seg) { ed.setNaturalEdges?.(null); return; }
  const lo = lungeOffsetModel();
  const a = AN.vadd(seg.base, lo), b = AN.vadd(seg.tip, lo);
  const segs = [{ a: [a.x, a.y, a.z], b: [b.x, b.y, b.z], color: 0x7cf03a }];
  // A CROSS AT THE TIP, sized by halfWidth — the carve radius is half of what
  // the edge actually is, and a bare line hides it entirely.
  const hw = seg.halfWidth;
  if (hw > 0.01) {
    const d = AN.vsub(b, a);
    const len = AN.vlen(d) || 1;
    const dir = AN.vmul(d, 1 / len);
    const cand = [AN.v3(1, 0, 0), AN.v3(0, 1, 0), AN.v3(0, 0, 1)];
    let best = 0, bd = 2;
    for (let i = 0; i < 3; i++) {
      const dot = Math.abs(AN.vdot(dir, cand[i]));
      if (dot < bd) { bd = dot; best = i; }
    }
    const u = AN.vnorm(AN.vcross(dir, cand[best]));
    const v = AN.vnorm(AN.vcross(u, dir));
    for (const ax of [u, v]) {
      const p0 = AN.vsub(b, AN.vmul(ax, hw)), p1 = AN.vadd(b, AN.vmul(ax, hw));
      segs.push({ a: [p0.x, p0.y, p0.z], b: [p1.x, p1.y, p1.z],
                  color: 0x3a8f5f, alpha: 0.8 });
    }
  }
  ed.setNaturalEdges?.(segs);
}

// Build a two-bone chain from the selected limb + its parent, which is the
// shape the engine requires (>=2 parts + effector, mob.cpp:311).
function addChainFromSelection() {
  const limb = selectedPart ? limbByName(selectedPart) : null;
  if (!limb) { toast('select a limb first (the LOWER bone)', true); return; }
  if (!limb.parent) { toast(`"${limb.name}" has no parent to pair with`, true); return; }
  const existing = chains().find(c => (c.parts || []).includes(limb.name));
  if (existing) { toast('that limb is already in a chain', true); return; }
  chains().push({
    tag: limb.tag || 'leg',
    parts: [limb.parent, limb.name],
    effector: limb.name,
    pole: [0, 0, 1],
    solver: 'twobone',
  });
  touched();
  toast(`chain ${limb.parent} → ${limb.name}`);
  renderAllPanels();
}

/* --------------------------------------------------------------------------
   Held-item preview UI: pick an item, see it in the fist, tune the angle.

   This is the reason the socket panel exists at all. The rig only decides
   WHERE the fist closes; the ANGLE a sword sits at is the item's own
   grip.rotation, so the fields below write assets/items/<id>.json and leave
   the rig sidecar alone. Two files, edited from one place, because they are
   two halves of one visual question.
   -------------------------------------------------------------------------- */

const RPY_LABELS = ['roll (X)', 'pitch (Y)', 'yaw (Z)'];

function renderHeldItem() {
  sideEl.append(el('div', { class: 'righdr' }, 'Held item preview'));

  if (itemErr && !itemList) {
    sideEl.append(el('div', { class: 'rignote' }, itemErr));
    return;
  }
  if (!itemList) {
    // First paint: kick the fetch and re-render when it lands.
    sideEl.append(el('div', { class: 'rignote' }, 'loading items…'));
    ensureItemList().then(() => renderAllPanels());
    return;
  }

  // --- which item ---
  sideEl.append(el('div', { class: 'rigf' },
    el('span', {}, 'item'),
    (() => {
      const sel = el('select');
      sel.append(el('option', { value: '' }, '(none)'));
      for (const it of itemList) {
        const o = el('option', { value: it.id }, it.name || it.id);
        if (it.id === heldItemId) o.selected = true;
        sel.append(o);
      }
      sel.addEventListener('change', () => {
        loadHeldItem(sel.value || null);
      });
      return sel;
    })(),
    // The same one-click arm the Attacks lane offers, here because this is
    // where you look for it. Picking the socket is the step people miss.
    !heldItemId ? el('button', {
      class: 'small primary', title: 'put a sword in this rig\'s hand',
      onclick: () => equipWeapon(),
    }, '+ sword') : null));

  if (!heldItemId) {
    sideEl.append(el('div', { class: 'rignote' },
      'Pick an item to hang it off this socket. It is drawn exactly where the ' +
      'engine would put it — socket × grip, hilt box centred on the socket ' +
      '(avatar.cpp EquipItem) — so what you see here is what the game does.'));
    return;
  }
  if (itemErr) {
    sideEl.append(el('div', { class: 'rignote' }, 'could not load: ' + itemErr));
    return;
  }
  if (!itemSc) { sideEl.append(el('div', { class: 'rignote' }, 'loading…')); return; }

  // --- which context ---
  const ctxs = Object.keys(itemSc.grip || {});
  if (!ctxs.length) {
    sideEl.append(el('div', { class: 'rignote' },
      'This item declares no grip contexts, so the engine refuses to equip it ' +
      'at all. Add one below to author a pose for "' + (activeSocket()?.name || 'this socket') + '".'),
      el('div', { class: 'rigbtns' }, el('button', {
        class: 'small',
        onclick: () => {
          gripCtx = activeSocket()?.name || 'held_right';
          activeGrip();                 // creates it
          itemDirty = true;
          bindGizmo(); ed.invalidate(); renderAllPanels();
        },
      }, '+ grip for "' + (activeSocket()?.name || 'held_right') + '"')));
    return;
  }
  sideEl.append(el('div', { class: 'rigf' },
    el('span', {}, 'context'),
    (() => {
      const sel = el('select');
      for (const c of ctxs) {
        const o = el('option', { value: c }, c);
        if (c === gripCtx) o.selected = true;
        sel.append(o);
      }
      sel.addEventListener('change', () => {
        gripCtx = sel.value;
        bindGizmo(); ed.invalidate(); renderAllPanels();
      });
      return sel;
    })()));

  const sockName = activeSocket()?.name || '';
  if (sockName && gripCtx !== sockName) {
    // Not an error — an author may be previewing a "ground" pose in the hand —
    // but the engine matches socket name to grip key exactly, so a mismatch is
    // worth saying out loud rather than letting it read as a broken preview.
    sideEl.append(el('div', { class: 'rignote' },
      `Previewing "${gripCtx}" on a socket named "${sockName}". The engine ` +
      `looks the grip up BY THE SOCKET'S NAME, so in game this socket uses ` +
      `"${sockName}" — which this item ` +
      (ctxs.includes(sockName) ? 'does declare.' : 'does NOT declare, so it cannot be held here.')));
  }

  sideEl.append(el('div', { class: 'rignote' },
    'Drag the coloured rings to turn the item, or type the angles. Euler ' +
    'degrees applied X then Y then Z, written to assets/items/' + heldItemId +
    '.json — the RIG is not modified.'));
  sideEl.append(el('div', { id: 'gripFields' }));
  renderGripFields();
}

/**
 * The roll/pitch/yaw + translation rows. Split from renderHeldItem so a ring
 * drag can refresh just the numbers without rebuilding the whole panel (which
 * would drop focus out of whichever field is being typed into).
 */
function renderGripFields() {
  // The host is created by renderHeldItem() during a full panel render, which
  // is what puts it in the right place in the socket section. A ring drag
  // calls this directly to refresh only the numbers — if the panel is not
  // currently showing the grip (no socket selected, another tab), there is
  // nothing to update and appending a stray host to the end of the sidebar
  // would be worse than doing nothing.
  const host = document.getElementById('gripFields');
  if (!host) return;
  host.innerHTML = '';
  const g = itemSc ? itemSc.grip?.[gripCtx] : null;
  if (!g) return;
  const grip = activeGrip();

  RPY_LABELS.forEach((label, k) => {
    host.append(el('div', { class: 'rigf' },
      el('span', {}, label),
      (() => {
        const inp = el('input', {
          type: 'number', step: '5', value: String(+grip.rotation[k] || 0),
        });
        inp.addEventListener('change', () => {
          const v = grip.rotation.slice();
          v[k] = num(inp.value, 0);
          grip.rotation = v;
          itemDirty = true;
          bindGizmo(); ed.invalidate(); updateSocketAxes(); renderAllPanels();
        });
        return inp;
      })()));
  });

  const rotBtn = (label, axis, sign) => el('button', {
    class: 'small', title: `${sign > 0 ? '+' : '-'}90° ${['roll','pitch','yaw'][axis]}`,
    onclick: () => {
      const v = grip.rotation.slice();
      v[axis] = ((+v[axis] || 0) + sign * 90) % 360;
      grip.rotation = v;
      itemDirty = true;
      bindGizmo(); ed.invalidate(); updateSocketAxes(); renderAllPanels();
    },
  }, label);
  host.append(el('div', { class: 'rigbtns' },
    rotBtn('+90 roll',  0,  1), rotBtn('-90 roll',  0, -1),
    rotBtn('+90 pitch', 1,  1), rotBtn('-90 pitch', 1, -1),
    rotBtn('+90 yaw',   2,  1), rotBtn('-90 yaw',   2, -1)));

  // Translation is the residual nudge ON TOP of the hilt-box alignment
  // (item.h), so it is zero for a well-authored item — shown because a
  // non-zero value here explains an offset the angles cannot.
  ['x', 'y', 'z'].forEach((ax, k) => {
    host.append(el('div', { class: 'rigf' },
      el('span', {}, 'nudge ' + ax),
      (() => {
        const inp = el('input', {
          type: 'number', step: '1', value: String(+grip.translation[k] || 0),
        });
        inp.addEventListener('change', () => {
          const v = grip.translation.slice();
          v[k] = num(inp.value, 0);
          grip.translation = v;
          itemDirty = true;
          ed.invalidate(); updateSocketAxes(); renderAllPanels();
        });
        return inp;
      })()));
  });

  host.append(el('div', { class: 'rignote' },
    itemSc.hilt
      ? 'Nudge is in MICRO units and is a residual on top of the hilt box, ' +
        'which is already centred on the socket. Zero is the healthy value.'
      : 'This item declares NO hilt box, so placement falls back to nudge ' +
        'alone — the arrangement that shipped the sword-at-the-feet bug. ' +
        'Consider authoring a hilt in its generator.'));

  host.append(el('div', { class: 'rigbtns' },
    el('button', {
      class: 'small' + (itemDirty ? ' on' : ''),
      title: 'write assets/items/' + heldItemId + '.json',
      onclick: () => saveHeldItem(),
    }, itemDirty ? 'save item ●' : 'save item'),
    el('button', {
      class: 'small',
      title: 'back to no rotation',
      onclick: () => {
        grip.rotation = [0, 0, 0];
        itemDirty = true;
        bindGizmo(); ed.invalidate(); updateSocketAxes(); renderAllPanels();
      },
    }, 'reset angles')));
}

/**
 * Add a socket on the selected limb, seeded at the CENTRE of that limb's model
 * box — which is what "where the fist closes" means for a hand, and what
 * gen_mina.py computes for mina's own socket. Seeding it anywhere else (the
 * origin, the joint anchor) would put the first drag's starting point somewhere
 * an author has to correct before they can even see it.
 *
 * The default context is "held_right" because that is the one the engine ships
 * an item for; a second socket is renamed in the panel.
 */
function addSocketFromSelection() {
  const limb = selectedPart ? limbByName(selectedPart) : null;
  if (!limb) { toast('select a limb first (the hand)', true); return; }
  const m = ed.getModels().find(o => o.name === limb.name);
  const c = m
    ? [m.offset.x + m.dim.x / 2, m.offset.y + m.dim.y / 2, m.offset.z + m.dim.z / 2]
    : [0, 0, 0];
  const taken = new Set(sockets().map(s => s.name));
  let name = 'held_right';
  for (let i = 2; taken.has(name); i++) name = 'held_right_' + i;
  sockets().push({
    name,
    part: limb.name,
    offset: c.map(v => Math.round(v * 2) / 2),
    // Identity: the socket frame IS the limb's frame. Which way a held thing
    // POINTS is the item's business (its grip rotation) — putting a rotation
    // here as well would mean two places encode the same fact and they would
    // drift apart. See mob.h's note on MobSocketDef::rotation.
    rotation: [0, 0, 0],
  });
  selectedSocket = sockets().length - 1;
  touched();
  toast(`socket "${name}" on ${limb.name}`);
  renderAllPanels();
}

function renderGaitReadout() {
  const r = document.getElementById('gaitReadout');
  if (r) {
    const s = num(ed.getSidecar()?.speed, 5) * gaitSpeedScale;
    r.textContent = `${gaitSpeedScale.toFixed(2)}× (${s.toFixed(1)} m/s)`;
  }
  const st = document.getElementById('gaitState');
  if (st && skel && anim) {
    if (!gaitOn) { st.textContent = ''; return; }
    const sw = anim.feet.map((f, i) =>
      (f.swinging ? '▲' : f.valid ? '▼' : '·')).join(' ');
    // WHICH CLIP IS DRIVING, and at what rate. Without this line the two ways
    // the preview can be wrong — "the family picked idle because the speed
    // slider is at 0.05" and "this rig has no walk clip at all" — look
    // identical, which is the bare-number failure CLAUDE.md rule 6 is about.
    let clips = '';
    if (anim.clips.length) {
      clips = anim.clips.map(i => {
        const c = skel.clips[i.clip];
        if (!c) return '?';
        const r = Math.abs(i.rate - 1) > 0.01 ? `×${i.rate.toFixed(2)}` : '';
        return `${c.name}${r}${i.stopping ? '↓' : ''}` +
               `@${(i.weight * (i.fade ?? 1)).toFixed(2)}`;
      }).join(' + ');
    } else {
      clips = autoLoco
        ? (skel.clips.length ? 'none — no idle/walk/run clip in this rig'
                             : 'none — this rig has no clips at all')
        : 'none (auto-loco off)';
    }
    st.textContent =
      `phase ${anim.gaitPhase.toFixed(2)}  bodyY ${anim.bodyY.toFixed(2)}  ` +
      `stride ${anim.strideRate.toFixed(2)}/s  feet ${sw}` +
      `   (▲ swinging ▼ planted · gone)\n` +
      `clips: ${clips}`;
    st.style.whiteSpace = 'pre-line';
  }
}

/* ==========================================================================
   4. anchor gizmo binding
   ========================================================================== */

function bindGizmo() {
  // A SELECTED SOCKET OWNS THE GIZMO. Both a socket and a joint anchor are
  // "a point on this part", so they share one drag gizmo rather than putting
  // two overlapping handles in the viewport; the socket wins while selected
  // because the author explicitly asked for it.
  const SK = sockets();
  if (selectedSocket !== null && selectedSocket >= 0 && selectedSocket < SK.length) {
    const s = SK[selectedSocket];
    const a = Array.isArray(s.offset) && s.offset.length === 3
      ? s.offset : [0, 0, 0];
    ed.setGizmo({
      anchor: a.slice(),
      // No swing arc: a socket does not hinge. Passing the limb's joint axis
      // here would draw an arc that means nothing for this handle.
      axis: [0, 1, 0],
      onChange: v => {
        s.offset = v.slice();
        touched();
        ed.invalidate();
        renderAnchorFields(v);
        updateSocketAxes();
      },
    });
    updateSocketAxes();
    // THE RINGS ROTATE THE ITEM'S GRIP, NOT THE SOCKET. A socket's frame is the
    // hand's frame (mob.h: its rotation stays identity); what an author is
    // actually tuning when they turn a sword in the fist is the ITEM's
    // grip.rotation, in the item's own sidecar. So the rings appear only when
    // an item is loaded, and they write there.
    const grip = itemSc && itemSc.grip?.[gripCtx] ? activeGrip() : null;
    if (grip) {
      ed.setRotGizmo({
        quat: eulerToQuat(grip.rotation.map(Number)),
        onChange: (q, done) => {
          // The rings hand back an absolute orientation in the gizmo's frame;
          // the sidecar stores Euler degrees, so convert on the way in. Snapped
          // to whole degrees because that is how the numbers are authored and
          // an unsnapped drag writes 42.7000000001 into the JSON.
          const e = quatToEuler(q).map(v => Math.round(v));
          grip.rotation = e;
          itemDirty = true;
          ed.invalidate();
          updateSocketAxes();
          renderGripFields();
        },
      });
    } else {
      ed.setRotGizmo(null);
    }
    poseEdit = null;
    return;
  }
  // Past this point no socket is selected, so the frame axes have nothing to
  // point at. Clearing here rather than in each branch below means a new
  // early return cannot leave them stranded at the last socket's position.
  ed.setSocketAxes(null);
  ed.setHiltBox?.(null);

  const limb = selectedPart ? limbByName(selectedPart) : null;
  if (!limb) {
    ed.setGizmo(null);
    ed.setRotGizmo(null);
    poseEdit = null;       // a stale edit would keep posing the old part
    return;
  }
  // A limb with no explicit anchor uses the engine's AutoAnchor (mob.cpp:59).
  // Rather than reimplement that heuristic, seed the gizmo at the model's
  // centre so the first drag writes a real value — and say so in the panel.
  let a = limb.anchor;
  if (!Array.isArray(a) || a.length !== 3) {
    const m = ed.getModels().find(o => o.name === limb.name);
    a = m ? [m.offset.x + m.dim.x / 2, m.offset.y + m.dim.y / 2, m.offset.z + m.dim.z / 2]
          : [0, 0, 0];
  }
  ed.setGizmo({
    anchor: a.slice(),
    axis: Array.isArray(limb.axis) && limb.axis.length === 3 ? limb.axis : [1, 0, 0],
    onChange: v => {
      limb.anchor = v.slice();
      touched();
      ed.invalidate();
      // Refresh only the anchor inputs, not the whole panel — a full re-render
      // mid-drag would tear the pointer capture away.
      renderAnchorFields(v);
    },
  });

  // Rotation rings only make sense while a clip is open: outside a clip there
  // is nowhere to put the pose.
  reseedPose();
}

/**
 * (Re)bind the rotation rings to the pose the clip samples at the cursor and
 * drop any in-progress pose edit. Called whenever what the rings should
 * reflect changed: part selection, scrub, key select, play toggle, key write.
 *
 * poseEdit exists only from first ring drag to key write. It used to be
 * seeded permanently at part-select time, which meant applyPoseEdit()
 * overrode the sampled pose forever after — scrubbing and playback visibly
 * did nothing for the selected part.
 */
function reseedPose() {
  poseEdit = null;
  const limb = selectedPart ? limbByName(selectedPart) : null;
  if (!activeClip || !limb) { ed.setRotGizmo(null); return; }
  // Seed from whatever the clip already says at the cursor, so grabbing a
  // ring continues the existing pose instead of snapping to identity.
  const seeded = sampleClipPose(limb.name, clipCursorMs);
  ed.setRotGizmo({
    quat: seeded.rot,
    onChange: (q, done) => {
      if (!poseEdit || poseEdit.part !== limb.name)
        poseEdit = { part: limb.name, rot: q,
                     pos: sampleClipPose(limb.name, clipCursorMs).pos };
      poseEdit.rot = q;
      ed.invalidate();
      if (done && autoKey) writeKey();
    },
  });
}

/**
 * Sample the active clip's track for one part at time t, using the ENGINE's
 * own sampler (anim.js sampleTrack) so the editor and the runtime agree about
 * what a partially-keyed track looks like.
 */
function sampleClipPose(partName, tMs) {
  const c = clipObj();
  const fallback = { rot: AN.qid(), pos: AN.v3() };
  if (!c || !skel) return fallback;
  const ci = skel.clips.findIndex(k => k.name === activeClip);
  if (ci < 0) return fallback;
  const pi = skel.findPart(partName);
  const tr = skel.clips[ci].tracks.find(t => t.part === pi);
  if (!tr) return fallback;
  const s = AN.sampleTrack(tr, tMs);
  return s ? { rot: s.rot, pos: s.pos } : fallback;
}

function renderAnchorFields(v) {
  const rows = sideEl?.querySelectorAll('.rigvec');
  if (!rows || !rows.length) return;
  const inputs = rows[0].querySelectorAll('input');
  [0, 1, 2].forEach(i => { if (inputs[i]) inputs[i].value = v[i]; });
}

/* ==========================================================================
   5. preview runtime (gait v2 + clips)

   All of the MATH lives in anim.js, which is a line-cited transcription of
   src/game/anim.cpp and the gait/procedural layer in mob.cpp. This section
   only owns the driving: build a skeleton from the sidecar, run the same
   stage order the engine runs, and hand the resulting model-space transforms
   back to editor.js for rendering.

   Engine stage order (mob.cpp:2577-2830 UpdateAnimation, avatar.cpp:903 for
   the player's own copy — the two agree stage for stage):
     velocity smoothing -> gaitPhase advance -> LOCOMOTION CLIP SELECTION
     -> AnimSampleAndBlend (1-3)
     -> procedural layer (legacy swing, bob/sway/roll/spine, springs)
     -> AnimApplySpineTwist (3.5)
     -> AnimFlatten (4) -> UpdateGait -> AnimSolveTwoBone per chain (5)
     -> ApplyWeaponArm (5.5) -> AnimClampPoseLimits (6)
   Physics/ragdoll blending is the one omitted stage: it needs Jolt.

   THE LOCOMOTION CLIP SELECTION WAS MISSING UNTIL 2026-09-01, and it is the
   whole of the reported "the tuner's walk is outdated". The gait layer places
   FEET; the arms swing because `walk` is PLAYING over it, and this preview
   only ever ran the clip the author had explicitly opened. Pressing K
   therefore showed IK legs and rest arms — a pose the engine never produces
   while moving. See anim.js pickLocoClip for the three details (exclusivity,
   the 0.80/0.70 hysteresis split, the stride re-rate) that are load-bearing.
   ========================================================================== */

let skel = null;              // built by rebuildSkeleton()
let anim = null;              // AnimState mirror
let previewCtx = null;
let previewOrigin = { x: 0, y: 0, z: 0 };
let restModel = null;         // model-space rest pose, for the delta transform
// AUTO-LOCO: let the family drive the clips while walking, instead of pinning
// the one the author has open. Off while a clip is being AUTHORED (that is
// what the clip lane is for); on by default for the gait preview, which is
// where "does this match the game" is the question being asked.
let autoLoco = true;

// Rebuild the runtime skeleton from the CURRENT sidecar + models. Called
// whenever either changes; cheap enough to do on every edit.
function rebuildSkeleton() {
  const models = ed.getModels();
  skel = AN.buildSkeleton(ed.getSidecar() || {}, models);
  anim = {
    clips: [],
    local: [], model: [],
    partAlive: new Array(skel.parts.length).fill(1),
    springs: skel.parts.map(() => ({ x: AN.v3(), v: AN.v3() })),
    feet: skel.chains.map(c => ({
      valid: false, swinging: false, planted: AN.v3(),
      swingFrom: AN.v3(), swingTo: AN.v3(), swingT: 0,
      legLength: AN.chainLegLength(skel, c),
    })),
    gaitPhase: 0,
    velocity: AN.v3(),
    bodyY: 0,
    bodyUp: AN.v3(0, 1, 0),
    // ---- the stride clock (avatar.cpp SyncStrideClock) ----
    // Zero until two steps have been TIMED, which is the honest answer for a
    // body that has not taken a stride yet — and is why the pelvis holds still
    // for the first step of a preview instead of guessing a rate.
    strideRate: 0, stepPeriod: 0, sinceTouchdown: 0, lastFootDown: -1,
    running: false,          // the walk/run hysteresis latch
  };
  // avatar.cpp:634 calls SyncStrideClock from the touchdown inside its own
  // UpdateGait; anim.js's updateGait is the NPC transcription, which does not,
  // so the avatar's half is installed as a hook rather than baked in.
  anim.onTouchdown = c => {
    dbgTouchdowns[c] = (dbgTouchdowns[c] || 0) + 1;
    AN.animSyncStrideClock(skel, anim, c);
  };
  previewOrigin = { x: 0, y: 0, z: 0 };
  previewCtx = {
    origin: AN.v3(), heading: 0, speedNow: 0,
    defSpeed: num(ed.getSidecar()?.speed, 5),
    prefabSize: ed.getDoc()?.size || { x: 1, y: 1, z: 1 },
    rootLimb: skel.rootLimb,
    footInit: false,
    groundY: () => 0,                       // APPROXIMATION: flat editor ground
  };
  // Rest pose in model space: the reference every preview transform is a
  // delta against, because the renderer draws models at their static prefab
  // offsets and we hand it a correction rather than an absolute placement.
  const rest = { clips: [], local: [], model: [], partAlive: anim.partAlive, springs: anim.springs };
  AN.animSampleAndBlend(skel, rest, 0);
  AN.animFlatten(skel, rest);
  restModel = rest.model;
  // SEED THE LIVE POSE TOO, at rest. anim.model[] is what everything asks the
  // rig about — where the shoulder is, how long the arm is, where the held
  // blade's edge runs — and it was EMPTY until the first preview step, which
  // only runs while something is animating. So equipping a sword on a still
  // rig left the Attacks readout showing a 0-voxel blade and a bare-arm reach
  // band until you pressed swing, and the driver was handed ClearArm on every
  // tick in between. The engine has no such gap: anim_.model is populated from
  // the first tick and stays populated.
  AN.animSampleAndBlend(skel, anim, 0);
  AN.animFlatten(skel, anim);
  strokeBodyClipIdx = -1;
  weaponRebuild();
}

function setGait(on) {
  gaitOn = on;
  if (!on) { rebuildSkeleton(); }
  ed.invalidate();
}

// Overwrite the selected part's local rotation with the in-progress pose.
// Only while a ring is actually being dragged or a clip is open, so it never
// interferes with plain gait preview.
function applyPoseEdit() {
  if (!poseEdit || !skel || !anim.local.length) return;
  const pi = skel.findPart(poseEdit.part);
  if (pi < 0) return;
  anim.local[pi].rot = AN.qnorm(poseEdit.rot);
  anim.local[pi].pos = AN.vadd(skel.parts[pi].rest.pos, poseEdit.pos);
}

/**
 * One preview step: exactly the engine's stage order.
 * `walk` drives the mob forward so the gait state machine has velocity to
 * react to — without it every foot stays planted and nothing moves, which is
 * correct engine behaviour (mob.cpp:608) but useless as a preview.
 */
/**
 * THE PREVIEW RUNS ON A FIXED STEP, like the engine, not on the frame's dt.
 *
 * `UpdateAnimation` is called from the sim tick, so every duration in a gait
 * block is measured against a constant. Feeding it the raw frame delta makes
 * the foot state machine frame-rate dependent, and it degenerates outright
 * once the frame is longer than `stepDuration`: at 10 fps against the human's
 * 0.1 s swing, `swingT += dt/stepDuration` reaches 1 in a SINGLE frame, so
 * every foot lands and re-steps every frame and the measured stride rate is
 * really the frame rate. Measured in the headless harness: stepPeriod 0.085 s
 * against a 0.085 s frame, giving 5.9 strides/s on a rig walking at 0.55.
 *
 * Sub-stepping costs nothing at 60 fps (one step per frame) and keeps a slow
 * or backgrounded tab honest. Capped so a stalled tab does not try to catch up
 * a hundred steps at once — the same bound weaponStep uses for the same reason.
 */
const kPreviewDt = 1 / 60;
let previewAccum = 0, previewPrimed = false;

function stepPreview(dtFrame) {
  if (!skel || !anim) return;
  previewAccum += dtFrame;
  let n = Math.min(Math.floor(previewAccum / kPreviewDt), 6);
  previewAccum -= n * kPreviewDt;
  // The first call must produce a pose even if the frame was short, or nothing
  // is drawn until the accumulator fills.
  if (n <= 0 && !previewPrimed) n = 1;
  previewPrimed = true;
  while (n-- > 0) stepPreviewFixed(kPreviewDt);
}

function stepPreviewFixed(dt, skipWeapon = false) {
  if (!skel || !anim) return;
  dbgPreviewSteps++;
  const sidecar = ed.getSidecar() || {};
  previewCtx.defSpeed = num(sidecar.speed, 5);
  previewCtx.prefabSize = ed.getDoc()?.size || { x: 1, y: 1, z: 1 };
  previewCtx.rootLimb = skel.rootLimb;

  // ---- the stroke driver, BEFORE the animation pass (mob.cpp:1895) -------
  // "Advance the stroke and push the pose, so the animation pass sees THIS
  // tick's pose rather than last tick's." Runs on the sim's own 30 Hz clock.
  if (!skipWeapon) weaponStep(dt);

  const speed = previewCtx.defSpeed * gaitSpeedScale;

  // ---- WHICH CLIPS ARE PLAYING ------------------------------------------
  //
  // Two mutually exclusive drivers, because the tab answers two questions:
  //
  //   AUTHORING a clip (the clip lane is open): the runtime instance is
  //   PINNED to the editor's cursor every step. animSampleAndBlend is the
  //   ENGINE's own loop and advances timeMs by dt*1000*rate itself, so
  //   pre-compensate — after its advance the sample lands exactly on
  //   clipCursorMs, paused / scrubbing / playing alike. `ageMs` is pinned
  //   past the blend-in for the same reason: a scrub is not a fade-in, and
  //   showing one would dim every pose the author is trying to judge.
  //
  //   PREVIEWING the gait (K, no clip open, or auto-loco on): the LOCOMOTION
  //   FAMILY drives it exactly as avatar.cpp does — one clip of
  //   idle/walk/run, exclusive, stride-rate-locked. This is the arm swing.
  // Preserve the stroke's body clip instance across the clip management
  // block — it was started by beginStroke and must keep running until the
  // stroke finishes. animSampleAndBlend advances its timeMs each frame.
  const strokeClipInst = (strokeBodyClipIdx >= 0)
      ? anim.clips.find(i => i.clip === strokeBodyClipIdx) : null;

  const authoring = !!activeClip && !(autoLoco && gaitOn);
  if (authoring) {
    const ci = skel.clips.findIndex(k => k.name === activeClip);
    anim.clips = ci >= 0
      ? [{ clip: ci, timeMs: clipCursorMs - dt * 1000, ageMs: 1e6, rate: 1,
           weight: 1, stopping: false, fade: 1 }]
      : [];
  } else if (gaitOn) {
    const want = AN.pickLocoClip(skel, anim, speed, previewCtx.defSpeed, {});
    const family = ['idle', 'walk', 'run', 'fall', 'hang'];
    const famIdx = new Set(family
      .map(n => skel.clips.findIndex(c => c.name === n)).filter(i => i >= 0));
    // THE FAMILY IS EXCLUSIVE (avatar.cpp:1679): every member that is not the
    // wanted one is retired. Leaving `idle` running under a walk is what
    // dragged an authored 14-degree arm swing down to about 4 in the engine,
    // and it would do exactly the same here.
    const wantIdx = want ? skel.clips.findIndex(c => c.name === want) : -1;
    for (const inst of anim.clips)
      if (famIdx.has(inst.clip) && inst.clip !== wantIdx) inst.stopping = true;
    if (wantIdx >= 0 && !anim.clips.some(i => i.clip === wantIdx && !i.stopping))
      anim.clips.push({ clip: wantIdx, timeMs: 0, ageMs: 0, rate: 1,
                        weight: 1, stopping: false, fade: 1 });
    // ---- lock the arm swing to the feet (avatar.cpp:1737) ----------------
    // Against the MEASURED stride rate, not the oscillator — see the note at
    // the gait clock below, and anim.js's animSyncStrideClock.
    if (anim.strideRate > 1e-4) {
      const wi = skel.clips.findIndex(c => c.name === 'walk');
      const ri = skel.clips.findIndex(c => c.name === 'run');
      for (const inst of anim.clips) {
        if (inst.clip !== wi && inst.clip !== ri) continue;
        const dur = skel.clips[inst.clip]?.durationMs || 0;
        if (dur <= 0.1) continue;
        inst.rate = AN.clipRateForStride(anim.strideRate, dur);
      }
    }
  } else {
    anim.clips = [];
  }

  if (strokeClipInst && !anim.clips.some(i => i.clip === strokeBodyClipIdx))
    anim.clips.push(strokeClipInst);

  if (gaitOn) {
    // Walk along +Z with heading 0 (APPROXIMATION: the engine's heading comes
    // from AI steering; the model is authored in its own frame).
    previewOrigin.z += speed * dt;
    previewCtx.origin = AN.v3(previewOrigin.x, previewOrigin.y, previewOrigin.z);
    anim.velocity = AN.v3(0, 0, speed);
    previewCtx.speedNow = speed;
  } else {
    previewCtx.origin = AN.v3(previewOrigin.x, previewOrigin.y, previewOrigin.z);
    anim.velocity = AN.v3();
    previewCtx.speedNow = 0;
  }

  // ---- the gait clock (avatar.cpp:945 animAdvanceGaitPhase) --------------
  // ONE CLOCK, AND THE FEET OWN IT. This used to be the NPC oscillator
  // (`cadence * speedFactor`), which on the shipped human wanted 4.6 strides/s
  // at a walk while the feet were taking about 1.8 — so the pelvis swayed at
  // 2.6x the footfall rate and any clip rated against it pinned at the rate
  // clamp. avatar.cpp abandoned that for exactly this reason and the preview
  // has to follow, because the rig being previewed IS the player avatar.
  const g = skel.gait;
  const strideRate = AN.animAdvanceGaitPhase(skel, anim, previewCtx, dt);

  // stages 1-3
  AN.animSampleAndBlend(skel, anim, dt);
  // Live pose being authored: applied on top of the sampled clip so the
  // rotation rings show their effect before the key is committed. This is an
  // EDITOR-ONLY layer — nothing in the engine corresponds to it, which is why
  // it goes here rather than inside anim.js.
  applyPoseEdit();
  // procedural layer (legacy swing + bob/sway/roll/spine + springs)
  AN.applyProceduralLayer(skel, anim, previewCtx, dt);
  // ---- stage 3.5: the torso serves a live swing (anim.h) ----------------
  // The pose's torso fields are already weight-scaled by the driver, so an
  // idle rig (weight 0) is untouched and a rig mid-stroke leans exactly as
  // the avatar does. Runs BEFORE the flatten in both engine drivers.
  {
    const wp = weaponPose();
    if (wp) AN.animApplySpineTwist(skel, anim, wp.torsoTwist, wp.torsoPitch,
                                   skel.rootLimb);
  }
  // ---- stage 3.5b: an AIM effector points its own part (mob.cpp:14777) ---
  // Also pre-flatten, and after the torso twist for the same reason the engine
  // orders them that way: the spine share this adds rides on top of whatever
  // the swing's own torso lean already put there.
  lastAim = null;
  lastPoseDriven = !!weaponPose();
  applyStrikeAim();
  // stage 4
  AN.animFlatten(skel, anim);
  // gait + stage 5 IK
  if (g.present && gaitOn) {
    AN.updateGait(skel, anim, previewCtx, dt);
    // mob.cpp:849 — world foot target -> model space. heading is 0 here so the
    // inverse yaw is identity; bodyOrigin uses the derived bodyY.
    const pivot = AN.v3(previewCtx.prefabSize.x * 0.5, 0, previewCtx.prefabSize.z * 0.5);
    const bodyOrigin = AN.v3(previewOrigin.x, anim.bodyY, previewOrigin.z);
    for (let c = 0; c < skel.chains.length && c < anim.feet.length; c++) {
      const f = anim.feet[c];
      const weight = f.valid ? skel.chains[c].weight : 0;
      if (weight <= 0) continue;
      const rel = AN.vsub(AN.vsub(f.planted, bodyOrigin), pivot);
      const prefabPt = AN.vadd(rel, pivot);          // heading 0 => RotateInv = id
      // PREFAB-ABSOLUTE, AND NOT REBASED — matches avatar.cpp / mob.cpp. The
      // hip animSolveTwoBone reads out of anim.model already carries the root
      // offset (animFlatten seeds the root from its own rest.pos, which IS the
      // root anchor), so subtracting rootAnchor from the target alone put the
      // two ends in different frames and splayed both legs sideways.
      AN.animSolveTwoBone(skel, anim, skel.chains[c], prefabPt, weight);
    }
  }
  // ---- stage 5.5: the weapon arm (game/melee.h) ------------------------
  // THE SAME CALL BOTH ENGINE DRIVERS MAKE. Returns the steered hinge plane
  // for the clamp below, exactly as Mob::ApplyWeaponArm fills a
  // PoseAxisOverride for AnimClampPoseLimits.
  const weaponHinge = applyWeaponArm();
  // ...and the style's per-joint brakes on what it solved (pose.cpp, right
  // after ApplyWeaponArm; melee.h ArmSmooth).
  smoothWeaponArm(dt);
  ATK.updateJointLag(armSmooth.valid ? armSmooth.lag : null);
  // ---- stage 6: the pose has to be anatomically possible ---------------
  // After every solve, never between them: the IK is what puts a joint out of
  // range, so clamping earlier would only clamp a pose about to be replaced.
  AN.animClampPoseLimits(skel, anim, weaponHinge ? [weaponHinge] : null);
  reapplyKeyedArm();
  // The blade's swept edge, recorded AFTER the pose is final — the item rides
  // the hand's animated transform, so anything sampled earlier is a tick stale.
  recordStrokeTrail();
  pushStrokeTrail();
  // The open natural weapon's edge rides the live pose too — same reason the
  // trail is sampled here and not earlier.
  renderNaturalEdgeOverlay();
  ed.invalidate();
}

/**
 * Hand the recorded sweep to the viewport. Windup is dim amber (the telegraph),
 * the cut is hot white-orange (the part that damages), the recover fades out.
 * Older ticks fade so the direction of travel is readable from a still.
 */
function pushStrokeTrail() {
  if (!strokeTrail.length) { ed.setStrokeTrail?.(goalMarkerSegs); return; }
  const n = strokeTrail.length;
  const segs = strokeTrail.map((s, i) => {
    const age = (i + 1) / n;                       // 0 oldest .. 1 newest
    const cut = s.phase === MELEE.STROKE_PHASE.Cut;
    return {
      a: [s.base.x, s.base.y, s.base.z],
      b: [s.tip.x, s.tip.y, s.tip.z],
      color: cut ? 0xffd08a : 0x6aa9ff,
      alpha: (cut ? 0.35 : 0.14) + age * (cut ? 0.65 : 0.3),
    };
  });
  // THE ROWS THE GAME'S HIT TEST CASTS BETWEEN TWO CUT TICKS (melee.cpp
  // MeleeSweepDamage): the edge lerped from last tick to this one, one row per
  // `sweepSpacing` of TIP travel, capped at `sweepMaxSteps`. The lines above
  // are where the blade IS each tick; these thin ones are where the game
  // LOOKS for flesh in between, so a gap here is a gap a limb can pass
  // through. The trail is in FILE voxels, the spacing in world voxels.
  const mt = meleeTuning();
  const S = rigScale();
  const spacing = Math.max(mt.sweepSpacing, 0.05) * S;
  const cap = Math.max(1, Math.round(mt.sweepMaxSteps));
  for (let i = 1; i < n; i++) {
    const p = strokeTrail[i - 1], q = strokeTrail[i];
    if (q.phase !== MELEE.STROKE_PHASE.Cut) continue;
    const steps = clamp(Math.ceil(AN.vlen(AN.vsub(q.tip, p.tip)) / spacing), 1, cap);
    const alpha = 0.12 + 0.3 * ((i + 1) / n);
    for (let k = 1; k < steps; k++) {
      const u = k / steps;
      const a = AN.vadd(p.base, AN.vmul(AN.vsub(q.base, p.base), u));
      const b = AN.vadd(p.tip, AN.vmul(AN.vsub(q.tip, p.tip), u));
      segs.push({ a: [a.x, a.y, a.z], b: [b.x, b.y, b.z], color: 0xff7a3a, alpha });
    }
  }
  // THE HAFT'S RIBBON (a mace's stick): its own shade -- sea-green, and
  // dimmer than the head's -- because it is the same kind of hitbox at a
  // fraction of the strength (sidecar `haft.power`). Its in-between rows are
  // drawn too, fainter, so a gap in the stick's coverage is visible like one
  // in the head's.
  for (let i = 0; i < n; i++) {
    const s = strokeTrail[i];
    if (!s.haftBase || !s.haftTip) continue;
    const age = (i + 1) / n;
    const cut = s.phase === MELEE.STROKE_PHASE.Cut;
    segs.push({ a: [s.haftBase.x, s.haftBase.y, s.haftBase.z],
                b: [s.haftTip.x, s.haftTip.y, s.haftTip.z],
                color: cut ? 0x5fd6a4 : 0x3f7f86,
                alpha: (cut ? 0.25 : 0.1) + age * (cut ? 0.45 : 0.2) });
    const p = i > 0 ? strokeTrail[i - 1] : null;
    if (!cut || !p || !p.haftBase || !p.haftTip) continue;
    const steps = clamp(Math.ceil(AN.vlen(AN.vsub(s.haftTip, p.haftTip)) / spacing), 1, cap);
    const alpha = 0.08 + 0.2 * age;
    for (let k = 1; k < steps; k++) {
      const u = k / steps;
      const a = AN.vadd(p.haftBase, AN.vmul(AN.vsub(s.haftBase, p.haftBase), u));
      const b = AN.vadd(p.haftTip, AN.vmul(AN.vsub(s.haftTip, p.haftTip), u));
      segs.push({ a: [a.x, a.y, a.z], b: [b.x, b.y, b.z], color: 0x2f9f78, alpha });
    }
  }
  ed.setStrokeTrail?.(goalMarkerSegs ? segs.concat(goalMarkerSegs) : segs);
}

/**
 * Per-model transform for the renderer. We compute the DELTA between the posed
 * model-space transform and the rest model-space transform, then express it as
 * a rotation about the part's anchor plus a translation — which is what
 * editor.js's instance rebuild applies.
 */
function modelTransform(modelIndex) {
  if (!previewActive() || !skel || !anim || !anim.model.length || !restModel) return null;
  const m = ed.getModels()[modelIndex];
  if (!m) return null;
  const pi = skel.parts.findIndex(p => p.modelIndex === modelIndex);
  if (pi < 0) return null;

  const posed = anim.model[pi], rest = restModel[pi];
  if (!posed || !rest) return null;

  // delta rotation q = posed.rot * rest.rot^-1, applied about the rest anchor
  const dq = AN.qnorm(AN.qmul(posed.rot, AN.qconj(rest.rot)));
  const anchor = skel.parts[pi].anchorLocal;
  // Translation: where the anchor ends up, minus where rotating about the old
  // anchor would put it.
  const dp = AN.vsub(posed.pos, rest.pos);

  // Body height: the whole rig rides on bodyY relative to its rest ground.
  let dy = 0;
  if (gaitOn && skel.gait.present && anim.feet.length) {
    // Same frame as anim.js/mob.cpp: bodyY is the prefab min corner, and the
    // rest stance is the sole height plus the rideHeight trim about it. Using
    // the old `rideHeight * legLength` here would offset the preview by a leg
    // length against the engine.
    const restY = AN.restSoleY(skel) +
                  (skel.gait.rideHeight - 1) * (anim.feet[0].legLength || 1);
    dy = anim.bodyY - restY;
  }

  // THE LUNGE CARRIES THE WHOLE BODY, so it is added here rather than to any
  // one part: the arc is a translation of the creature, exactly as
  // Mob::Launch moves `origin_` and every limb rides it. In FILE voxels,
  // which is the one conversion this seam owns.
  const lg = lungeOffsetModel();
  return {
    pivot: { x: anchor.x, y: anchor.y, z: anchor.z },
    quat: dq,
    pos: { x: dp.x + lg.x, y: dp.y + dy + lg.y, z: dp.z + lg.z },
  };
}

/** Where the lunge has carried the body, in FILE voxels. Zero when not flying. */
function lungeOffsetModel() {
  if (!lungeArc) return AN.v3();
  const S = rigScale();
  // Forward is +Z at heading 0 (the rig's own frame), which is the direction
  // the preview's walk already travels in.
  return AN.v3(0, lungeArc.y * S, lungeArc.x * S);
}

// The preview runs whenever something wants a posed rig: the gait walk, clip
// playback, simply having a clip open (so scrubbing and ring-dragging show
// their result on a paused rig), or a live stroke.
// ...or a held GOAL FRAME, which is a posed rig with nothing live on it: the
// arm sits on one of the style's destinations and does not move, so none of
// the other four predicates is true and without this the preview would not
// run at all (and the goal would never be drawn).
const previewActive = () =>
  gaitOn || clipPlaying || !!activeClip || strokeLive() || !!ATK.goalFrame?.();

/* ==========================================================================
   5b. THE WEAPON ARM — the real stroke driver, in the preview

   editor/melee.js is a line-cited port of MeleeState and the shared stroke
   program runner; this section is the CALLER, and it mirrors the three engine
   sites that make an authored attack move an arm:

     Mob::WeaponStrokePose (mob.cpp:8220)  where the blade is NOW, so the
       driver steers from the truth rather than from a guess
     Mob::HeadKeepOut      (mob.cpp:8262)  and where the wielder's own head is
     Mob::ApplyWeaponArm   (mob.cpp:7767)  the IK target, the steered pole and
       the hinge plane the stage-6 clamp is handed

   UNITS ARE THE ONE THING THIS SEAM OWNS. The driver speaks WORLD voxels
   (every constant in melee.h is a converted metre); the editor's model space
   is FILE voxels, `skinScale` of them per world voxel. Everything handed IN is
   divided by that and everything handed BACK is multiplied by it, in this
   file, once — melee.js carries no scale so its constants stay byte-identical
   to the header's.
   ========================================================================== */

// sim/scale.h:100 — the world's own resolution. 1 / kVoxelMeters.
const kVoxelsPerMetre = 10;

/**
 * sim/scale.h:114 SkinScaleFor — file voxels per WORLD voxel, from the asset's
 * own authored density.
 *
 * `skinScale`/`scale` are the LEGACY keys and are honoured first because a
 * hand-written test rig may still carry one; every shipped asset states
 * `artVoxelsPerMetre` instead and the engine derives the factor from it.
 * Deriving it here too is what fixed the held-item preview: the human is
 * authored at 80 vox/m (skinScale 8) and the sword at 40 (scale 4), so one
 * sword voxel is TWO human voxels — and with both keys absent the old
 * `rigScale / itemScale` read 1/1 and drew the blade at half length. A blade
 * length that is wrong is not cosmetic here: `MeleeState::SetStroke` MEASURES
 * it, and the whole reach band is solved against it.
 */
function skinScaleOf(doc) {
  const legacy = +(doc?.skinScale ?? doc?.scale);
  if (Number.isFinite(legacy) && legacy > 0) return legacy;
  const a = +doc?.artVoxelsPerMetre;
  if (Number.isFinite(a) && a > 0 && a % kVoxelsPerMetre === 0)
    return a / kVoxelsPerMetre;
  return 1;
}

const rigScale = () => skinScaleOf(sc());

// --- live stroke state ----------------------------------------------------
//
// THE STYLE DOCUMENT LIVES IN attacks.js, not here. This file owns the RIG and
// the driver; that one owns attack_styles.json and the panel. The seam is the
// object registered with ATK.bind() below, and it is the only thing either
// file knows about the other.
let melee = null;             // MELEE.MeleeState for the armed hand, or null
let strokeCur = null;         // MELEE.newStrokeCursor()
let strokeStyle = -1;         // the style the live program is running
let strokeTick = 0;           // ticks into the current program
let strokeTrail = [];         // [{base,tip,phase}] the swept edge, FILE voxels
const kStrokeTrailMax = 90;
// The blade tip's speed and edge alignment, for the panel's readout. Measured
// here rather than in melee.js because they are a property of the POSED blade
// (the item riding the animated hand), not of the driver's internal frame —
// which is the same reason MeleeSweepDamage measures them off the rig.
let strokeTipSpeed = 0, strokeEdgeAlign = 1;
let strokeTipPrev = null;
// WHAT THE PROGRAM ASKED FOR VERSUS WHAT THE ARM DID — the numbers that turn
// "the elevation wanders" into a cause. `strokeStart` is the pose the driver
// SEEDED from (the arm as it hung when the swing began: a stroke starts from
// wherever the blade IS, melee.cpp's take-over), `strokeCommit` the residual
// between the windup's target and the pose actually reached when the aim was
// frozen. A windup shorter than the raise it has to make shows up here as a
// residual the cut then spends its own ticks closing.
let strokeStart = null;       // {az, el, radius} after the first driven tick
let strokeCommit = null;      // {dAz, dEl, dR} at the windup -> cut edge
// SOLO: hold the pose at the end of the chosen segment for a beat instead of
// running on, and burst through the segments before it. See weaponTick.
let strokeHoldTicks = 0;
let strokeBodyClipIdx = -1;
const kSoloHoldTicks = 24;    // 0.8 s of holding the segment's end pose
// The rig parts the driver needs. -1 until weaponRebuild() finds them.
let wpHandPart = -1, wpChain = -1, wpHeadPart = -1;

// ---- THE STRIKE EFFECTOR (mob.cpp:14530 Mob::SetStrikeEffector) -----------
//
// WHICH PART THE DRIVER IS MOVING, and how. Until the unarmed package the
// answer was always "the hand the weapon socket names", because the whole
// swing pipeline started at the held item; a fist and a set of jaws have no
// item, so the part itself has to be nameable (game/impact.h
// StrikeEffectorMode, docs/PLAN_impact_unarmed.md §4):
//
//   'held'   the socket's hand, holding an item. Today's path, untouched.
//   'chain'  a natural weapon on a part an IK chain serves (a fist): the same
//            two-bone solve, no wrist steering (there is no blade to lay along
//            a line), and the swept edge is the PART'S OWN.
//   'aim'    a natural weapon on a part in no chain (the jaws): the part and a
//            share of the spine are ROTATED until the part's forward lies
//            along the stroke's bearing. No IK at all.
//
// `strikeEff` is the OVERRIDE and -1 means "no override" — resolveEffector()
// then falls through to the held hand, exactly as Mob::ResolveEffector does,
// so a creature that finishes a punch while still holding a sword goes back to
// carrying the sword without anything having to write that value back.
let strikeEff = -1;              // part index, or -1
let strikeMode = 'none';         // 'none' | 'held' | 'chain' | 'aim'
let strikeNatural = -1;          // index into the sidecar's `natural` array

// THE SIM RUNS AT 30 Hz AND SO DOES A STROKE. `ticks` in attack_styles.json
// are sim ticks, and StepStrokeProgram advances exactly one per call, so the
// preview must step on the same clock or a 12-tick windup would not be 0.40 s.
const kStrokeDt = 1 / 30;

const strokeLive = () =>
  !!(strokeCur && strokeCur.phase !== MELEE.STROKE_PHASE.Idle);

/**
 * The socket the weapon hangs from. Prefers the one NAMED for the grip context
 * being edited, so the arm the panel drives does not change when the author
 * clicks a different socket in the rig list; falls back to the selection and
 * then to the first socket, so a rig with one unnamed socket still works.
 */
function weaponSocket() {
  const SK = sockets();
  const byCtx = SK.find(s => s.name === gripCtx);
  if (byCtx) return byCtx;
  return activeSocket() || SK[0] || null;
}

/**
 * Re-derive which parts the weapon arm is made of. Called from
 * rebuildSkeleton(), so it follows every rig edit.
 *
 * DERIVED FROM THE RIG, NEVER HARDCODED — mob.cpp:7774 says why: "a
 * left-handed rig, or a second weapon, needs no change here". The hand is the
 * socket's part and the arm is the chain tagged `arm` whose effector IS that
 * hand.
 */
function weaponRebuild() {
  wpHandPart = wpChain = wpHeadPart = -1;
  if (!skel) return;
  const sock = weaponSocket();
  if (sock && sock.part) wpHandPart = skel.findPart(sock.part);
  if (wpHandPart < 0) {
    // No socket yet: fall back to the arm chain the author is most likely to
    // mean, so the Attacks tab is useful before a socket exists.
    const arm = (skel.chains || []).find(c => c.tag === 'arm' && c.effector >= 0);
    if (arm) wpHandPart = arm.effector;
  }
  wpChain = (skel.chains || []).findIndex(
    c => c.tag === 'arm' && c.effector === wpHandPart && c.parts.length >= 2);
  wpHeadPart = skel.parts.findIndex(p => p.tag === 'head');
  if (!melee) {
    melee = new MELEE.MeleeState(meleeTuning());
    strokeCur = MELEE.newStrokeCursor();
  }
  melee.tuning = meleeTuning();
  // mob.cpp:8123 HandSign — ".L" is the character's LEFT in the NAME, and the
  // name is what the stroke's asymmetric azimuth limits want.
  const hn = wpHandPart >= 0 ? (skel.parts[wpHandPart].name || '') : '';
  melee.setHandSign(hn.endsWith('.L') ? -1 : 1);
  // ...and what it is holding, so a still rig reports a real blade and a real
  // band rather than the empty-fist defaults. See weaponSeedFromRig.
  weaponSeedFromRig();
}

/* --------------------------------------------------------------------------
   NATURAL WEAPONS (mob.h MobNaturalWeaponDef; the sidecar's `natural` block)

   A fist and a set of jaws are weapons nobody handed the creature: an authored
   EDGE SEGMENT and a StrikeProfile, living on a part of the rig instead of on
   a borrowed slot. The editor reads the same array the engine does and does
   not resolve it any further — `partIndex` is looked up per call rather than
   cached, because the Models tab renames and deletes limbs while the panel is
   open and a cached index would outlive the part it names.
   -------------------------------------------------------------------------- */

const naturalList = () => {
  const a = sc()?.natural;
  return Array.isArray(a) ? a : [];
};
const naturalIndexNamed = (name) =>
  naturalList().findIndex(w => w && w.name === name);

/**
 * mob.cpp:14746 Mob::ChainForEffector — the chain whose EFFECTOR is this part
 * or this part's parent. Deriving it from the rig rather than naming an arm is
 * what lets a left-handed rig, a second weapon or a bare fist need no change
 * here.
 *
 * Returns the chain INDEX (rig.js addresses chains by index everywhere else)
 * or -1, and writes the hand part into `out.hand`.
 */
function chainForEffector(part, out) {
  if (out) out.hand = -1;
  if (!skel || part < 0 || part >= skel.parts.length) return -1;
  const parent = skel.parts[part].parent;
  for (let i = 0; i < skel.chains.length; i++) {
    const ch = skel.chains[i];
    if (!ch || (ch.parts || []).length < 2 || ch.effector < 0) continue;
    if (ch.effector !== part && ch.effector !== parent) continue;
    if (out) out.hand = ch.effector;
    return i;
  }
  return -1;
}

/**
 * mob.cpp:14582 Mob::NaturalWeaponUsable — the part is alive AND, for a chain
 * effector, so is every part of the chain that serves it. In the editor
 * nothing is severed, so "alive" reduces to "is a limb of this rig" — but the
 * shape is kept because it is the thing the panel's warning is about: a style
 * naming a weapon this rig does not declare is a content error whose only
 * other symptom is a creature that never swings.
 */
function naturalUsable(nw) {
  if (!skel || !nw || !nw.part) return false;
  const pi = skel.findPart(nw.part);
  if (pi < 0) return false;
  const out = {};
  const ci = chainForEffector(pi, out);
  if (ci >= 0)
    for (const k of skel.chains[ci].parts) if (k < 0 || k >= skel.parts.length) return false;
  return true;
}

/**
 * mob.cpp:14552 Mob::ResolveEffector — the explicit override if there is one,
 * else the held hand. Returns { part, mode, natural }; part < 0 means the rig
 * cannot say (no socket, no arm, no natural weapon).
 */
function resolveEffector() {
  if (strikeMode !== 'none' && strikeEff >= 0 && skel &&
      strikeEff < skel.parts.length)
    return { part: strikeEff, mode: strikeMode, natural: strikeNatural };
  if (wpHandPart >= 0) return { part: wpHandPart, mode: 'held', natural: -1 };
  return { part: -1, mode: 'none', natural: -1 };
}

/**
 * mob.cpp:3273 Mob::ArmForStyle — point the driver at whatever this style
 * swings. Called from beginStroke, exactly where MobSystem::BeginStroke calls
 * it, so the preview and the game choose the effector from the same field.
 *
 * Returns false when the style names a weapon this rig has not got, which is
 * the same answer that stops the engine starting a stroke with no edge on the
 * end of it — the panel says so rather than previewing a swing the game would
 * never play.
 */
function armForStyle(sty) {
  if (!sty || !sty.weapon || sty.weapon === 'held') {
    clearStrikeEffector();
    if (wpHandPart < 0) return false;
    // UNARMED MODE IS UNARMED even if an item is still loaded (a late fetch,
    // a dropdown): the fist swings, never a sword the panel is not showing.
    if (heldItemId && ATK.currentWeaponMode?.() !== 'unarmed') return true;
    // mob.cpp Mob::ArmForStyle: AN EMPTY HAND THROWS THE STROKE WITH ITS
    // FIST -- the natural weapon on the hand part -- so the preview swings
    // (and trails) the knuckles, as the game does. A rig whose sidecar does
    // not restate `natural` (a character inheriting the human's) still arms
    // the hand; effectorEdgeModel then trails the hand's own box.
    const NW = naturalList();
    const ni = NW.findIndex(w => w && w.part && skel.findPart(w.part) === wpHandPart);
    const out = {};
    strikeEff = wpHandPart;
    strikeMode = chainForEffector(wpHandPart, out) >= 0 ? 'chain' : 'aim';
    strikeNatural = ni >= 0 && naturalUsable(NW[ni]) ? ni : -1;
    return true;
  }
  const ni = naturalIndexNamed(sty.weapon);
  const nw = ni >= 0 ? naturalList()[ni] : null;
  if (!nw || !naturalUsable(nw)) return false;
  const pi = skel.findPart(nw.part);
  const out = {};
  const chained = chainForEffector(pi, out) >= 0;
  strikeEff = pi;
  strikeMode = chained ? 'chain' : 'aim';
  strikeNatural = ni;
  return true;
}

function clearStrikeEffector() {
  strikeEff = -1;
  strikeMode = 'none';
  strikeNatural = -1;
}

/**
 * mob.cpp:15622 Mob::WeaponEdge, natural branch — the authored segment on the
 * part's LIVE transform, in the editor's MODEL space (file voxels).
 *
 * TWO DIFFERENCES FROM THE ENGINE, both of them frame bookkeeping:
 *
 *   * NO ArtToWorld SCALING. The engine multiplies the authored points by
 *     `def.ArtToWorld()` at load because its model space is WORLD voxels; the
 *     editor's is file voxels, which is the frame the numbers are authored in.
 *     The seam converts once, where every other length does (weaponArmSeed).
 *   * THE ORIGIN IS THE JOINT, NOT THE MIN CORNER. `WeaponEdge` composes
 *     against the limb's Jolt transform, whose position IS the model's min
 *     corner; `anim.model[i].pos` is the part's JOINT. mob.cpp states the
 *     relation in WeaponStrokePose — world min corner = joint - rot * anchor —
 *     so the rebasing term is `-anchorLocal`, and leaving it out puts a fist's
 *     knuckles wherever the wrist happens to be, which is a whole hand's
 *     length of quiet error.
 */
function naturalEdgeModel(nw) {
  if (!nw || !skel || !anim?.model?.length) return null;
  const i = skel.findPart(nw.part);
  if (i < 0 || !anim.model[i]) return null;
  const e = nw.edge || {};
  const pt = (v) => (Array.isArray(v) && v.length === 3)
    ? AN.v3(+v[0] || 0, +v[1] || 0, +v[2] || 0) : null;
  const from = pt(e.from), to = pt(e.to);
  if (!from || !to) return null;
  const m = anim.model[i];
  // THE EDGE IS AUTHORED FROM THE PART MODEL'S OWN MIN CORNER (mob.cpp's
  // loader; the engine composes it on the limb's min-corner transform), and
  // `anchorLocal` is in PREFAB space. So the authored point goes to prefab
  // space first (+ the model's offset), then rides the part exactly as
  // modelTransform() carries the drawn voxels: posed joint + the delta from
  // rest. Subtracting the prefab anchor from a model-local point threw the
  // fist ~8 voxels past the knuckles (the hand's whole prefab offset).
  const model = skel.parts[i]._model;
  const off = model && model.offset ? AN.v3(model.offset.x, model.offset.y, model.offset.z)
                                    : AN.v3();
  const rest = restModel?.[i];
  const restPos = rest ? rest.pos : skel.parts[i].anchorLocal;
  const dq = rest ? AN.qnorm(AN.qmul(m.rot, AN.qconj(rest.rot))) : m.rot;
  const place = (q) => AN.vadd(m.pos, AN.qrot(dq, AN.vsub(AN.vadd(q, off), restPos)));
  return { base: place(from), tip: place(to), part: i,
           halfWidth: +e.halfWidth || 0, flat: AN.v3() };
}

/**
 * The segment the driver is actually swinging, whichever effector is live:
 * the held item's blade, or the natural weapon's own edge. `flat` is a zero
 * vector for a natural weapon, which melee.js's edge-alignment reads as "no
 * evidence" rather than as a bad angle — a fist cannot land flat (mob.cpp
 * WeaponEdge says the same in the same words).
 */
function effectorEdgeModel() {
  const eff = resolveEffector();
  if (eff.mode === 'chain' || eff.mode === 'aim') {
    const nw = naturalList()[eff.natural];
    if (nw) return naturalEdgeModel(nw);
    return partBoxSegment(eff.part);
  }
  return bladeSegmentModel();
}

/**
 * A part's own reach as a segment, for an armed hand with no authored fist:
 * from its JOINT to the far end of its box along the joint-to-centre line,
 * posed exactly as modelTransform() carries the drawn voxels. MODEL space.
 */
function partBoxSegment(i) {
  if (i < 0 || !anim?.model?.[i]) return null;
  const posed = anim.model[i];
  const model = skel.parts[i]._model;
  const rest = restModel?.[i];
  if (!model || !model.offset || !model.dim || !rest) return null;
  const c = AN.v3(model.offset.x + model.dim.x * 0.5, model.offset.y + model.dim.y * 0.5,
                  model.offset.z + model.dim.z * 0.5);
  const dq = AN.qnorm(AN.qmul(posed.rot, AN.qconj(rest.rot)));
  const centre = AN.vadd(posed.pos, AN.qrot(dq, AN.vsub(c, rest.pos)));
  const tip = AN.vadd(posed.pos, AN.vmul(AN.vsub(centre, posed.pos), 2));
  return { base: posed.pos, tip, part: i, halfWidth: 0, flat: AN.v3() };
}

// tuning.json's melee block, live off the HOST'S OWN tuning document — the
// same object the Tuning tab edits, so one number has one home and a knob
// moved there is visible in the very next previewed swing. Supplied through
// __tunerModelsHooks (tuner.html) rather than fetched, because the page may
// have unsaved edits and re-reading the file would silently preview the
// version on disk instead of the one on screen.
let tuningGet = null, tuningTouchFn = null;
const tuningDoc = () => (tuningGet ? tuningGet() : null);
const tuningTouch = () => { tuningTouchFn?.(); };
const meleeTuning = () => MELEE.meleeTuningFrom(tuningDoc());

/**
 * mob.cpp:8173 WeaponArmPose + :8220 WeaponStrokePose — where the blade is
 * NOW, in the frame SetStroke speaks: shoulder-relative, WORLD voxels.
 *
 * Returns null when the rig cannot say (no arm chain, no live pose yet), which
 * is exactly the case the engine answers with ClearArm.
 */
function weaponArmSeed() {
  if (!skel || !anim || !anim.model.length) return null;
  const eff = resolveEffector();

  // ---- AIM: THE PIVOT IS THE PART'S OWN JOINT, AND THERE IS NO ARM -------
  //
  // mob.cpp:15402. A part in no chain still has a pivot and still has a
  // length, and the driver needs both — the pivot is what the stroke's azimuth
  // and elevation are measured about, the length is what bounds its reach
  // band. THE PIVOT IS THE PART'S OWN JOINT AND NOT THE PARENT'S, which is
  // where the engine's first version of this went wrong: four of the neck's
  // five voxels are rigid, nothing rotates about the chest, and handing the
  // driver a lever it cannot move divides every commanded radian by the
  // fraction the head actually owns (measured there: 0.08 rad realised on 0.51
  // commanded, which on screen is a bite that never opens its mouth).
  //
  // So the "hand" IS the pivot, the point is the edge tip, and `bladeLen_`
  // becomes the edge span — one part rotating, which is what a bite is.
  if (eff.mode === 'aim') {
    const nw = naturalList()[eff.natural];
    const seg = nw ? naturalEdgeModel(nw) : null;
    if (!seg || !anim.model[seg.part]) return null;
    const S = rigScale();
    const pivot = anim.model[seg.part].pos;
    const span = AN.vlen(AN.vsub(seg.tip, seg.base)) / S;
    if (!(span > 1e-3)) return null;
    return {
      hand: AN.v3(),                                   // the hand IS the pivot
      tip: AN.vmul(AN.vsub(seg.tip, pivot), 1 / S),
      flat: AN.v3(), reach: span, shoulder: pivot, S,
    };
  }

  // ---- HELD / CHAIN: a two-bone arm ---------------------------------------
  // The chain is the one whose EFFECTOR is the part or the part's parent, so a
  // fist is served by the arm the hand is on the end of without anything
  // naming "arm.R" (mob.cpp:14941).
  const hand = { hand: -1 };
  const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
  if (ci < 0 || !skel.chains[ci]) return null;
  const ch = skel.chains[ci];
  const handPart = hand.hand >= 0 ? hand.hand : wpHandPart;
  if (handPart < 0 || !anim.model[handPart]) return null;
  const i0 = ch.parts[0], i1 = ch.parts[1];
  if (i0 < 0 || i1 < 0 || !anim.model[i0] || !anim.model[i1]) return null;
  const S = rigScale();
  const shoulder = anim.model[i0].pos;
  const handModel = anim.model[handPart].pos;
  // Heading is 0 in the editor, so Rotate(yaw, .) is the identity and the
  // model-space difference IS the shoulder-relative offset.
  const handFromShoulder = AN.vmul(AN.vsub(handModel, shoulder), 1 / S);
  // Reach from the LIVE bone lengths rather than a constant: it follows the
  // rig, and it is the same L1+L2 the solver clamps its own annulus against —
  // including the effector-IS-the-lower-bone case, which animSolveTwoBone
  // extends by the bone's rest length (mob.cpp:8204).
  const root = anim.model[i0].pos, joint = anim.model[i1].pos;
  const tipJoint = (handPart === i1)
    ? AN.vadd(joint, AN.qrot(anim.model[i1].rot, skel.parts[i1].rest.pos))
    : anim.model[handPart].pos;
  const reach = (AN.vlen(AN.vsub(joint, root)) +
                 AN.vlen(AN.vsub(tipJoint, joint))) / S;
  if (!(reach > 1e-3)) return null;

  // ---- A CHAIN EFFECTOR REPORTS NO BLADE AT ALL (mob.cpp:15477) ----------
  //
  // `MeleeState::RadiusBand` has two models and picks between them on
  // `bladeLen_`: with a blade it solves the reach ANNULUS of a rigid bar pinned
  // at a hand held `extendLive_` from the shoulder, and with none it says "the
  // point IS the hand, so the band is simply the arm". A fist is the second
  // case wearing the first's clothes — the knuckles ride a voxel past the
  // wrist — and reporting that voxel as a blade collapsed the band to under two
  // voxels of travel and leaked the radial drive into elevation (measured in
  // the engine: a punch authoring 0.02 rad of elevation commanded 0.7 of it).
  //
  // So a natural weapon reports its TIP AT ITS HAND. Nothing about the hitbox
  // moves: the swept edge is still the authored segment on the live transform
  // (effectorEdgeModel), which is a separate question from where the driver is
  // steering.
  if (eff.mode === 'chain')
    return { hand: handFromShoulder, tip: handFromShoulder, flat: AN.v3(),
             reach, shoulder, S };

  // ---- the BLADE half, off the same live pose so the two ends of the seed
  // cannot disagree by a leaning spine's worth of voxels.
  let tipFromShoulder = handFromShoulder, flat = AN.v3();
  const blade = bladeSegmentModel();
  if (blade) {
    // Expressed RELATIVE TO THE HAND and then added to the hand offset the
    // inverse already produced (mob.cpp:8250), rather than re-running the
    // shoulder subtraction.
    const rel = AN.vmul(AN.vsub(blade.tip, handModel), 1 / S);
    tipFromShoulder = AN.vadd(handFromShoulder, rel);
    flat = blade.flat;
  }
  return { hand: handFromShoulder, tip: tipFromShoulder, flat, reach, shoulder, S };
}

/**
 * The held item's cutting edge in the editor's MODEL space (file voxels), off
 * the item's own live transform — the same derivation updateHiltBox() uses for
 * the hilt box, so the segment the driver measures and the segment the author
 * sees drawn cannot drift.
 *
 * The sidecar authors `from`/`to` as DISTANCES along `axis` in item micro
 * units, in the art's own Z-up frame; melee.cpp:374 maps that to the engine's
 * Y-up with (x, z, -y) and does NOT rebase on the model origin (they already
 * lie on it). Both are reproduced here.
 */
function bladeSegmentModel() { return itemSegmentModel(itemSc?.edge); }

/**
 * The held item's HAFT (sidecar `haft`, item.h ItemDef::hasHaft): the weak
 * second striking segment -- a mace's stick -- through exactly the same map
 * as the edge. Only while the held item is what is swinging; a fist style
 * with a mace in the other hand has no haft in the blow.
 */
function haftSegmentModel() {
  if (resolveEffector().mode !== 'held') return null;
  return itemSegmentModel(itemSc?.haft);
}

/** One authored item segment (`edge` or `haft`) in MODEL space, file voxels. */
function itemSegmentModel(e) {
  const xf = heldItemTransform();
  if (!xf || !e) return null;
  const ax = Array.isArray(e.axis) && e.axis.length === 3
    ? { x: +e.axis[0] || 0, y: +e.axis[1] || 0, z: +e.axis[2] || 0 }
    : { x: 0, y: 0, z: 1 };
  const axE = AN.vnorm(AN.v3(ax.x, ax.z, -ax.y));      // Z-up -> Y-up
  // WHERE ACROSS THE BLADE (melee.cpp `line`): a point in the art's Z-up scene
  // units, mapped and rebased exactly as hiltEngine() maps the hilt, then
  // projected off the edge axis. Without it the segment rides the model's
  // origin line -- the box corner -- and the trail drew beside the steel
  // while the engine's hitbox ran through it.
  let lineOff = AN.v3();
  if (Array.isArray(e.line) && e.line.length === 3) {
    const l = e.line.map(Number);
    const m = itemDoc?.models?.find(x => x.name === (itemSc.model || heldItemId)) ||
              itemDoc?.models?.[0];
    const sm = m?.sceneMin || { x: 0, y: 0, z: 0 };
    const off = m?.offset || { x: 0, y: 0, z: 0 };
    const o = AN.v3(l[0] - sm.x + off.x, l[2] - sm.y + off.y,
                    -l[1] - (sm.z - 1) + off.z);
    lineOff = AN.vsub(o, AN.vmul(axE, AN.vdot(axE, o)));
  }
  const at = d => {
    const p = AN.qrot(xf.quat, AN.vmul(AN.vadd(AN.vmul(axE, d), lineOff), xf.scale));
    return AN.vadd(xf.pos, p);
  };
  const seg = { base: at(+e.from || 0), tip: at(+e.to || 0) };
  // THE FLAT, through the same map. A DIRECTION, so no scale; orthogonalized
  // against the edge because two separately-authored axes that are
  // nearly-but-not-quite perpendicular give the roll a slow shear.
  let flat = AN.v3();
  if (Array.isArray(e.flat) && e.flat.length === 3) {
    const f = { x: +e.flat[0] || 0, y: +e.flat[1] || 0, z: +e.flat[2] || 0 };
    let fe = AN.v3(f.x, f.z, -f.y);
    fe = AN.vsub(fe, AN.vmul(axE, AN.vdot(axE, fe)));
    if (AN.vlen(fe) > 1e-4) flat = AN.qrot(xf.quat, AN.vnorm(fe));
  }
  seg.flat = flat;
  return seg;
}

/**
 * mob.cpp:8262 HeadKeepOut — the wielder's own head, so no authored windup
 * sweeps through it. Shoulder-relative, WORLD voxels, off the LIVE pose.
 */
function headKeepOut() {
  if (wpHeadPart < 0 || wpChain < 0 || !anim?.model?.length) return null;
  const m = anim.model[wpHeadPart];
  const model = skel.parts[wpHeadPart]._model;
  if (!m || !model) return null;
  const S = rigScale();
  // Half the model box. The engine divides by the SKIN scale because its model
  // space is world voxels; here the box is already in file voxels and the
  // whole result is converted once, at the end.
  const half = AN.v3(model.dim.x * 0.5, model.dim.y * 0.5, model.dim.z * 0.5);
  const center = AN.vadd(m.pos, AN.qrot(m.rot, half));
  const rel = AN.vsub(center, anim.model[skel.chains[wpChain].parts[0]].pos);
  return {
    center: AN.vmul(rel, 1 / S),
    radius: Math.max(half.x, Math.max(half.y, half.z)) / S,
  };
}

/** The WeaponPose the rig consumes, or null when nothing claims the arm. */
function weaponPose() {
  if (!melee || wpChain < 0) return null;
  // A HELD FRAME THAT IS A POSE (melee.js keyedFramePose): the frame, still,
  // with nothing driving the stroke underneath it.
  if (heldKeyed) {
    return { hand: AN.v3(), bladeDir: AN.v3(0, 1, 0), bladeFlat: AN.v3(0, 0, 1),
             bendPole: AN.v3(0, 0, -1), weight: 1, release: -1, steerBlade: false,
             steerAmount: 0, torsoTwist: heldKeyed.twist, torsoPitch: heldKeyed.pitch,
             keyed: heldKeyed };
  }
  const p = melee.pose();
  if (!(p.weight > 0)) return null;
  // A KEYED STROKE (melee.js strokeKeyedPose): the joints and the torso are
  // the frames', and the driver's arm is not used.
  const lib = strokeLive() ? ATK.library() : null;
  const sty = lib && strokeStyle >= 0 ? lib.styles[strokeStyle] : null;
  const kp = sty ? MELEE.strokeKeyedPose(strokeCur, sty, p.release) : null;
  if (kp) {
    p.keyed = kp.on ? kp : null;
    p.torsoTwist = kp.twist;
    p.torsoPitch = kp.pitch;
  }
  return p;
}
// Set by weaponTick while a goal frame with a pose is held (see there).
let heldKeyed = null;

/**
 * mob.cpp:1895 — the ORDER matters and is the engine's: seed the driver from
 * where the blade actually is, THEN advance the program, so the pose the
 * animation pass reads this tick is the one the program just produced.
 *
 * Called from stepPreview BEFORE animSampleAndBlend, on the sim's own 30 Hz
 * tick rather than the browser's frame — a stroke authored in ticks must be
 * previewed in ticks or every duration in the panel is a lie.
 */
let strokeAccum = 0;
function weaponStep(dtFrame) {
  if (!melee || wpChain < 0) return;
  strokeAccum += dtFrame;
  // Bounded catch-up: a backgrounded tab must not run four hundred ticks in
  // one frame and finish the swing before it is drawn.
  let steps = Math.min(Math.floor(strokeAccum / kStrokeDt), 4);
  strokeAccum -= steps * kStrokeDt;
  while (steps-- > 0) weaponTick();
}

// Counters, for the harness. A stroke that does not advance has at least four
// causes — the preview loop is not running, weaponStep is bailing on a missing
// arm, the program has no style to read, or the driver is refusing the input —
// and from outside all four are the same frozen phase (CLAUDE.md rule 6).
let dbgPreviewSteps = 0, dbgWeaponTicks = 0, dbgNoStyle = 0;
const dbgTouchdowns = {};

/**
 * Tell the driver what arm and blade it has, from the CURRENT pose.
 *
 * Split out of weaponTick because it is also the answer to "what would this
 * stroke do" on a rig that is standing still. `bladeLen_` and therefore the
 * whole reach band live inside the MeleeState and are only written here, so
 * before this was called at rest the Attacks readout showed a 0-voxel blade
 * and a bare-arm band on a rig that was visibly holding a sword — and the
 * `reach` field of an authored segment, which is a position in that band,
 * was being previewed against the wrong annulus.
 */
function weaponSeedFromRig() {
  if (!melee) return;
  const seed = weaponArmSeed();
  if (seed) melee.setStroke(seed.hand, seed.tip, seed.flat, seed.reach);
  else melee.clearArm();
  // ---- THE SPHERE IS FOR A BLADE, AND ONLY FOR A BLADE (mob.cpp:15570) ---
  //
  // What the keep-out models is a LONG RIGID SEGMENT swung about the shoulder:
  // a held sword is a child of the hand, its far end is a metre from any
  // joint, and no pose limit in the rig knows it exists. A natural weapon's
  // segment IS the part — a voxel of knuckle on an arm whose shoulder cone and
  // elbow hinge are already clamped every tick — and a sphere sized off the
  // head model is wider than the whole fist, so left on it shoves a chambered
  // punch (which legitimately sits beside the chin) out and up. And when the
  // face IS the weapon there is no radius at which "keep the jaws away from
  // the head" means anything at all.
  const keep = resolveEffector().mode === 'held' ? headKeepOut() : null;
  if (keep) melee.setKeepOut(keep.center, keep.radius);
  else melee.clearKeepOut();
}

/* --------------------------------------------------------------------------
   THE LUNGE, AS A TIMELINE THE AUTHOR CAN LINE UP (strokes.h StyleLunge)

   The engine's launch (Mob::Launch) hands the BODY a velocity and UpdateFall
   carries it, wall test and all. The preview has no world, no footing probe
   and no victim to be a wall, so it does NOT re-implement that: what it
   reproduces is the arc's own arithmetic — the magnitude the engine solves at
   the moment of the launch, and the ballistic flight that follows from the
   authored rise — and it translates the previewed body along it.

   WHY THAT IS THE USEFUL HALF. Lining the landing up with the cut is the
   AUTHOR'S job (strokes.h says so in as many words: "windup.ticks ~ ticks"),
   and the thing they cannot see from the JSON is that those two numbers are
   measured on different clocks — `lunge.ticks` is what the magnitude is
   BUDGETED against, while the flight actually ends when gravity brings the
   rise back to the ground. The panel prints both against the tick the cut
   starts on, which is the number to move.

   THE STAND-OFF IS THE STYLE'S OWN `reach`, because that is the distance the
   creature commits from: MobSystem::BeginStroke refuses a style whose reach
   the target is outside, so a lunging bite authored at 22 voxels launches from
   at most 22. The arrival distance is the EFFECTOR's reach and not the style's
   — the two mean opposite things and confusing them makes the lunge a no-op
   (mob.cpp:3697 measured every launch computing `dist - 22 <= 0` and produced
   a creature that hopped on the spot).
   -------------------------------------------------------------------------- */
let lungeArc = null;

/** The loco state the preview is in, and what it scales a leap by. */
function lungeStateScale() {
  if (!skel || !anim) return { scale: 1, state: '' };
  const r = AN.animSelectState(skel, anim);
  if (r < 0 || !skel.states?.[r]) return { scale: 1, state: '' };
  const rule = skel.states[r];
  return { scale: Math.max(0, rule.lungeScale ?? 1), state: rule.name || '' };
}

/**
 * Solve the arc one style would fly, without flying it. Pure: the panel calls
 * it to print the timeline on a rig that is standing still.
 */
function lungeSolve(sty) {
  if (!sty || !MELEE.lungeAny(sty.lunge)) return null;
  const t = tuningDoc();
  // physics.gravity is metres/s^2 and the arc is in WORLD voxels, so it goes
  // through the same MetresToCells every other authored length does.
  const g = (Number.isFinite(+t?.physics?.gravity) ? +t.physics.gravity : 9.8)
            / MELEE.kVoxelMeters;
  const seed = weaponArmSeed();
  const armReach = (seed?.reach ?? 0) > 1e-3 ? seed.reach
                                             : 0.60 / MELEE.kVoxelMeters;
  const { scale, state } = lungeStateScale();
  const standOff = sty.reach > 0 ? sty.reach : armReach * 2;
  const airTime = Math.max(1, sty.lunge.ticks) / 30;
  const want = (standOff - armReach) / airTime;
  const mag = Math.min(sty.lunge.speed / MELEE.kVoxelMeters * scale,
                       Math.max(0, want));
  const rise = sty.lunge.rise / MELEE.kVoxelMeters * scale;
  // A BALLISTIC FLIGHT ENDS WHERE IT STARTED: the preview ground is the body's
  // own launch height, so the flight is 2*rise/g — plus the one tick
  // UpdateFall's launch latch buys, which is why a lunge with no rise lands
  // the tick after it left (strokes.h, and test_melee.mjs asserts every
  // shipped lunge climbs).
  const landTicks = Math.max(1, Math.round(2 * rise / Math.max(g, 1e-6) * 30));
  return { mag, rise, g, scale, state, standOff, armReach,
           budget: Math.max(1, sty.lunge.ticks), landTicks,
           at: sty.lunge.at, travel: mag * landTicks / 30 };
}

/**
 * ONE DELIBERATE DIFFERENCE FROM THE ENGINE. There, the effector is chosen at
 * BeginStroke and cleared at the end of the stroke — a creature is only ever
 * holding a weapon or swinging one. Here the panel is an AUTHORING surface:
 * clicking the `bite` chip has to move the readout onto the head, draw the
 * jaws' edge and report the head's own reach band BEFORE anything swings, or
 * every number under the author's cursor describes a different weapon from the
 * one being edited.
 *
 * So between strokes the effector follows the SELECTION. A live stroke owns it
 * outright, exactly as the engine's does.
 *
 * CALLED FROM tick(), NOT ONLY FROM weaponTick — the preview loop only runs
 * when something wants a posed rig (a gait, an open clip, a live stroke), and
 * the common case for reading the panel is a rig standing still with none of
 * those. Driving it off the preview meant selecting a bite changed nothing
 * until you pressed swing.
 */
function syncEffectorToSelection() {
  if (strokeLive()) return;
  const lib = ATK.library();
  const sel = lib ? lib.styles[ATK.styleIndex()] : null;
  if (!sel || !armForStyle(sel)) clearStrikeEffector();
}

function weaponTick() {
  // A solo burst beginStroke deferred to this tick, now that the rig has been
  // posed at rest once (see beginStroke).
  if (strokeBurst) {
    const solo = strokeBurst;
    strokeBurst = null;
    runStrokeBurst(solo);
    return;
  }
  dbgWeaponTicks++;
  syncEffectorToSelection();
  // ---- 1. WHERE THE BLADE IS NOW ---------------------------------------
  weaponSeedFromRig();
  captureRestSeed();

  // ---- 2. the basis. Heading is 0 and the rig is authored in its own frame,
  // so this is the rig's facing basis: fwd = +Z, up = +Y, and right = fwd x up
  // = -X, which is the sign anim.cpp:404 states and the side every def's `.R`
  // limbs are authored on.
  const right = AN.v3(-1, 0, 0), up = AN.v3(0, 1, 0), fwd = AN.v3(0, 0, 1);

  // ---- 3. drive the phase — the SHARED runner (editor/melee.js) ---------
  const lib = ATK.library();
  const sty = (lib && strokeStyle >= 0) ? lib.styles[strokeStyle] : null;
  if (!sty) dbgNoStyle++;
  // ---- 3a. THE GOAL FRAME, held still (attacks.js's goal chips) ----------
  //
  // The arm is held where the PROGRAM ARRIVES at the end of that frame
  // (melee.js runStrokeToFrame): the style run from rest, with this swing's
  // jitter draw, stopped on the frame's last tick — the same pose a solo of
  // that segment freezes on. It used to be the AUTHORED destination with every
  // eased channel settled, which a chase windup never reaches and which leaned
  // the blade the other way, so "windup" and a solo'd windup were two arms.
  // The authored target is drawn beside it (pushGoalMarker).
  //
  // Re-run every tick rather than once on the click, so nudging the aim
  // sliders or an az/el box moves the held frame under the cursor. It reads
  // the SELECTED style, not `strokeStyle`: nothing is live.
  const goalKey = ATK.goalFrame?.();
  heldKeyed = null;
  if (goalKey) {
    const gi = ATK.styleIndex();
    const gsty = lib ? lib.styles[gi] : null;
    const rest = restFor();
    const gaim = aimAtRest();
    // A FRAME THAT IS A POSE is simply shown: nothing to run, nothing to
    // arrive at. The driver is let go so nothing else claims the arm.
    const gk = typeof goalKey === 'string' && goalKey[0] === 'f' ? +goalKey.slice(1) : -1;
    const kp = gsty ? MELEE.keyedFramePose(gsty, gk, gaim.az, gaim.el) : null;
    if (kp) {
      heldKeyed = kp;
      melee.reset();
      goalMarkerSegs = null;
      return;
    }
    let arrived = null;
    if (gsty && rest) {
      // Take over from the REST arm a solo starts from, not the rig's current
      // arm — which is this held frame, and would feed the run its own output.
      melee.tuning = meleeTuning();
      melee.setStroke(rest.hand, rest.tip, rest.flat, rest.reach);
      arrived = MELEE.runStrokeToFrame(gsty, gi, strokeSeed(), melee, goalKey,
                                       gaim.az, gaim.el, kStrokeDt, right, up, fwd,
                                       gaim.dist);
    }
    // No rest arm for THIS effector yet (the style changed while a frame was
    // held): let go for a tick so the rig poses at rest and the seed is taken.
    if (!arrived) melee.reset();
    pushGoalMarker(gsty, goalKey, gaim, rest);
    return;
  }
  goalMarkerSegs = null;
  if (!strokeLive()) { melee.update(kStrokeDt, false, !!sty, right, up, fwd); return; }
  // SOLO HOLD: the segment under study has ended; keep the pose it ended in
  // on screen (no step, so the driver's pose is unchanged) for a beat, then
  // run the program again from the start.
  if (strokeHoldTicks > 0) {
    if (--strokeHoldTicks === 0) {
      // ONLY `vary` ADVANCES THE DRAW. See the loop exit below.
      if (ATK.looping()) { if (ATK.varying?.()) ATK.nextSwing();
                           beginStroke(strokeStyle); }
      else stopStroke();
    }
    return;
  }
  // The target's bearing from WHERE THE SHOULDER IS NOW, every tick, as
  // mob.cpp re-aims from StrokePivotWorld each tick.
  const aim = aimLive();
  const P = MELEE.STROKE_PHASE;
  const phaseBefore = strokeCur.phase;
  // ---- 3b. THE LUNGE, fired on the FIRST TICK OF ITS NAMED PHASE and before
  // the program advances (mob.cpp:3678), so a windup lunge leaves the ground on
  // the same tick the telegraph starts and the flight IS the telegraph.
  if (sty && !lungeArc && strokeCur.phaseTick === 0 &&
      ((sty.lunge.at === 'windup' && strokeCur.phase === P.Windup) ||
       (sty.lunge.at === 'cut' && strokeCur.phase === P.Cut))) {
    const sol = lungeSolve(sty);
    if (sol)
      lungeArc = { ...sol, x: 0, y: 0, vy: sol.rise, airTicks: 0,
                   landed: false, cutStart: -1 };
  }
  if (lungeArc && !lungeArc.landed) {
    // Forward along the rig's own facing (+Z at heading 0) and up, integrated
    // on the STROKE's 30 Hz clock — the same one Mob::UpdateFall runs on.
    lungeArc.x += lungeArc.mag * kStrokeDt;
    lungeArc.y += lungeArc.vy * kStrokeDt;
    lungeArc.vy -= lungeArc.g * kStrokeDt;
    lungeArc.airTicks++;
    if (lungeArc.airTicks > 1 && lungeArc.y <= 0) {
      lungeArc.y = 0;
      lungeArc.landed = true;
    }
  }
  if (lungeArc && lungeArc.cutStart < 0 && strokeCur.phase === P.Cut)
    lungeArc.cutStart = strokeTick;
  const r = MELEE.stepStrokeProgram(strokeCur, sty, melee, aim.az, aim.el,
                                    kStrokeDt, right, up, fwd, aim.dist);
  strokeTick++;
  if (strokeTick === 1)
    strokeStart = { az: melee.strokeAz(), el: melee.strokeEl(), radius: melee.strokeRadius() };
  if (phaseBefore === P.Windup && strokeCur.phase === P.Cut)
    strokeCommit = { dAz: strokeCur.wantAz - melee.strokeAz(),
                     dEl: strokeCur.wantEl - melee.strokeEl(),
                     dR: strokeCur.wantReach - melee.strokeRadius() };
  const solo = ATK.soloSegment?.() || 'all';
  if (solo !== 'all') {
    const want = solo === 'windup' ? P.Windup : solo === 'cut' ? P.Cut : P.Recover;
    if (r === MELEE.STEP.Finished || strokeCur.phase > want) {
      // Past the segment: freeze here. The cursor is left where it is so the
      // readout still names the phase that just ended.
      strokeHoldTicks = kSoloHoldTicks;
      return;
    }
  }
  if (r === MELEE.STEP.Finished) {
    // ---- THE LOOP REPLAYS THE SAME SWING unless `vary` is on (2026-09-21).
    // It used to call nextSwing() here unconditionally, so every cycle drew a
    // new seed: a different start bow (up to 0.24 rad on the shipped NPC
    // styles) and different post-tempo tick counts (±0.28). Authoring against
    // that is authoring against a moving target — the segment you are watching
    // changes shape once a cycle and nothing on screen says why, which is the
    // "solo gives different animations randomly" report. `reroll` and the
    // `vary` chip both still step the sequence, deliberately.
    if (ATK.looping()) { if (ATK.varying?.()) ATK.nextSwing();
                         beginStroke(strokeStyle); }
    else stopStroke();
  }
}

/**
 * Start one swing. Mirrors MobSystem::BeginStroke: reset the container, re-tune
 * the MeleeState (tuning.json hot-reloads and a swing must use the current
 * values), then fill the program fields.
 *
 * THE SEED IS `wielder ^ salt ^ startTick` in the engine; here it is the swing
 * NUMBER, so "swing again" walks the same deterministic jitter sequence the
 * game would and the author can see all of it rather than one draw.
 */
function beginStroke(styleIndex) {
  const lib = ATK.library();
  if (!melee || !lib) return;
  const sty = lib.styles[styleIndex];
  if (!sty) return;
  strokeStyle = styleIndex;
  // ---- POINT THE DRIVER AT WHATEVER THIS STYLE SWINGS (mob.cpp:3327) -----
  // MobSystem::BeginStroke calls ArmForStyle here, before the program is
  // seeded, and refuses the swing when the creature has not got the weapon.
  // The preview refuses it the same way rather than running a stroke with no
  // edge on the end of it — a style previewed against a rig that does not
  // declare its natural weapon is a content error, and showing a plausible
  // swing for it is how one ships.
  if (!armForStyle(lib.styles[styleIndex])) {
    strokeStyle = -1;
    toast('"' + (sty.weapon || 'held') + '" is not on this rig — add it in the ' +
          'Natural weapons block, or pick a style this creature can swing', true);
    return;
  }
  strokeCur = MELEE.newStrokeCursor();
  melee.tuning = meleeTuning();
  melee.reset();
  strokeTick = 0;
  lungeArc = null;
  strokeTrail.length = 0;
  strokeTipPrev = null;
  strokeTipSpeed = 0;
  strokeEdgeAlign = 1;
  // Knuth's multiplicative hash of the swing number, standing in for the
  // engine's `wielder ^ salt ^ startTick`: any injective map does, because
  // what the seed has to be is DIFFERENT PER SWING and reproducible, not any
  // particular value.
  MELEE.beginStrokeProgram(strokeCur, sty, styleIndex, strokeSeed());
  strokeAccum = 0;
  strokeStart = null;
  strokeCommit = null;
  strokeHoldTicks = 0;
  // mob.cpp BeginStroke calls PlayClip for the body animation. Start the
  // style's clip so the rest of the body (torso lean, off-hand, step) plays
  // alongside the weapon arm.
  strokeBodyClipIdx = -1;
  if (sty.clip && skel) {
    const ci = skel.clips.findIndex(c => c.name === sty.clip);
    if (ci >= 0) {
      strokeBodyClipIdx = ci;
      anim.clips = anim.clips.filter(i => i.clip !== ci);
      anim.clips.push({ clip: ci, timeMs: 0, ageMs: 0, rate: 1,
                         weight: 1, stopping: false, fade: 1 });
    }
  }
  // SOLO cut / recover: BURST through the segments before the one under
  // study, so the preview opens on that segment's first tick. The burst runs
  // the same ticks the engine would — seed from the rig, step the program —
  // it just does not wait a frame between them, so the rig pose the seed
  // reads is one frame stale for the burst's duration, which is the engine's
  // own one-tick latency and not a different swing.
  //
  // DEFERRED ONE TICK, so every replay starts from the SAME pose. The driver
  // seeds from wherever the rig's arm is (the take-over), and a loop replay
  // is begun from inside the solo HOLD, where the rig is still frozen at the
  // end of the last segment. Bursting right here seeded replay 2+ from the
  // end of the previous cut while the first swing (clicked from rest) seeded
  // from rest, and a windup that does not quite reach its pose then gives
  // two different cuts in alternation — "two different strikes with jitter
  // at 0". One preview step at weight 0 (melee.reset above) puts the arm back
  // at rest, and the burst runs from there on the next tick.
  const solo = ATK.soloSegment?.() || 'all';
  strokeBurst = (solo === 'cut' || solo === 'recover') ? solo : null;
  // The joint brakes' memory is per swing here for the same reason: a replay
  // must not start braking from where the LAST swing left the arm.
  armSmooth.valid = false;
  armSmooth.keyLive = null;
}
let strokeBurst = null;

// The swing number's seed (see beginStroke) — shared with the held goal frame
// so a goal and a solo of the same swing draw the same jitter.
const strokeSeed = () => Math.imul(ATK.swingNumber() + 1, 2654435761) >>> 0;

/* --------------------------------------------------------------------------
   THE REST ARM A HELD GOAL FRAME TAKES OVER FROM

   runStrokeToFrame replays the program from a take-over, and the take-over
   reads the arm (weaponArmSeed). While a frame is held the rig's arm IS that
   frame, so seeding from it would feed the run its own output and the pose
   would creep. Instead the seed is remembered from the last pose the rig was
   given with NOTHING driving the arm — the pose a solo starts from — keyed by
   the effector and the held item, because a punch's fist and a sword's point
   are different arms. `lastPoseDriven` is set where the pose is built (stage
   3.5b), since anim.model is the PREVIOUS frame's pose when weaponTick runs.
   -------------------------------------------------------------------------- */
let restSeed = null;
let lastPoseDriven = false;
let goalMarkerSegs = null;  // the authored target, drawn with the trail
function restSeedKey() {
  const e = resolveEffector();
  return `${e.part}|${e.mode}|${e.natural}|${wpHandPart}|${heldItemId || ''}`;
}
function captureRestSeed() {
  if (lastPoseDriven || !melee || melee.poseWeight() > 0) return;
  const s = weaponArmSeed();
  if (!s) return;
  restSeed = { key: restSeedKey(), hand: { ...s.hand }, tip: { ...s.tip },
               flat: { ...s.flat }, reach: s.reach,
               // for the aim orb: where the pivot stands and the neutral
               // reach, AT REST, so the target does not ride the swing
               shoulder: { ...s.shoulder }, S: s.S,
               neutral: MELEE.strokeReachIn(melee, 0),
               head: headCenterScene() };
}
const restFor = () => (restSeed && restSeed.key === restSeedKey() ? restSeed : null);

/** The head's centre in scene (model) space, or null on a rig with no head. */
// `anim.model[i].pos` is the part's ANCHOR (the joint it turns about), not a
// corner of its box: the box centre is the model's own `offset + dim/2` in
// file voxels, carried by the part's delta from rest exactly as
// modelTransform() carries the drawn voxels.
function headCenterScene() {
  if (wpHeadPart < 0 || !anim?.model?.length) return null;
  const posed = anim.model[wpHeadPart];
  const model = skel.parts[wpHeadPart]._model;
  if (!posed || !model || !model.offset || !model.dim) return null;
  const c = AN.v3(model.offset.x + model.dim.x * 0.5, model.offset.y + model.dim.y * 0.5,
                  model.offset.z + model.dim.z * 0.5);
  const rest = restModel?.[wpHeadPart];
  if (!rest) return c;
  const dq = AN.qnorm(AN.qmul(posed.rot, AN.qconj(rest.rot)));
  return AN.vadd(posed.pos, AN.qrot(dq, AN.vsub(c, rest.pos)));
}

/* --------------------------------------------------------------------------
   THE TARGET IS A POINT (attacks.js `aim`).

   The panel states the target as the character sees it — a bearing from the
   EYES and a distance — and it is turned into a point here, fixed in the
   world: placed from the head AT REST, so it does not ride the swing or fly
   with a lunge. The program is then handed that point's bearing and distance
   about the stroke's own pivot (strokes.cpp StrokeAimAt), which is what the
   engine does for an NPC's `targetPoint` and a player's look ray. So az/el
   0/0 is straight ahead of the FACE, and the shoulder's aim comes out a little
   across the body, the way a real target in front of you is.
   -------------------------------------------------------------------------- */
function aimEyeScene() {
  const rest = restFor();
  if (rest && rest.head) return rest.head;
  const h = headCenterScene();
  if (h) return h;
  const live = weaponArmSeed();
  return rest ? rest.shoulder : live ? live.shoulder : null;
}
function aimTargetScene() {
  const a = ATK.currentAim();
  const eye = aimEyeScene();
  if (!eye) return null;
  const S = restFor()?.S || rigScale();
  const d = AN.v3(-Math.cos(a.el) * Math.sin(a.az), Math.sin(a.el),
                  Math.cos(a.el) * Math.cos(a.az));
  return AN.vadd(eye, AN.vmul(d, Math.max(0.5, a.dist ?? 6) * S));
}
/** The target's bearing + distance (world voxels) about a SCENE pivot. */
function aimFrom(pivot, S) {
  const t = aimTargetScene();
  if (!t || !pivot) return { az: 0, el: 0, dist: 0 };
  const to = AN.vsub(t, pivot);
  // The rig's facing basis at heading 0: right = -X, up = +Y, fwd = +Z.
  const x = -to.x / S, y = to.y / S, z = to.z / S;
  const r = Math.max(Math.hypot(x, y, z), 1e-4);
  return { az: Math.atan2(x, z), el: Math.asin(Math.max(-1, Math.min(1, y / r))), dist: r };
}
/** The aim the program gets from the REST pivot (a held goal, the readout). */
function aimAtRest() {
  const rest = restFor();
  if (rest) return aimFrom(rest.shoulder, rest.S || rigScale());
  const live = weaponArmSeed();
  return live ? aimFrom(live.shoulder, live.S || rigScale()) : { az: 0, el: 0, dist: 0 };
}
/** ...and from the LIVE pivot, lunge included (a swing in progress). */
function aimLive() {
  const live = weaponArmSeed();
  if (!live) return aimAtRest();
  return aimFrom(AN.vadd(live.shoulder, lungeOffsetModel()), live.S || rigScale());
}

/**
 * The AUTHORED target of the held frame, beside the arm that arrived: a cross
 * at the target tip and a line from where the tip actually got to. A chase
 * frame that runs out of ticks shows as a gap; a frame that arrives shows the
 * cross on the point.
 */
function pushGoalMarker(sty, key, aim, rest) {
  goalMarkerSegs = null;
  const seed = sty && rest ? weaponArmSeed() : null;
  const g = seed ? MELEE.strokeGoalPose(sty, melee, key, aim.az, aim.el) : null;
  if (!g) return;
  const S = seed.S || rigScale();
  const sh = AN.vadd(seed.shoulder, lungeOffsetModel());
  const sc = v => [sh.x - v.x * S, sh.y + v.y * S, sh.z + v.z * S];
  const tgtL = AN.vmul(dirL(g.az, g.el), g.reach);
  const t = sc(tgtL), a = sc(melee.tipL_);
  const c = 0.45 * S, C = 0xffc857;
  goalMarkerSegs = [
    { a: [t[0] - c, t[1], t[2]], b: [t[0] + c, t[1], t[2]], color: C, alpha: 1 },
    { a: [t[0], t[1] - c, t[2]], b: [t[0], t[1] + c, t[2]], color: C, alpha: 1 },
    { a: [t[0], t[1], t[2] - c], b: [t[0], t[1], t[2] + c], color: C, alpha: 1 },
    { a, b: t, color: 0xffffff, alpha: 0.45 },
  ];
}

/** The solo burst beginStroke deferred (see there). */
function runStrokeBurst(solo) {
  const P = MELEE.STROKE_PHASE;
  const want = solo === 'cut' ? P.Cut : P.Recover;
  for (let guard = 0; guard < 400 && strokeLive() && strokeCur.phase < want &&
                      strokeHoldTicks === 0; guard++)
    weaponTick();
}

function stopStroke() {
  strokeCur = MELEE.newStrokeCursor();
  strokeBurst = null;
  // mob.cpp:14542 ClearStrikeEffector — DROPS THE OVERRIDE, and that is all:
  // a rig that finishes a punch while still holding a sword goes back to
  // carrying the sword through resolveEffector's fall-through, not through a
  // value written here.
  clearStrikeEffector();
  lungeArc = null;
  strokeHoldTicks = 0;
  if (strokeBodyClipIdx >= 0) {
    for (const inst of anim.clips)
      if (inst.clip === strokeBodyClipIdx) inst.stopping = true;
    strokeBodyClipIdx = -1;
  }
  if (melee) melee.reset();
  strokeTrail.length = 0;
  strokeTipPrev = null;
  strokeAccum = 0;
  ed.setStrokeTrail?.(null);
}

/**
 * mob.cpp:7767 Mob::ApplyWeaponArm — stage 5.5.
 *
 * Returns the PoseAxisOverride for the stage-6 clamp (anim.h), or null. Only
 * the pieces the editor can honour are here: the IK target, the steered pole,
 * and the elbow's hinge plane. What is NOT ported is the diagnostic block
 * (WeaponArmDiag) and the wrist alignment, which needs the item's own
 * transform chain rather than the rig's.
 */
function applyWeaponArm() {
  const pose = weaponPose();
  if (!pose || !anim?.model?.length) return null;
  const eff = resolveEffector();
  // ---- AIM: A PART WITH NO CHAIN (mob.cpp:14915, stage 0) ---------------
  //
  // The jaws are not on the end of anything the two-bone solver can serve, so
  // an aim effector is driven the other way a part can be pointed at
  // something — and it is applied EARLIER, in applyStrikeAim, because it
  // writes the PRE-FLATTEN locals (it rotates a part about its joint and
  // shares the yaw with the spine above it). This stage runs after the
  // flatten, so an aim written from here would be a write nobody reads.
  if (eff.mode === 'aim') return null;
  // mob.cpp: THE RELEASE IS NOT SOLVED -- smoothWeaponArm turns the joints
  // from the last driven pose to the animation's (WeaponPose::release).
  if (pose.release >= 0) return null;
  // ...and NEITHER IS A KEYED POSE: smoothWeaponArm writes its joints.
  if (pose.keyed) return null;
  const hand = { hand: -1 };
  const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
  if (ci < 0 || !skel.chains[ci]) return null;
  const ch = skel.chains[ci];
  const weight = ch.weight * pose.weight;
  if (weight <= 0) return null;
  const S = rigScale();
  const i0 = ch.parts[0], i1 = ch.parts[1];

  // THE LIVE SHOULDER, not the rest anchor: the spine leans and the pelvis
  // bobs, so the real shoulder wanders a couple of voxels from its authored
  // anchor every stride, and AnimSolveTwoBone measures its own reach from the
  // live joint. Both ends must use the same one (mob.cpp:7820).
  const shoulder = anim.model[i0].pos;
  // Heading 0 => toRig is the identity. World voxels back to file voxels here,
  // which is the only place the conversion happens on this side.
  const targetLocal = AN.vadd(shoulder, AN.vmul(pose.hand, S));

  const steer = { ...ch };
  if (pose.steerBlade && AN.vlen(pose.bendPole) > 1e-4) steer.pole = pose.bendPole;
  AN.animSolveTwoBone(skel, anim, steer, targetLocal, weight);
  // ---- THE HAND CARRIES THE BLADE (mob.cpp ApplyWeaponArm §3) -------------
  // Ported 2026-09-25: until then this preview never turned the wrist, so the
  // blade rode the forearm's grip angle while the game laid it along the
  // stroke — the preview and the game were two different swings, and a pose
  // BAKED from the preview (attacks.js bakeStyle) missed the targets the game's
  // swing used to hit.
  if (pose.steerBlade && eff.mode === 'held')
    steerWristToBlade(pose, hand.hand >= 0 ? hand.hand : wpHandPart, weight);

  if (!pose.steerBlade) return null;
  // ---- the elbow's hinge plane follows the bend (mob.cpp:7850) ----------
  const fore = skel.parts[i1];
  const forePar = fore.parent;
  if (!fore.hasPoseLimit || !fore.poseHinge || forePar < 0) return null;
  // THE PLANE FROM THE SOLVER'S OWN INPUTS, not from the bones it produced: at
  // a nearly straight elbow `upper x lower` is short and its direction is
  // almost pure noise, so the clamp would project the forearm onto a plane
  // chosen at random. The pole and the shoulder-to-target line are well
  // conditioned at every bend.
  const toTarget = AN.vsub(targetLocal, anim.model[i0].pos);
  let n = AN.v3();
  if (AN.vlen(toTarget) > 1e-4) {
    const dirTarget = AN.vnorm(toTarget);
    const pole = AN.vnorm(steer.pole);
    const polePerp = AN.vsub(pole, AN.vmul(dirTarget, AN.vdot(dirTarget, pole)));
    if (AN.vlen(polePerp) > 1e-4) n = AN.vcross(AN.vnorm(polePerp), dirTarget);
  }
  if (AN.vlen(n) <= 1e-3) return null;
  n = AN.vnorm(n);
  // Into the frame the clamp states its axis in: parent-relative, then
  // rest-relative.
  const frame = AN.qmul(anim.model[forePar].rot, fore.rest.rot);
  let axis = AN.qrotinv(frame, n);
  // AND ITS SIGN, MEASURED WITH THE CLAMP'S OWN FUNCTION. Either sense names
  // the same plane, but a hinge's range is authored [0, 130], so an axis whose
  // sense makes the SOLVED bend negative is clamped straight to zero — a
  // 70-degree correction that throws the arm across the room. Matching the
  // authored axis instead chooses the wrong half every time the pole swings
  // round (mob.cpp:7918).
  {
    const par = anim.model[forePar];
    const delta = AN.qmul(AN.qconj(fore.rest.rot),
                          AN.qmul(AN.qconj(par.rot), anim.model[i1].rot));
    const bend = AN.animHingeAngleAbout(delta, axis);
    if (bend !== null && bend < 0) axis = AN.vmul(axis, -1);
  }
  lastElbowOverride = { part: i1, axis, blend: clamp(pose.steerAmount, 0, 1) };
  return lastElbowOverride;
}

/**
 * mob.cpp Mob::SmoothWeaponArm — THE AUTHOR'S BRAKES ON THE POSED ARM
 * (melee.h ArmSmooth, strokes.h AttackStyle::joints). Per joint (shoulder =
 * the chain's upper bone, elbow = its lower bone, wrist = the hand), the
 * rotation RELATIVE TO ITS PARENT is low-passed (`smooth`, a half-life in
 * ticks) and then capped (`maxDeg` per tick) against last tick's braked
 * value, and the arm's subtree is re-flattened so the sword rides the braked
 * hand. The brakes in force are the LIVE program's style's; after the stroke
 * ends the last ones stay on until the arm has caught up, so turning them
 * off cannot snap.
 */
let armSmooth = { valid: false, part: [-1, -1, -1], prev: [null, null, null],
                  params: null, lag: [0, 0, 0],
                  // mob.h ArmSmoothState: the joint-space release's memory.
                  lastValid: false, lastPart: [-1, -1, -1], last: [null, null, null],
                  relActive: false, relFrom: [null, null, null],
                  // mob.h ArmSmoothState::keyLive: the arm a keyed stroke took
                  // over, which its first frame blends from.
                  keyLive: null };
function liveArmJoints() {
  if (!strokeLive() || !melee || !(melee.poseWeight() > 0)) return null;
  const lib = ATK.library();
  const sty = (lib && strokeStyle >= 0) ? lib.styles[strokeStyle] : null;
  // THE CURRENT FRAME'S brakes (strokes.cpp StrokeJointsNow).
  const js = sty ? MELEE.strokeJointsNow(strokeCur, sty) : null;
  return MELEE.jointsAny(js) ? js : null;
}
/**
 * mob.cpp ApplyWeaponArm §3 — the hand oriented so the blade points along the
 * driver's bladeDir with its flat on bladeFlat: aim, then roll about the
 * blade; the turn capped at wristMaxAngle (a wrist is not a ball joint) and
 * THEN scaled by steerAmount; weighted by the arm claim. Everything hanging
 * off the hand re-flattens rigidly.
 */
function steerWristToBlade(pose, handPart, weight) {
  if (handPart < 0 || !anim.model[handPart]) return;
  const blade = bladeSegmentModel();
  if (!blade) return;
  const rigid = anim.model[handPart].rot;
  const bd = AN.vsub(blade.tip, blade.base);
  if (AN.vlen(bd) < 1e-4) return;
  const bladeHand = AN.qrotinv(rigid, AN.vnorm(bd));
  let flatHand = AN.vlen(blade.flat) > 1e-4 ? AN.qrotinv(rigid, blade.flat) : AN.v3();
  flatHand = AN.vsub(flatHand, AN.vmul(bladeHand, AN.vdot(bladeHand, flatHand)));
  const haveFlat = AN.vlen(flatHand) > 1e-4;
  if (haveFlat) flatHand = AN.vnorm(flatHand);
  let wantDir = pose.bladeDir || AN.v3();
  if (AN.vlen(wantDir) < 1e-4) return;
  wantDir = AN.vnorm(wantDir);
  let wantFlat = pose.bladeFlat || AN.v3();
  wantFlat = AN.vsub(wantFlat, AN.vmul(wantDir, AN.vdot(wantDir, wantFlat)));
  let want = AN.qfromto(bladeHand, wantDir);
  if (haveFlat && AN.vlen(wantFlat) > 1e-4) {
    wantFlat = AN.vnorm(wantFlat);
    const cur = AN.qrot(want, flatHand);
    const s = AN.vdot(AN.vcross(cur, wantFlat), wantDir);
    const c = AN.vdot(cur, wantFlat);
    if (Math.abs(s) > 1e-6 || Math.abs(c) > 1e-6)
      want = AN.qmul(AN.qaxisangle(wantDir, Math.atan2(s, c)), want);
  }
  let delta = AN.qnorm(AN.qmul(want, AN.qconj(rigid)));
  const ang = 2 * Math.acos(Math.min(1, Math.abs(delta.w)));
  const lim = Math.max(pose.wristMaxAngle ?? 3.10, 0);
  if (ang > lim && ang > 1e-4) delta = AN.qslerp(AN.qid(), delta, lim / ang);
  const steerAmt = clamp(pose.steerAmount ?? 1, 0, 1);
  if (steerAmt < 1) delta = AN.qslerp(AN.qid(), delta, steerAmt);
  const handRot = AN.qnorm(AN.qmul(delta, rigid));
  anim.model[handPart].rot = AN.qslerp(rigid, handRot, clamp(weight, 0, 1));
  // Everything hanging off the hand follows rigidly (parents-first).
  const n = Math.min(skel.parts.length, anim.model.length);
  const moved = new Uint8Array(n);
  moved[handPart] = 1;
  for (let k = handPart + 1; k < n; k++) {
    const par = skel.parts[k].parent;
    if (par < 0 || !moved[par] || !anim.local[k]) continue;
    moved[k] = 1;
    anim.model[k].rot = AN.qnorm(AN.qmul(anim.model[par].rot, anim.local[k].rot));
    anim.model[k].pos = AN.vadd(anim.model[par].pos, AN.qrot(anim.model[par].rot, anim.local[k].pos));
  }
}

/** The weapon arm's [shoulder, elbow, wrist] part indices (-1 = none). */
function weaponArmParts() {
  const parts = [-1, -1, -1];
  if (!skel) return parts;
  const eff = resolveEffector();
  if (eff.mode === 'aim') return parts;
  const hand = { hand: -1 };
  const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
  const ch = ci >= 0 ? skel.chains[ci] : null;
  if (!ch || (ch.parts || []).length < 2) return parts;
  const h = eff.part >= 0 ? hand.hand : ch.effector;
  parts[0] = ch.parts[0];
  parts[1] = ch.parts[1];
  parts[2] = h !== ch.parts[1] ? h : -1;
  return parts;
}

/** Each weapon-arm joint's rotation relative to its parent, as posed now. */
function weaponArmLocals(parts) {
  return parts.map(i => {
    if (i < 0 || !anim.model[i]) return null;
    const par = skel.parts[i].parent;
    return par >= 0 && anim.model[par]
      ? AN.qnorm(AN.qmul(AN.qconj(anim.model[par].rot), anim.model[i].rot))
      : anim.model[i].rot;
  });
}

/**
 * THE ARM ON SCREEN AS A FRAME POSE (melee.js "A FRAME IS A POSE") — the
 * held frame's shoulder / elbow / wrist, relative to their parents, with the
 * frame's AIM TURN TAKEN BACK OFF the shoulder (a pose is stored as authored
 * against a target straight ahead; playback turns it onto the real one), and
 * the torso the pose was shown with.
 *
 * `step` first runs one weapon tick and one preview step, so the model is the
 * held frame and not whatever was drawn last (the bake calls it right after
 * changing the held frame). Returns null for an aim effector (no arm).
 */
function captureHeldPose(step = true) {
  if (!skel || !anim?.model?.length || !melee) return null;
  if (step) {
    // A few tries: a style whose effector just changed has no rest arm yet,
    // and the goal branch lets go for a tick to take one (weaponTick).
    for (let i = 0; i < 6; i++) {
      weaponTick();
      const posed = !!heldKeyed || melee.poseWeight() > 0;
      stepPreviewFixed(kPreviewDt, true);
      if (posed) break;
    }
  }
  const parts = weaponArmParts();
  if (parts[0] < 0 || parts[1] < 0) return null;
  const loc = weaponArmLocals(parts);
  const key = ATK.goalFrame?.();
  const lib = ATK.library();
  const sty = lib ? lib.styles[ATK.styleIndex()] : null;
  // 'f<k>' or 'f<k>@<tick>' (a mid-frame sample): the frame it belongs to.
  const k = typeof key === 'string' && key[0] === 'f' ? parseInt(key.slice(1), 10) : -1;
  const f = sty && k >= 0 ? sty.frames[k] : null;
  const a = f && !f.fromBody ? aimAtRest() : { az: 0, el: 0 };
  const par0 = skel.parts[parts[0]].parent;
  const P = par0 >= 0 ? anim.model[par0].rot : AN.qid();
  const R = MELEE.aimQuat(a.az, a.el);
  loc[0] = AN.qnorm(AN.qmul(AN.qconj(P), AN.qmul(AN.qconj(R), AN.qmul(P, loc[0]))));
  const wp = weaponPose();
  return { joint: loc, twist: wp ? wp.torsoTwist || 0 : 0,
           pitch: wp ? wp.torsoPitch || 0 : 0 };
}

function smoothWeaponArm(dt) {
  const S = armSmooth;
  S.keyedGot = null;
  // mob.cpp: THE RELEASE takes this stage over; the brakes are off for it.
  const wp = weaponPose();
  const weight = wp ? wp.weight : 0;
  const releasing = !!wp && wp.release >= 0 && weight > 0;
  const driven = weight > 0 && !releasing;
  const keyed = driven && wp.keyed ? wp.keyed : null;
  const asked = driven ? liveArmJoints() : null;
  if (asked) S.params = asked;
  else if (!S.valid && !driven && !releasing) {
    S.lastValid = false; S.relActive = false; S.keyLive = null;
    return;
  }
  if (!skel || !anim?.model?.length) { S.valid = false; return; }
  const eff = resolveEffector();
  const parts = [-1, -1, -1];
  if (eff.mode !== 'aim') {
    const hand = { hand: -1 };
    const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
    const ch = ci >= 0 ? skel.chains[ci] : null;
    if (ch && (ch.parts || []).length >= 2) {
      const h = eff.part >= 0 ? hand.hand : ch.effector;
      parts[0] = ch.parts[0];
      parts[1] = ch.parts[1];
      parts[2] = h !== ch.parts[1] ? h : -1;
    }
  }
  const n = Math.min(skel.parts.length, anim.model.length);
  for (let k = 0; k < 3; k++) if (parts[k] >= n) parts[k] = -1;
  if (parts[0] < 0 || parts[1] < 0) {
    S.valid = false; S.lastValid = false; S.relActive = false;
    return;
  }
  if (S.valid && parts.some((p, k) => p !== S.part[k])) S.valid = false;
  const localOf = (i) => {
    const par = skel.parts[i].parent;
    return par >= 0 && par < n
      ? AN.qnorm(AN.qmul(AN.qconj(anim.model[par].rot), anim.model[i].rot))
      : anim.model[i].rot;
  };
  const want = parts.map(i => (i >= 0 ? localOf(i) : null));
  // ---- A KEYED POSE (melee.js "A FRAME IS A POSE"; mob.cpp) ---------------
  // `want` becomes the frames' joints, slerped; the brakes below still see it
  // as the pose asked for. applyWeaponArm solved nothing, so `want` is the
  // animation's arm until this replaces it -- which is exactly the arm a
  // stroke that starts from rest takes over from.
  if (keyed) {
    if (keyed.fromLive) {
      if (!S.keyLive) {
        const same = S.lastValid && parts.every((pp, k) => pp === S.lastPart[k]);
        S.keyLive = same ? S.last.slice() : want.slice();
      }
    } else {
      S.keyLive = null;
    }
    const par0 = skel.parts[parts[0]].parent;
    const P = par0 >= 0 && par0 < n ? anim.model[par0].rot : AN.qid();
    // The whole arm turned about the shoulder, in the BODY's frame: a
    // rotation R of the upper arm's model rotation is conj(P) R P on its local.
    const aimed = (q, a) => (a && (a.yaw || a.pitch))
      ? AN.qnorm(AN.qmul(AN.qconj(P), AN.qmul(MELEE.aimQuat(a.yaw, a.pitch), AN.qmul(P, q))))
      : q;
    for (let k = 0; k < 3; k++) {
      if (parts[k] < 0) continue;
      let from = keyed.fromLive ? S.keyLive[k] : keyed.from[k];
      let to = keyed.to[k];
      if (!from || !to) continue;               // no key for this joint: the animation's
      if (k === 0) { from = aimed(from, keyed.aimFrom); to = aimed(to, keyed.aimTo); }
      want[k] = AN.qnorm(AN.qslerp(from, to, keyed.t));
    }
  } else {
    S.keyLive = null;
  }
  let got = [null, null, null];
  if (releasing) {
    // mob.cpp: every joint turns from the last driven pose to the
    // animation's (`want` -- applyWeaponArm solved nothing this tick).
    if (!S.relActive) {
      const same = S.lastValid && parts.every((pp, k) => pp === S.lastPart[k]);
      if (!same) { S.valid = false; return; }
      S.relFrom = S.last.slice();
      S.relActive = true;
      // A release out of a KEYED pose is not clamped either (reapplyKeyedArm):
      // its first tick is that pose, which was never clamped.
      S.relKeyed = !!S.lastKeyed;
    }
    for (let k = 0; k < 3; k++)
      if (parts[k] >= 0) got[k] = AN.qnorm(AN.qslerp(S.relFrom[k], want[k], wp.release));
    S.valid = false;
    S.lastValid = false;
  } else {
  S.relActive = false;
  // (A keyed pose falls through to the re-flatten below: nothing else has
  // written it into the model.)
  if (!asked && !S.valid) {
    S.last = want.slice(); S.lastPart = parts.slice(); S.lastValid = driven;
    if (!keyed) return;
    got = want.slice();
  } else if (!S.valid) {
    S.part = parts.slice();
    S.prev = want.slice();
    S.lag = [0, 0, 0];
    S.last = want.slice(); S.lastPart = parts.slice(); S.lastValid = driven;
    S.valid = !!asked;
    if (!keyed) return;
    got = want.slice();
  } else {
  const ticks = Math.max(dt * 30, 0);
  const angleBetween = (a, b) =>
    2 * Math.acos(Math.min(1, Math.abs(AN.qdot(a, b))));
  let lagMax = 0;
  for (let k = 0; k < 3; k++) {
    if (parts[k] < 0) continue;
    const p = S.params[MELEE.ARM_JOINTS[k]] || { smooth: 0, maxDeg: 0 };
    let q = S.prev[k];
    if (!(p.smooth > 0 || p.maxDeg > 0)) {
      q = want[k];
    } else {
      // Low-pass first, THEN the cap: the cap is a ceiling on what the joint
      // actually does this tick, so it must see the smoothed step.
      let out = want[k];
      if (p.smooth > 0) out = AN.qslerp(q, want[k], 1 - Math.pow(2, -ticks / p.smooth));
      if (p.maxDeg > 0) {
        const ang = angleBetween(q, out);
        const lim = p.maxDeg * Math.PI / 180 * ticks;
        if (ang > lim && ang > 1e-5) out = AN.qslerp(q, out, lim / ang);
      }
      q = AN.qnorm(out);
    }
    got[k] = q;
    S.prev[k] = q;
    S.lag[k] = angleBetween(q, want[k]);
    lagMax = Math.max(lagMax, S.lag[k]);
  }
  if (!asked && lagMax < 0.0035) S.valid = false;
  S.last = parts.map((pp, k) => (pp >= 0 ? got[k] : want[k]));
  S.lastPart = parts.slice(); S.lastValid = driven;
  }
  }
  reflattenArm(parts, got);
  // A KEYED arm is written again AFTER the stage-6 clamp (reapplyKeyedArm):
  // the pose is the author's, exactly, at every tick.
  if (driven) S.lastKeyed = !!keyed;
  S.keyedGot = keyed || (releasing && S.relKeyed)
    ? { parts: parts.slice(), got: got.slice() } : null;
}

// RE-FLATTEN THE ARM'S SUBTREE with the given locals (parents-first): every
// part keeps its transform relative to its parent, so the sword rides along.
function reflattenArm(parts, got) {
  const n = Math.min(skel.parts.length, anim.model.length);
  const old = anim.model.slice(0, n).map(t => ({ pos: { ...t.pos }, rot: { ...t.rot } }));
  const moved = new Uint8Array(n);
  const root = parts[0];
  for (let i = root; i < n; i++) {
    const par = skel.parts[i].parent;
    if (i !== root && (par < 0 || !moved[par])) continue;
    moved[i] = 1;
    if (par < 0 || par >= n) continue;
    let rel = AN.qnorm(AN.qmul(AN.qconj(old[par].rot), old[i].rot));
    const relPos = AN.qrotinv(old[par].rot, AN.vsub(old[i].pos, old[par].pos));
    for (let k = 0; k < 3; k++) if (parts[k] === i && got[k]) rel = got[k];
    anim.model[i].rot = AN.qnorm(AN.qmul(anim.model[par].rot, rel));
    anim.model[i].pos = AN.vadd(anim.model[par].pos,
                                AN.qrot(anim.model[par].rot, relPos));
  }
}

/**
 * mob.cpp Mob::ReapplyKeyedArm — A KEYED POSE IS NOT CLAMPED. The frames are
 * the author's arm, and the stage-6 clamp projecting them onto the rig's
 * authored hinge and ball limits changed the arm BETWEEN two frames that were
 * each legal on their own (the slerp crosses a limit's edge and the projection
 * jumps), which is exactly the spin a keyed stroke exists to remove. So the
 * clamp runs for the rest of the body and the keyed arm is written again after
 * it. Limits are the author's to respect while posing.
 */
function reapplyKeyedArm() {
  const k = armSmooth.keyedGot;
  if (!k || !skel || !anim?.model?.length) return;
  reflattenArm(k.parts, k.got);
}

/**
 * mob.cpp:14777 Mob::ApplyStrikeAim — stage 3.5, and PRE-FLATTEN.
 *
 * When the effector is an AIM part (the jaws), this IS the attack: the part
 * and an authored share of the spine are rotated until the part's forward lies
 * along the stroke's own bearing. There is no IK behind it and, in the game,
 * the body's travel — the lunge — is what carries it to the target.
 *
 * THE STROKE'S BEARING, NOT THE POSE'S BLADE DIRECTION, and the distinction is
 * the whole reason the bite reads as a snap. `pose.bladeDir` is the driver's
 * solved blade LEAN (melee.js's law of cosines: the angle a rigid bar has to
 * sit at so its far end reaches the commanded radius from a hand held at the
 * live extension). For a held sword that is exactly right; for an aim effector
 * there is no bar and no hand, so driving the part at the lean meant chasing a
 * quantity derived from a geometry it is not in. `strokeAz`/`strokeEl` are the
 * commanded bearing about the pivot in the wielder's own basis — laid straight
 * onto the part's forward they are a 1:1 ask.
 *
 * NO SMOOTHING, deliberately: a four-tick cut can never catch a goal through a
 * halflife. A stroke owns its part outright.
 *
 * Returns the applied {yaw, pitch} for the readout, or null.
 */
function applyStrikeAim() {
  const eff = resolveEffector();
  if (eff.mode !== 'aim' || !melee || !skel || !anim?.local?.length) return null;
  const w0 = melee.poseWeight();
  if (!(w0 > 0)) return null;
  // The rig's own facing basis. Heading is 0 in the editor and the model is
  // authored in its own frame, so fwd = +Z, up = +Y and right = fwd x up = -X
  // — the sign anim.cpp:404 states and the side every def's `.R` limbs sit on.
  const right = AN.v3(-1, 0, 0), up = AN.v3(0, 1, 0), fwd = AN.v3(0, 0, 1);
  let dir = AN.v3();
  if (strokeLive()) {
    const az = melee.strokeAz(), el = melee.strokeEl();
    const ce = Math.cos(el);
    dir = AN.vadd(AN.vadd(AN.vmul(right, ce * Math.sin(az)),
                          AN.vmul(up, Math.sin(el))),
                  AN.vmul(fwd, ce * Math.cos(az)));
  }
  // A guard, or a pose pushed in by something that is not a stroke program,
  // still has a blade direction and nothing better to offer.
  const pose = melee.pose();
  if (AN.vlen(dir) < 1e-4 && pose) dir = pose.bladeDir || AN.v3();
  if (AN.vlen(dir) < 1e-4 && pose) dir = pose.hand || AN.v3();
  if (AN.vlen(dir) < 1e-4) return null;
  dir = AN.vnorm(dir);
  // mob.cpp:14666 AimAnglesTo, at heading 0: a bearing in the rig's own
  // heading convention (0 = +Z, positive toward +X) and a pitch positive UP.
  const yaw = Math.atan2(dir.x, dir.z);
  const pitch = Math.asin(Math.max(-1, Math.min(1, dir.y)));
  // FULL WEIGHT THROUGH THE CUT: PoseWeight is already 1 in every phase except
  // a releasing recover, so this changes nothing today — it is here so that a
  // future change to the arm-claim ramp cannot quietly halve a bite.
  const w = (strokeCur?.phase === MELEE.STROKE_PHASE.Cut) ? 1 : w0;
  const share = Number.isFinite(+sc()?.aimSpineShare) ? +sc().aimSpineShare : 0.35;
  AN.animApplyAimPart(skel, anim, eff.part, yaw, pitch, w, share, skel.rootLimb);
  lastAim = { part: eff.part, yaw, pitch, weight: w, share };
  return lastAim;
}

// What applyStrikeAim last commanded, for the panel's readout and the harness.
// Null when no aim effector ran this tick — which is the distinction a bare
// "the head did not move" cannot make: no aim was asked for, versus one was
// asked for and the part refused it.
let lastAim = null;

// The override the stage-6 clamp was last handed, kept for the test seam.
// Measuring the elbow's bend about its AUTHORED axis while the driver is
// steering the plane reads a different quantity — mob.cpp:7918 is the whole
// note on why the two senses name one plane but only one makes the authored
// [0, 130] describe the bend. A probe that used the wrong one reported -64
// degrees on a perfectly legal arm.
let lastElbowOverride = null;

/**
 * THE SEAM attacks.js is given. Everything the panel needs from the rig is
 * behind this object, so that file never touches the skeleton and this one
 * never parses a style.
 */
function bindAttacks() {
  ATK.bind({
    el, toast,
    begin: async (idx) => {
      const lib = ATK.library();
      const sty = lib?.styles?.[idx];
      if (sty?.clip) await ensureClipOnSkeleton(sty.clip);
      beginStroke(idx);
    },
    stop: stopStroke,
    rerender: () => renderTimeline(),
    limbByName,
    sidecarName: () => ed.getDocName?.() || '<mob>.json',
    sidecar: () => sc(),
    touchSidecar: () => touched(),
    tuning: () => tuningDoc(),
    touchTuning: () => tuningTouch(),
    // ONE Ctrl+Z for the whole tab. The Attacks lane edits three documents
    // that are not the model, and a second undo stack would make the shortcut
    // mean different things depending on where the pointer was.
    pushUndo: (label, undoFn, redoFn) => ed.pushUndoEntry?.(label, undoFn, redoFn),
    equipWeapon: (id, off) => (off ? loadHeldItem(null).then(() => {
      rebuildSkeleton(); ed.invalidate(); renderAllPanels(); return true;
    }) : equipWeapon(id)),
    weaponEquipped,
    weaponName: () => heldItemId || '',
    meleeItems: () => (itemList || []).filter(it => it.kind === 'melee'),
    ensureItemList,
    onTuningChanged: () => { if (melee) melee.tuning = meleeTuning(); },
    // READ-ONLY, for the panel's own hints — "0 = the global value, which is
    // currently 0.22 s" is the difference between a field an author can set
    // and one they have to go and look up.
    meleeTuning,
    onStylesChanged: () => { /* a live swing keeps running on the edited data */ },
    // ONE GOAL FRAME for the panel's readout: the AUTHORED destination, plus
    // `arrived` — where the program really gets to by the end of the frame,
    // which is the pose weaponTick holds. Run on a scratch driver with the
    // same arm, tuning, seed and aim, because the panel repaints on the click,
    // before the first held tick has run.
    goalPose: (key) => {
      const lib = ATK.library();
      const gi = ATK.styleIndex();
      const s = lib ? lib.styles[gi] : null;
      const a = aimAtRest();
      const g = (s && melee) ? MELEE.strokeGoalPose(s, melee, key, a.az, a.el) : null;
      if (!g) return null;
      let arrived = null;
      const rest = restFor();
      if (rest) {
        const m = new MELEE.MeleeState(meleeTuning());
        m.setHandSign(melee.handSign_);
        if (melee.keepR_ > 0) m.setKeepOut(melee.keepC_, melee.keepR_);
        m.setStroke(rest.hand, rest.tip, rest.flat, rest.reach);
        if (MELEE.runStrokeToFrame(s, gi, strokeSeed(), m, key, a.az, a.el, kStrokeDt,
                                   AN.v3(-1, 0, 0), AN.v3(0, 1, 0), AN.v3(0, 0, 1), a.dist))
          arrived = { az: m.azLive_, el: m.elLive_, reach: m.radLive_ };
      }
      return { ...g, arrived };
    },
    onTrailToggled: () => { strokeTrail.length = 0; ed.setStrokeTrail?.(null); },
    // THE HELD FRAME'S ARM AS A POSE (captureHeldPose): the bake and the
    // "capture" button both read the rig through this.
    captureHeldPose: (step = true) => captureHeldPose(step),
    bakeMidPose: (p0, p1) => bakeMidPose(p0, p1),
    mirrorPose: (p) => mirrorPose(p),
    flipElbowPose: (p) => flipElbowPose(p),
    rebuild: () => { rebuildSkeleton(); ed.invalidate(); },
    state: () => ({
      live: strokeLive(),
      phase: MELEE.STROKE_PHASE_NAME[strokeCur?.phase ?? 0],
      phaseTick: strokeCur?.phaseTick ?? 0,
      tick: strokeTick,
      holding: strokeHoldTicks > 0,
      meleePhase: melee ? melee.phaseName() : 'idle',
      az: melee?.strokeAz() ?? 0,
      el: melee?.strokeEl() ?? 0,
      radius: melee?.strokeRadius() ?? 0,
      weight: melee?.poseWeight() ?? 0,
      tipSpeed: strokeTipSpeed,
      edgeAlign: strokeEdgeAlign,
      // the program's own numbers (strokes.cpp StepStrokeProgram): where the
      // windup is steering to, and the aim it froze
      wantAz: strokeCur?.wantAz ?? 0,
      wantEl: strokeCur?.wantEl ?? 0,
      wantReach: strokeCur?.wantReach ?? 0,
      aimAz: strokeCur?.aimAz ?? 0,
      aimEl: strokeCur?.aimEl ?? 0,
      aimed: !!strokeCur?.aimed,
      windupTicks: strokeCur?.windupTicks ?? 0,
      cutTicks: strokeCur?.cutTicks ?? 0,
      start: strokeStart,
      commit: strokeCommit,
      // the closed-loop steering cap, radians per tick: 16 units x the aim
      // gain (strokes.cpp steerTo). A raise bigger than cap x windupTicks
      // cannot finish inside the windup.
      capAz: 16 * (melee?.tuning?.aimGainX ?? 0),
      capEl: 16 * (melee?.tuning?.aimGainY ?? 0),
    }),
    armInfo: () => {
      if (!skel) return { chain: -1 };
      // THE CHAIN THE EFFECTOR IS ON, not the socket's — with a fist style
      // selected the panel's per-limb rows and reach band must describe the
      // arm that is actually swinging.
      const eff = resolveEffector();
      const h = { hand: -1 };
      const ci = eff.mode === 'aim' ? -1
               : eff.part >= 0 ? chainForEffector(eff.part, h) : wpChain;
      const ch = ci >= 0 ? skel.chains[ci] : null;
      const seed = weaponArmSeed();
      const band = melee ? melee.reachBand() : { lo: 0, hi: 0 };
      return {
        // AIM REPORTS chain 0, NOT -1. The panel treats a negative chain as
        // "this rig has no weapon arm at all" and replaces its whole readout
        // with that warning — true for a sword, and nonsense for a bite, which
        // needs no chain by construction.
        chain: eff.mode === 'aim' ? 0 : ci,
        armName: ch ? ch.parts.map(i => skel.parts[i]?.name).join(' → ')
                    : (eff.part >= 0 ? (skel.parts[eff.part]?.name || '') : ''),
        armParts: ch ? ch.parts.map(i => skel.parts[i]).filter(Boolean)
                     : (eff.part >= 0 && skel.parts[eff.part]
                        ? [skel.parts[eff.part]] : []),
        handSign: melee ? melee.handSign_ : 1,
        reach: seed?.reach ?? 0,
        bladeLen: melee?.bladeLen_ ?? 0,
        blade: !!bladeSegmentModel(),
        bandLo: band.lo, bandHi: band.hi,
        // ---- which part the driver is moving, and with what ---------------
        effPart: eff.part >= 0 ? (skel.parts[eff.part]?.name || '') : '',
        effMode: eff.mode,
        effWeapon: eff.natural >= 0 ? (naturalList()[eff.natural]?.name || '') : '',
        aim: lastAim ? { yaw: lastAim.yaw, pitch: lastAim.pitch,
                         share: lastAim.share } : null,
      };
    },
    // The open rig's own `natural` block, for the style's weapon picker: a
    // style names a weapon and only this rig can say whether it has one.
    naturalWeapons: () => naturalList().map(w => ({
      name: w?.name || '', part: w?.part || '',
      usable: naturalUsable(w),
      mode: (() => {
        const pi = skel ? skel.findPart(w?.part || '') : -1;
        return pi < 0 ? '—' : (chainForEffector(pi, {}) >= 0 ? 'chain' : 'aim');
      })(),
    })),
    // Every limb TAG this rig publishes, for the `target` weight row. Tags and
    // not names, because that is what a style's table is keyed on (strokes.h
    // StyleTargetWeight: "any rig that tags its parts answers it, including
    // one with four of them").
    limbTags: () => {
      const out = [];
      for (const p of (skel?.parts || []))
        if (p.tag && !out.includes(p.tag)) out.push(p.tag);
      return out.sort();
    },
    // The arc a style would fly, solved but not flown, plus the live flight
    // when one is in the air. See lungeSolve.
    lungeInfo: (sty) => {
      const sol = lungeSolve(sty);
      if (!sol) return null;
      return { ...sol, live: lungeArc ? {
        airTicks: lungeArc.airTicks, landed: lungeArc.landed,
        cutStart: lungeArc.cutStart, x: lungeArc.x, y: lungeArc.y } : null };
    },
    // The post-jitter tick counts of the LIVE program, so the timeline can say
    // which tick the cut actually starts on rather than the authored one.
    cursorTicks: () => ({ windup: strokeCur?.windupTicks ?? 0,
                          cut: strokeCur?.cutTicks ?? 0,
                          tick: strokeTick }),
  });
}

/**
 * The swept edge, recorded per TICK, in file voxels — the shape of the attack
 * and the only way to see whether a style's cut passes through where it was
 * aimed or behind it. Coloured by phase so the telegraph is visibly separate
 * from the cut.
 */
function recordStrokeTrail() {
  if (!strokeLive()) return;
  // WHICHEVER EDGE IS SWINGING. A fist's knuckles and a set of jaws leave a
  // trail for the same reason a blade does, and they are the only picture of
  // where an unarmed style actually puts its hitbox.
  const blade = effectorEdgeModel();
  if (!blade) return;
  // ---- the two numbers the damage formula scales by ---------------------
  // TIP SPEED, in WORLD voxels/sec, because melee.minSpeedMps and
  // fullSpeedMps are: a rig-voxel figure would read 8x high on the human and
  // silently pass a threshold it should not. Measured across the whole frame
  // rather than one sim tick — the sweep does the same.
  const S = rigScale();
  if (strokeTipPrev)
    strokeTipSpeed = AN.vlen(AN.vsub(blade.tip, strokeTipPrev)) / S / kStrokeDt;
  // EDGE ALIGNMENT (melee.cpp:812 MeleeEdgeAlign): |cos| between the flat's
  // normal and the travel, floored by melee.edgeFloor. 1 is edge-on, the floor
  // is a pure slap with the side. Absolute because a double-edged blade cuts
  // equally well on either face.
  if (strokeTipPrev && AN.vlen(blade.flat) > 1e-6) {
    const travel = AN.vsub(blade.tip, strokeTipPrev);
    const tl = AN.vlen(travel);
    if (tl > 1e-6) {
      const c = Math.abs(AN.vdot(blade.flat, travel) / (AN.vlen(blade.flat) * tl));
      const floorF = clamp(meleeTuning().edgeFloor, 0, 1);
      strokeEdgeAlign = floorF + (1 - floorF) * (1 - clamp(c, 0, 1));
    }
  }
  strokeTipPrev = blade.tip;
  if (!ATK.trailEnabled()) { strokeTrail.length = 0; return; }
  // RECORDED WITH THE OFFSET IT HAPPENED AT. The body's lunge translation is
  // applied by the renderer (modelTransform), so a trail sampled in model space
  // would stay behind on the launch pad while the creature flew away from it.
  const lo = lungeOffsetModel();
  // The haft rides the same record (null when the item authors none), so its
  // ribbon is sampled on exactly the ticks the head's is.
  const haft = haftSegmentModel();
  strokeTrail.push({ base: AN.vadd(blade.base, lo), tip: AN.vadd(blade.tip, lo),
                     haftBase: haft ? AN.vadd(haft.base, lo) : null,
                     haftTip: haft ? AN.vadd(haft.tip, lo) : null,
                     phase: strokeCur.phase });
  while (strokeTrail.length > kStrokeTrailMax) strokeTrail.shift();
}

/* ==========================================================================
   6. timeline + flipbook frames

   A flipbook is a named range over the file's models: frame i of tag T is
   models[T.frames[i].model]. That matches the schema
   ("flipbooks": { "death": { "frames": [ {part, model, durationMs} ] } })
   and means frames cost nothing extra in the .vox — they ARE the models.
   ========================================================================== */

const DEFAULT_FRAME_MS = 100;

// The frame list the timeline shows: the active tag's frames, or an implicit
// one-frame-per-model list when no tag is selected.
function frameList() {
  const fb = flipbooks();
  if (activeTag && fb[activeTag]) {
    if (!Array.isArray(fb[activeTag].frames)) fb[activeTag].frames = [];
    return fb[activeTag].frames;
  }
  return null;                 // implicit: every model, in order
}

function frameCount() {
  const f = frameList();
  return f ? f.length : ed.getModels().length;
}

// Model index displayed at timeline position i.
function frameModel(i) {
  const f = frameList();
  if (!f) return i;
  const e = f[i];
  return e ? clamp(num(e.model, 0), 0, ed.getModels().length - 1) : 0;
}

function frameMs(i) {
  const f = frameList();
  if (!f) return DEFAULT_FRAME_MS;
  return Math.max(1, num(f[i]?.durationMs, DEFAULT_FRAME_MS));
}

const isRigged = () => limbs().length > 0;

const modelIndexByName = n => ed.getModels().findIndex(m => m.name === n);

// The part a tag's frames animate. The ENGINE requires every frame to name a
// part (mob.cpp drops frames whose part does not resolve), and in practice a
// whole tag animates one part, so the UI exposes it per tag. Returns the
// unique part name, '' when no frame has one, or null when frames disagree.
function tagPartOf(tag) {
  const frames = flipbooks()[tag]?.frames || [];
  let part = '';
  for (const f of frames) {
    const p = f.part || '';
    if (!part) part = p;
    else if (p && p !== part) return null;          // mixed
  }
  return part;
}

// Default part for a new tag on a rigged file: the selected limb, else root,
// else the first limb.
function defaultTagPart() {
  return (selectedPart && limbByName(selectedPart)) ? selectedPart
       : (sc().root || limbs()[0]?.name || '');
}

/**
 * User navigation to a frame (click / [ ] keys). Also selects the frame's
 * model so the brushes edit what you are looking at. Automatic PLAYBACK never
 * comes through here — setActiveModel resets the per-model undo stack, which
 * is fine for a deliberate switch and disastrous 10x per second.
 */
function gotoFrame(i) {
  const n = frameCount();
  if (!n) return;
  frameIndex = ((i % n) + n) % n;
  frameClockMs = 0;
  if (!playing) ed.setActiveModel(frameModel(frameIndex));
  ed.invalidate();
  renderTimeline();
}

// Cheap highlight-only refresh for playback: a full renderTimeline() per
// frame advance would rebuild thumbnails and tear focus out of the duration
// inputs.
function updateFrameStrip() {
  const cells = timelineEl?.querySelectorAll('.framestrip .frame');
  if (cells) cells.forEach((c, i) => c.classList.toggle('on', i === frameIndex));
}

function togglePlay() {
  if (!playing && !frameList() && isRigged()) {
    // "All models" on a rig would step the view through the LIMBS — always
    // wrong, and confusing enough that we refuse rather than preview garbage.
    toast('this file is a rig — flipbooks animate one part: create a tag ' +
      '(+ tag) to author frames, or use clips / the gait preview', true);
    return;
  }
  playing = !playing;
  frameClockMs = 0;
  ed.invalidate();
  renderTimeline();
}

/**
 * What the viewport should show — see editor.js rebuildInstances.
 * - Flipbook PLAYBACK: the composed frame, exactly what the engine renders.
 *   For a rig that means every limb's model plus the current frame's model
 *   swapped in at the flipbooked part's slot (parked variant models hidden);
 *   for a plain file it means only the current frame's model.
 * - Gait / clip preview: everything at full brightness.
 * - Otherwise null: the normal editing view.
 */
function viewPlan() {
  if (playing) {
    const models = ed.getModels();
    const frames = frameList();
    const cur = frames?.[frameIndex];
    if (cur && cur.part && isRigged()) {
      const entries = [];
      for (const l of limbs()) {
        const mi = modelIndexByName(l.name);
        if (mi < 0) continue;
        if (l.name === cur.part) {
          // The engine swaps the FRAME model's voxels into the part's slot:
          // draw that model at the part's offset, moving with the part.
          const fmi = clamp(num(cur.model, mi), 0, models.length - 1);
          entries.push({ model: fmi, offset: models[mi].offset, xfModel: mi });
        } else {
          entries.push({ model: mi });
        }
      }
      return { entries };
    }
    // Plain flipbook file (or an implicit all-models book): one frame at a
    // time, like the engine's microvox flipbooks.
    return { entries: [{ model: frameModel(frameIndex) }] };
  }
  if (gaitOn || clipPlaying || activeClip) return { allBright: true };
  return null;
}

/**
 * THE TIMELINE AREA HAS TWO LANES, and they are separate tabs rather than two
 * stacked panels because the Attacks lane needs the full width: it carries a
 * per-limb table and a compass pad, and squeezed under the frame strip and the
 * clip rows it was a column of single characters — the same failure the `.rigf>i`
 * note in tuner.html describes, one panel over.
 */
let laneTab = 'animation';    // 'animation' | 'attacks'

function renderTimeline() {
  if (!timelineEl) return;
  timelineEl.innerHTML = '';

  const tabs = el('div', { class: 'tagbar' });
  const laneBtn = (id, label, title) => el('button', {
    class: 'small' + (laneTab === id ? ' on' : ''), title,
    onclick: () => { laneTab = id; renderTimeline(); },
  }, label);
  tabs.append(
    laneBtn('animation', 'animation',
      'flipbook frames and keyframed clips — the pose data in this rig\'s own sidecar'),
    laneBtn('attacks', 'attacks',
      'the stroke programs in assets/mobs/attack_styles.json, previewed on ' +
      'this rig through the real melee driver'),
    el('span', { class: 'spacer' }),
    el('span', { class: 'hint' },
      laneTab === 'animation'
        ? 'K walks · space plays the flipbook · P plays the clip · I keys'
        : 'the swing runs the SAME driver the game does — melee.js is a port ' +
          'of melee.cpp, not a second implementation'));
  timelineEl.append(tabs);

  if (laneTab === 'attacks') {
    const atkWrap = el('div', { class: 'clipwrap' });
    timelineEl.append(atkWrap);
    ATK.render(atkWrap);
    return;
  }

  const fb = flipbooks();
  const tags = Object.keys(fb);

  /* ---- tag bar ---- */
  const tagbar = el('div', { class: 'tagbar' });
  tagbar.append(el('button', {
    class: 'small' + (activeTag === null ? ' on' : ''),
    title: 'every model in file order',
    onclick: () => { activeTag = null; gotoFrame(0); renderTimeline(); },
  }, 'all models'));
  for (const t of tags) {
    tagbar.append(el('button', {
      class: 'small' + (activeTag === t ? ' on' : ''),
      onclick: () => { activeTag = t; gotoFrame(0); renderTimeline(); },
    }, t + ' (' + (fb[t].frames?.length || 0) + ')'));
  }
  tagbar.append(
    el('button', {
      class: 'small', title: 'new flipbook tag',
      onclick: () => {
        const n = prompt('tag name (walk / idle / attack / death)', 'idle');
        if (!n) return;
        if (fb[n]) return toast('that tag already exists', true);
        if (isRigged()) {
          // The engine's flipbooks are PER-PART: each frame swaps one limb's
          // model for another model in the file. Seed one frame showing the
          // part's own model; the author duplicates models for further frames.
          const part = defaultTagPart();
          const mi = Math.max(0, modelIndexByName(part));
          fb[n] = { frames: [{ part, model: mi, durationMs: DEFAULT_FRAME_MS }] };
          toast(`tag "${n}" animates "${part}" — change it in the part dropdown; ` +
            'duplicate a model (⧉), edit it, then + to add it as a frame');
        } else {
          // Plain flipbook file: every model is a frame (microvox convention).
          fb[n] = {
            frames: ed.getModels().map((_, i) =>
              ({ model: i, durationMs: DEFAULT_FRAME_MS })),
          };
        }
        touched();
        activeTag = n;
        gotoFrame(0);
        renderTimeline();
      },
    }, '+ tag'),
    activeTag ? el('button', {
      class: 'small danger', title: 'delete this tag',
      onclick: () => {
        if (!confirm(`Delete flipbook "${activeTag}"? (models are kept)`)) return;
        delete fb[activeTag];
        touched();
        activeTag = null;
        gotoFrame(0);
        renderTimeline();
      },
    }, '✕ tag') : null,
    el('span', { class: 'spacer' }));

  // Part selector for the active tag. The engine DROPS frames whose part does
  // not resolve to a limb, so a rigged file's tag without a part is dead data
  // — surface that here instead of at load time in the engine.
  if (activeTag && isRigged()) {
    const part = tagPartOf(activeTag);
    const psel = el('select', { class: 'sortsel', title: 'the limb this flipbook animates' });
    if (part === null) psel.append(el('option', { value: '' }, '(mixed parts)'));
    else if (!part) psel.append(el('option', { value: '' }, '⚠ no part — pick one'));
    for (const l of limbs()) psel.append(el('option', { value: l.name }, l.name));
    psel.value = part || '';
    psel.addEventListener('change', () => {
      if (!psel.value) return;
      for (const f of (fb[activeTag].frames || [])) f.part = psel.value;
      touched();
      renderTimeline();
    });
    tagbar.append(el('span', { class: 'hint' }, 'part'), psel);
  }

  tagbar.append(
    el('button', {
      class: 'small' + (playing ? ' on' : ''),
      title: 'play the flipbook — the view shows the composed frame, exactly ' +
        'what the engine renders',
      onclick: togglePlay,
    }, playing ? '■ stop [space]' : '▶ play [space]'),
    el('button', {
      class: 'small' + (onionOn ? ' on' : ''),
      title: 'onion skin: prev red / next blue, wrapping the loop',
      onclick: () => { onionOn = !onionOn; ed.invalidate(); renderTimeline(); },
    }, 'onion [O]'));
  timelineEl.append(tagbar);

  /* ---- frame strip ---- */
  const strip = el('div', { class: 'framestrip' });
  const n = frameCount();
  for (let i = 0; i < n; i++) {
    const mi = frameModel(i);
    const cell = el('div', {
      class: 'frame' + (i === frameIndex ? ' on' : ''),
      draggable: 'true',
      onclick: () => gotoFrame(i),
    });
    cell.append(el('span', { class: 'fnum' }, String(i)));
    const thumb = ed.thumbnail(mi, 40);
    thumb.className = 'fthumb';
    cell.append(thumb);
    cell.append(el('span', { class: 'fname' }, ed.getModels()[mi]?.name || ''));

    // Inline duration, only meaningful for real (tagged) frames.
    if (frameList()) {
      const ms = el('input', { class: 'fms', type: 'number', min: '1', step: '10' });
      ms.value = frameMs(i);
      ms.addEventListener('click', e => e.stopPropagation());
      ms.addEventListener('change', () => {
        frameList()[i].durationMs = Math.max(1, Math.round(+ms.value || DEFAULT_FRAME_MS));
        touched();
      });
      cell.append(ms);
    } else {
      cell.append(el('span', { class: 'fms ro' }, '—'));
    }

    // Drag to reorder (tagged frames only — model order is the file's own).
    if (frameList()) {
      cell.addEventListener('dragstart', e => {
        e.dataTransfer.setData('text/plain', String(i));
        e.dataTransfer.effectAllowed = 'move';
      });
      cell.addEventListener('dragover', e => { e.preventDefault(); cell.classList.add('drop'); });
      cell.addEventListener('dragleave', () => cell.classList.remove('drop'));
      cell.addEventListener('drop', e => {
        e.preventDefault();
        cell.classList.remove('drop');
        const from = parseInt(e.dataTransfer.getData('text/plain'), 10);
        if (!Number.isInteger(from) || from === i) return;
        moveFrame(from, i);
      });
    }
    strip.append(cell);
  }

  if (frameList()) {
    strip.append(el('button', {
      class: 'framadd', title: 'append the ACTIVE model as a new frame',
      onclick: () => {
        const frame = { model: ed.getActiveModel(), durationMs: DEFAULT_FRAME_MS };
        // Frames inherit the tag's part — a part-less frame is dead data to
        // the engine (mob.cpp drops it).
        const part = tagPartOf(activeTag);
        if (part) frame.part = part;
        frameList().push(frame);
        touched();
        gotoFrame(frameCount() - 1);
      },
    }, '+'));
  }
  timelineEl.append(strip);

  const partless = isRigged() && frameList() &&
    frameList().some(f => !f.part || !limbByName(f.part));
  timelineEl.append(el('div', { class: 'hint' },
    frameList()
      ? (partless
          ? '⚠ some frames have no valid part — the ENGINE will drop them; ' +
            'pick a part in the dropdown above. '
          : '') +
        'D duplicate frame · Del delete · [ ] step · drag to reorder · ' +
        'space play · O onion'
      : (isRigged()
          ? 'Showing every model (= limb) in the file for editing. To animate: ' +
            'K walks the gait, clips pose limbs, and "+ tag" makes a per-part ' +
            'flipbook (model swap per frame).'
          : 'Showing every model in the file — playing treats them as flipbook ' +
            'frames, one at a time. Create a tag for per-frame durations.')));

  // The clip lane renders into its own cleared container: renderClipLane()
  // is also called standalone (play/auto-key toggles, end of playback) and
  // appending straight to timelineEl from there would duplicate the lane.
  clipWrap = el('div', { class: 'clipwrap' });
  timelineEl.append(clipWrap);
  renderClipLane();
}

/* ==========================================================================
   6b. clip keyframe lane

   The second timeline lane. Time is in integer ms (the schema's convention),
   the cursor scrubs, and each rigged part gets a row of key diamonds.
   ========================================================================== */

let CLIP_PX_PER_MS = 0.45;        // lane zoom; 420ms clip ≈ 190px
// avatar.cpp's AvatarLocoClips: five names the engine resolves by STRING and
// drives exclusively. Kept here so the lane can mark them — see the note where
// it does.
const LOCO_FAMILY = ['idle', 'walk', 'run', 'fall', 'hang'];
// Show every rigged part's row, not just the keyed ones. Off by default (a
// 15-limb rig is a wall of empty lanes) but essential while BLOCKING OUT a
// clip, when by definition nothing has keys yet — the old lane showed exactly
// one row in that state and gave no way to see the others.
let clipShowAll = false;
// One copied key, for paste and mirror-paste.
let clipboardKey = null;          // { rot, pos, hasRot, hasPos, ease }

function clipObj() {
  const C = clips();
  return activeClip && C[activeClip] ? C[activeClip] : null;
}

// Track for a part inside the active clip, created on demand.
function trackFor(partName) {
  const c = clipObj();
  if (!c) return null;
  if (!c.tracks || typeof c.tracks !== 'object') c.tracks = {};
  if (!c.tracks[partName]) c.tracks[partName] = {};
  return c.tracks[partName];
}

// Every key time in a track, from both the rot and pos lists (the engine
// FUSES them by time — mob.cpp:349 upsert — so the UI must show one diamond
// per time, not one per channel).
function keyTimes(track) {
  const t = new Set();
  for (const k of (track?.rot || [])) t.add(+k.t || 0);
  for (const k of (track?.pos || [])) t.add(+k.t || 0);
  return [...t].sort((a, b) => a - b);
}

/* ---- the clip library: assets/anims/<name>.json ---------------------------
 * Same /api/model route the sidecars use ("anims" is a MODEL_DIRS entry in
 * tuner_server.py). One clip per file, the clip-lane schema plus `name` and
 * `sidecarVoxelsPerMetre` — the same world-length stamp a sidecar carries,
 * because pos keys are world voxels and mob.cpp scales them by it. */
let libraryClips = null;          // [{name, path}] or null until fetched
let libraryFetching = false;

// A SECOND CALLER WAITS FOR THE FETCH IN FLIGHT. It used to get an already
// resolved promise, and libraryClipPicker's `.then(renderClipLane)` then
// re-rendered with the list still null, called this again, and so on — a
// microtask loop that never yields, so the fetch it was waiting for could
// never complete: the whole tab froze whenever the clip lane rendered twice
// during the fetch (it is why every headless Models-tab harness hung).
let libraryFetch = null;
function refreshLibraryClips() {
  if (libraryFetch) return libraryFetch;
  libraryFetching = true;
  libraryFetch = (async () => {
    try {
      const r = await fetch('/api/models', { cache: 'no-store' });
      const j = await r.json();
      libraryClips = (j.files || []).filter(f => f.dir === 'anims' && /\.json$/i.test(f.name))
        .map(f => ({ name: f.name.replace(/\.json$/i, ''), path: f.path }));
    } catch {
      libraryClips = [];
    } finally {
      libraryFetching = false;
      libraryFetch = null;
    }
  })();
  return libraryFetch;
}

/** The clip's file form: what the engine's LoadClipLibrary reads. */
function libraryClipDoc(name, clip) {
  const s = sc();
  const vpm = Number.isFinite(+s.sidecarVoxelsPerMetre) ? +s.sidecarVoxelsPerMetre : 10;
  return Object.assign({ name, sidecarVoxelsPerMetre: vpm }, clip);
}

async function saveClipToLibrary(name) {
  const C = clips();
  const c = C[name];
  if (!c) return;
  const file = prompt('library clip name (assets/anims/<name>.json)', name);
  if (!file) return;
  if (!/^[A-Za-z0-9_\-]+$/.test(file))
    return toast('clip file names are letters, digits, _ and - only', true);
  if (!libraryClips) await refreshLibraryClips();
  if ((libraryClips || []).some(l => l.name === file) &&
      !confirm(`assets/anims/${file}.json exists — overwrite it?`)) return;
  try {
    const body = JSON.stringify(libraryClipDoc(file, c), null, 2) + '\n';
    const r = await fetch('/api/model?path=' + encodeURIComponent('anims/' + file + '.json'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body,
    });
    const j = await r.json().catch(() => ({ ok: false }));
    if (!j.ok) throw new Error(j.error || ('HTTP ' + r.status));
    await refreshLibraryClips();
    toast(`saved anims/${file}.json — name it in an attack style\'s clip field; ` +
          'press R in the game to hot-reload');
    renderClipLane();
  } catch (e) {
    toast('library save failed: ' + (e.message || e), true);
  }
}

async function ensureClipOnSkeleton(name) {
  if (!skel || !name) return false;
  if (skel.clips.some(c => c.name === name)) return true;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent('anims/' + name + '.json'),
                          { cache: 'no-store' });
    if (!r.ok) return false;
    const doc = await r.json();
    const clip = Object.assign({}, doc);
    delete clip.name; delete clip.sidecarVoxelsPerMetre;
    if (!clip.tracks || typeof clip.tracks !== 'object') clip.tracks = {};
    clips()[name] = clip;
    rebuildSkeleton();
    return true;
  } catch { return false; }
}

async function importClipFromLibrary(name) {
  const C = clips();
  if (C[name] && !confirm(`this rig already has a clip "${name}" — replace it ` +
                          'with the library copy?')) return;
  try {
    const r = await fetch('/api/model?path=' + encodeURIComponent('anims/' + name + '.json'),
                          { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const doc = await r.json();
    const clip = Object.assign({}, doc);
    delete clip.name; delete clip.sidecarVoxelsPerMetre;
    if (!clip.tracks || typeof clip.tracks !== 'object') clip.tracks = {};
    C[name] = clip;
    touched();
    activeClip = name; clipCursorMs = 0; selectedKey = null;
    reseedPose();
    toast(`imported anims/${name}.json into this rig\'s clips (save the model to keep it)`);
    renderAllPanels();
  } catch (e) {
    toast('library import failed: ' + (e.message || e), true);
  }
}

/** A <select> of the library, or nothing while it is still unknown. */
function libraryClipPicker() {
  if (!libraryClips) { refreshLibraryClips().then(() => renderClipLane()); return null; }
  const sel = el('select', {
    class: 'small',
    title: 'bring a clip in from the shared library (assets/anims/) — it is ' +
      'copied into this rig\'s sidecar so you can edit it here, then → library ' +
      'writes it back',
  });
  sel.append(el('option', { value: '' },
    libraryClips.length ? '← library…' : '← library (empty)'));
  for (const l of libraryClips) sel.append(el('option', { value: l.name }, l.name));
  sel.addEventListener('change', () => {
    const n = sel.value; sel.value = '';
    if (n) importClipFromLibrary(n);
  });
  return sel;
}

function renderClipLane() {
  if (!clipWrap) return;
  clipWrap.innerHTML = '';
  const C = clips();
  const names = Object.keys(C);

  const bar = el('div', { class: 'tagbar' }, el('span', { class: 'hint' }, 'clips'));
  for (const n of names) {
    // THE LOCOMOTION FAMILY IS FIVE RESERVED NAMES, and nothing in the schema
    // says so — avatar.cpp resolves `idle` / `walk` / `run` / `fall` / `hang`
    // by string and drives them exclusively, so a clip called `walk` behaves
    // completely differently from one called `walk2`. Marking them is the
    // cheapest way to stop an author renaming one and losing the arm swing.
    const loco = LOCO_FAMILY.includes(n);
    bar.append(el('button', {
      class: 'small' + (activeClip === n ? ' on' : ''),
      title: loco
        ? `"${n}" is a LOCOMOTION clip: the engine (and the gait preview) ` +
          'starts it by name and retires the rest of the family. Renaming it ' +
          'silently stops it ever playing.'
        : 'a plain clip: something has to PlayClip it by name',
      onclick: () => {
        activeClip = activeClip === n ? null : n;
        clipCursorMs = 0; selectedKey = null;
        clipPlaying = false;
        reseedPose();
        renderAllPanels();
      },
    }, loco ? '◆ ' + n : n));
  }
  bar.append(
    el('button', {
      class: 'small',
      onclick: () => {
        const n = prompt('clip name (attack / hurt / idle)', 'attack');
        if (!n) return;
        if (C[n]) return toast('a clip with that name exists', true);
        C[n] = { durationMs: 500, loop: false, mode: 'override',
                 blendInMs: 60, blendOutMs: 120, tracks: {} };
        touched();
        activeClip = n; clipCursorMs = 0;
        renderAllPanels();
      },
    }, '+ clip'),
    activeClip ? el('button', {
      class: 'small danger',
      onclick: () => {
        if (!confirm(`Delete clip "${activeClip}"?`)) return;
        delete C[activeClip];
        activeClip = null; selectedKey = null; poseEdit = null;
        touched(); renderAllPanels();
      },
    }, '✕ clip') : null,
    activeClip ? el('button', {
      class: 'small',
      title: 'rename',
      onclick: () => {
        const n = prompt('rename clip', activeClip);
        if (!n || n === activeClip) return;
        if (C[n]) return toast('a clip with that name exists', true);
        C[n] = C[activeClip]; delete C[activeClip];
        activeClip = n; touched(); renderAllPanels();
      },
    }, 'rename') : null,
    // ---- THE CLIP LIBRARY (assets/anims/<name>.json) ----------------------
    // A clip lives in ONE sidecar until it is put here. The library is what an
    // attack style names (attack_styles.json `clip`, strokes.h
    // AttackStyle::clip): LoadMobDefs compiles every library clip onto every
    // rig whose part names it fits, so a swing animation authored once on the
    // human plays on every human-shaped creature. Saving the MODEL still saves
    // the sidecar's own copy; this is the extra file.
    activeClip ? el('button', {
      class: 'small primary',
      title: 'save this clip to the shared library as assets/anims/<name>.json ' +
        '— the file an attack style can name in its `clip` field. The rig\'s ' +
        'own copy stays in the sidecar; a sidecar clip of the same name wins ' +
        'over the library on that rig.',
      onclick: () => saveClipToLibrary(activeClip),
    }, '→ library') : null,
    libraryClipPicker(),
    el('span', { class: 'spacer' }),
    activeClip ? el('button', {
      class: 'small' + (autoKey ? ' on' : ''),
      title: 'auto-key: posing a part writes a key at the cursor',
      onclick: () => { autoKey = !autoKey; renderClipLane(); },
    }, 'auto-key') : null,
    activeClip ? el('button', {
      class: 'small primary', title: 'write a key at the cursor [I]',
      onclick: () => writeKey(),
    }, 'Key [I]') : null,
    activeClip ? el('button', {
      class: 'small' + (clipPlaying ? ' on' : ''),
      onclick: () => { clipPlaying = !clipPlaying; reseedPose(); renderClipLane(); },
    }, clipPlaying ? '■ stop [P]' : '▶ play [P]') : null,
    activeClip ? el('button', {
      class: 'small' + (clipShowAll ? ' on' : ''),
      title: 'show a row for EVERY rigged part, not just the keyed ones — what ' +
        'you want while blocking out a new clip, when nothing has keys yet',
      onclick: () => { clipShowAll = !clipShowAll; renderClipLane(); },
    }, 'all parts') : null,
    activeClip ? el('button', {
      class: 'small', title: 'zoom the lane out',
      onclick: () => { CLIP_PX_PER_MS = Math.max(0.08, CLIP_PX_PER_MS / 1.5);
                       renderClipLane(); },
    }, '−') : null,
    activeClip ? el('button', {
      class: 'small', title: 'zoom the lane in — a 2 s clip is unreadable at ' +
        'the default 0.45 px/ms',
      onclick: () => { CLIP_PX_PER_MS = Math.min(4, CLIP_PX_PER_MS * 1.5);
                       renderClipLane(); },
    }, '+') : null);
  clipWrap.append(bar);

  const c = clipObj();
  if (!c) {
    clipWrap.append(el('div', { class: 'rignote' },
      'No clip selected. Clips are keyframed poses layered over the gait; ' +
      'the engine samples them with nlerp between fused quat+pos keys.'));
    return;
  }

  /* ---- clip properties ---- */
  const props = el('div', { class: 'clipprops' });
  props.append(field('durationMs', numInput(c, 'durationMs', { int: true, ph: '500' })));
  const loopC = el('input', { type: 'checkbox' });
  loopC.checked = !!c.loop;
  loopC.addEventListener('change', () => { c.loop = loopC.checked; touched(); });
  props.append(field('loop', loopC));
  const modeS = el('select', { class: 'cell' });
  for (const m of ['override', 'additive'])
    modeS.append(el('option', { value: m }, m));
  modeS.value = c.mode || 'override';
  modeS.addEventListener('change', () => { c.mode = modeS.value; touched(); });
  props.append(field('mode', modeS,
    c.mode === 'additive' ? 'delta vs the clip\'s OWN frame 0' : 'replaces the base pose'));
  props.append(field('blendInMs', numInput(c, 'blendInMs', { int: true, ph: '0' })));
  props.append(field('blendOutMs', numInput(c, 'blendOutMs', { int: true, ph: '0' }),
    'non-looping clips only'));
  clipWrap.append(props);

  /* ---- mask ---- */
  const maskWrap = el('div', { class: 'clipmask' },
    el('span', { class: 'hint' }, 'mask'));
  const L = limbs();
  if (!Array.isArray(c.mask)) c.mask = [];
  for (const limb of L) {
    const on = c.mask.includes(limb.name);
    maskWrap.append(el('button', {
      class: 'small' + (on ? ' on' : ''),
      title: on ? 'in the mask' : 'not masked',
      onclick: () => {
        const i = c.mask.indexOf(limb.name);
        if (i >= 0) c.mask.splice(i, 1); else c.mask.push(limb.name);
        // An EMPTY mask means "affects all parts" (anim.h:77). Drop the key
        // entirely rather than leaving [] behind, which reads the same to the
        // engine but is noise in the file.
        if (!c.mask.length) delete c.mask;
        touched(); renderClipLane();
      },
    }, limb.name));
  }
  maskWrap.append(el('span', { class: 'hint' },
    (!c.mask || !c.mask.length) ? '(empty = all parts)' : ''));
  clipWrap.append(maskWrap);

  /* ---- scrubber ---- */
  const dur = Math.max(1, num(c.durationMs, 500));
  const laneW = Math.max(160, dur * CLIP_PX_PER_MS);
  const scrub = el('div', { class: 'cliplane', style: `width:${laneW}px` });
  scrub.addEventListener('pointerdown', e => {
    const r = scrub.getBoundingClientRect();
    const setT = ev => {
      clipCursorMs = clamp(Math.round((ev.clientX - r.left) / CLIP_PX_PER_MS), 0, dur);
      // No re-render mid-drag (it would rebuild the element under the
      // pointer): move the cursor marker and re-seed the pose preview.
      reseedPose();
      updateClipCursorUI();
      ed.invalidate();
    };
    setT(e);
    const mv = ev => setT(ev);
    const up = () => { window.removeEventListener('pointermove', mv);
                       window.removeEventListener('pointerup', up); };
    window.addEventListener('pointermove', mv);
    window.addEventListener('pointerup', up);
  });
  // tick marks every 100ms
  for (let t = 0; t <= dur; t += 100)
    scrub.append(el('span', { class: 'ctick', style: `left:${t * CLIP_PX_PER_MS}px` },
      el('i', {}, t + '')));
  scrub.append(el('span', { class: 'ccursor', style: `left:${clipCursorMs * CLIP_PX_PER_MS}px` }));
  clipWrap.append(el('div', { class: 'clipscrubwrap' },
    el('span', { class: 'hint cliptime' }, Math.round(clipCursorMs) + ' ms'), scrub));

  /* ---- per-part key rows ---- */
  const rows = el('div', { class: 'cliprows' });
  for (const limb of L) {
    const tr = c.tracks?.[limb.name];
    const times = keyTimes(tr);
    // Keep it tight by default; "all parts" is for blocking out a new clip.
    if (!times.length && limb.name !== selectedPart && !clipShowAll) continue;
    const row = el('div', { class: 'cliprow' + (limb.name === selectedPart ? ' on' : '') });
    row.append(el('span', {
      class: 'cliplbl',
      onclick: () => { selectedPart = limb.name; bindGizmo(); renderAllPanels(); },
    }, limb.name));
    const lane = el('div', { class: 'cliplane keys', style: `width:${laneW}px` });
    for (const t of times) {
      const sel = selectedKey && selectedKey.part === limb.name && selectedKey.tMs === t;
      lane.append(el('span', {
        class: 'ckey' + (sel ? ' on' : ''),
        style: `left:${t * CLIP_PX_PER_MS}px`,
        title: t + ' ms',
        onclick: e => {
          e.stopPropagation();
          selectedKey = { part: limb.name, tMs: t };
          clipCursorMs = t;
          selectedPart = limb.name;
          bindGizmo();
          ed.invalidate();
          renderAllPanels();
        },
      }));
    }
    row.append(lane);
    rows.append(row);
  }
  clipWrap.append(rows);

  /* ---- selected key inspector ---- */
  if (selectedKey) {
    const tr = c.tracks?.[selectedKey.part];
    const rk = (tr?.rot || []).find(k => (+k.t || 0) === selectedKey.tMs);
    const pk = (tr?.pos || []).find(k => (+k.t || 0) === selectedKey.tMs);
    const box = el('div', { class: 'clipprops' },
      el('span', { class: 'hint' }, `key ${selectedKey.part} @ ${selectedKey.tMs}ms`));

    const tIn = el('input', { class: 'cell num', type: 'number', step: '10' });
    tIn.value = selectedKey.tMs;
    tIn.addEventListener('change', () => {
      const nt = Math.max(0, Math.round(+tIn.value || 0));
      for (const k of [rk, pk]) if (k) k.t = nt;
      selectedKey.tMs = nt;
      sortTrack(tr);
      touched(); renderClipLane();
    });
    box.append(field('t (ms)', tIn));

    const eIn = el('select', { class: 'cell' });
    for (const e of AN.EASES) eIn.append(el('option', { value: e }, e));
    eIn.value = (rk?.ease || pk?.ease || 'linear');
    eIn.addEventListener('change', () => {
      // Easing belongs to the OUTGOING key (anim.cpp:174) and the engine fuses
      // rot+pos into one key, so both channels must carry the same value.
      for (const k of [rk, pk]) if (k) k.ease = eIn.value;
      touched(); renderClipLane();
    });
    box.append(field('ease', eIn, 'applies to the segment AFTER this key'));

    box.append(el('div', { class: 'rigbtns' },
      el('button', {
        class: 'small',
        title: 'copy this key\'s pose',
        onclick: () => {
          clipboardKey = {
            rot: rk?.q ? [...rk.q] : null,
            pos: pk?.v ? [...pk.v] : null,
            ease: rk?.ease || pk?.ease || 'linear',
          };
          toast(`copied ${selectedKey.part} @ ${selectedKey.tMs}ms`);
          renderClipLane();
        },
      }, 'copy'),
      clipboardKey ? el('button', {
        class: 'small',
        title: 'paste onto the SELECTED part at the cursor',
        onclick: () => pasteKey(false),
      }, 'paste') : null,
      clipboardKey ? el('button', {
        class: 'small',
        title: 'paste MIRRORED onto the opposite limb (.L <-> .R) at the ' +
          'cursor. A walk cycle is one arm authored twice, half a period ' +
          'apart, and doing that by hand is where sign errors come from.',
        onclick: () => pasteKey(true),
      }, 'mirror →') : null,
      el('button', {
        class: 'small danger',
        onclick: () => {
          if (tr?.rot) tr.rot = tr.rot.filter(k => (+k.t || 0) !== selectedKey.tMs);
          if (tr?.pos) tr.pos = tr.pos.filter(k => (+k.t || 0) !== selectedKey.tMs);
          if (tr && !tr.rot?.length) delete tr.rot;
          if (tr && !tr.pos?.length) delete tr.pos;
          if (tr && !tr.rot && !tr.pos) delete c.tracks[selectedKey.part];
          selectedKey = null;
          touched(); renderAllPanels();
        },
      }, 'delete key')));
    clipWrap.append(box);
  }

  clipWrap.append(el('div', { class: 'hint' },
    'click a lane to scrub · I key (no ring drag = holds the sampled pose) · ' +
    'P play clip · select a part then drag the rotation rings to pose · ' +
    'auto-key writes on release · ± zooms the lane'));
}

/**
 * Paste the copied key at the cursor, optionally MIRRORED to the opposite limb.
 *
 * THE MIRROR IS ABOUT THE MODEL'S X AXIS, which is the plane every one of
 * these rigs is built symmetric about (`.L` limbs sit at high model x and `.R`
 * at low — mob.cpp:8130 says so where it derives HandSign from the name). For
 * a quaternion that reflection is (x, -y, -z, w): the x component survives
 * because a rotation ABOUT the mirror axis is unchanged by it, and the other
 * two flip because their axes do.
 *
 * The target limb is the same name with .L and .R swapped; with no such limb
 * it pastes onto the selected part and says so, rather than silently doing
 * nothing.
 */
function pasteKey(mirror) {
  const c = clipObj();
  if (!c || !clipboardKey) return;
  let part = selectedPart;
  if (mirror && part) {
    const other = part.endsWith('.L') ? part.slice(0, -2) + '.R'
                : part.endsWith('.R') ? part.slice(0, -2) + '.L' : null;
    if (other && limbByName(other)) part = other;
    else toast(`no mirror of "${selectedPart}" in this rig — pasted in place`, true);
  }
  if (!part || !limbByName(part)) return toast('select a part first', true);
  const t = Math.round(clipCursorMs);
  const tr = trackFor(part);
  const q = clipboardKey.rot;
  const v = clipboardKey.pos;
  if (q) {
    const m = mirror ? [q[0], -q[1], -q[2], q[3]] : [...q];
    tr.rot = (tr.rot || []).filter(k => (+k.t || 0) !== t);
    tr.rot.push({ t, q: m, ease: clipboardKey.ease });
  }
  if (v) {
    // A POSITION mirrors on the axis itself, not on the rotation's rule: the
    // reflection negates x and leaves y and z, which is the opposite pattern
    // to the quaternion above and is the easiest thing here to get backwards.
    const m = mirror ? [-v[0], v[1], v[2]] : [...v];
    tr.pos = (tr.pos || []).filter(k => (+k.t || 0) !== t);
    tr.pos.push({ t, v: m, ease: clipboardKey.ease });
  }
  sortTrack(tr);
  selectedKey = { part, tMs: t };
  selectedPart = part;
  touched();
  bindGizmo();
  renderAllPanels();
}

// Playback-rate UI updates: move the cursor marker and the ms label without
// rebuilding the lane (which would tear focus and event state).
function updateClipCursorUI() {
  const cur = clipWrap?.querySelector('.ccursor');
  if (cur) cur.style.left = (clipCursorMs * CLIP_PX_PER_MS) + 'px';
  const lbl = clipWrap?.querySelector('.cliptime');
  if (lbl) lbl.textContent = Math.round(clipCursorMs) + ' ms';
}

function sortTrack(tr) {
  if (tr?.rot) tr.rot.sort((a, b) => (+a.t || 0) - (+b.t || 0));
  if (tr?.pos) tr.pos.sort((a, b) => (+a.t || 0) - (+b.t || 0));
}

/**
 * Write the current pose edit into the active clip at the cursor.
 * Produces exactly the schema shape: rot keys carry {t,q,ease}, pos keys
 * {t,v,ease} (mob.cpp:356-367).
 */
function writeKey() {
  const c = clipObj();
  if (!c) { toast('select or create a clip first', true); return; }
  if (!selectedPart) { toast('select a part to key', true); return; }
  const tr = trackFor(selectedPart);
  const t = Math.round(clipCursorMs);

  // No ring drag in progress => key the pose the clip SAMPLES at the cursor
  // (a "hold" key). Keying identity instead would snap the limb to rest,
  // which is never what an untouched Key press means.
  const sampled = sampleClipPose(selectedPart, t);
  const editing = poseEdit && poseEdit.part === selectedPart;
  const rot = editing ? poseEdit.rot : sampled.rot;
  const pos = editing ? poseEdit.pos : sampled.pos;

  tr.rot = tr.rot || [];
  let rk = tr.rot.find(k => (+k.t || 0) === t);
  if (!rk) { rk = { t, ease: 'linear' }; tr.rot.push(rk); }
  rk.q = [round4(rot.x), round4(rot.y), round4(rot.z), round4(rot.w)];

  // Only write a pos key when there is an actual offset: an all-zero pos track
  // costs file size and makes the engine's hasPos flag true for no reason.
  if (Math.abs(pos.x) > 1e-6 || Math.abs(pos.y) > 1e-6 || Math.abs(pos.z) > 1e-6) {
    tr.pos = tr.pos || [];
    let pk = tr.pos.find(k => (+k.t || 0) === t);
    if (!pk) { pk = { t, ease: rk.ease }; tr.pos.push(pk); }
    pk.v = [round4(pos.x), round4(pos.y), round4(pos.z)];
  }
  sortTrack(tr);
  selectedKey = { part: selectedPart, tMs: t };
  touched();
  // The pose now lives in the track — sample it back so the rings and the
  // preview agree with what was actually written.
  reseedPose();
  toast(`keyed ${selectedPart} @ ${t}ms`);
  renderAllPanels();
}

const round4 = v => Math.round(v * 10000) / 10000;

function moveFrame(from, to) {
  const f = frameList();
  if (!f) return;
  const [x] = f.splice(from, 1);
  f.splice(to, 0, x);
  touched();
  frameIndex = to;
  renderTimeline();
}

function duplicateFrame() {
  const f = frameList();
  if (!f || !f.length) { toast('create a tag first', true); return; }
  f.splice(frameIndex + 1, 0, { ...f[frameIndex] });
  touched();
  gotoFrame(frameIndex + 1);
}

function deleteFrame() {
  const f = frameList();
  if (!f) { toast('create a tag first', true); return; }
  if (f.length <= 1) { toast('a flipbook needs at least one frame', true); return; }
  f.splice(frameIndex, 1);
  touched();
  gotoFrame(Math.min(frameIndex, f.length - 1));
}

/* ==========================================================================
   7. onion skinning

   Ghost instances appended to the SAME InstancedMesh as the live model, so
   they cost no extra draw call. Two deliberate choices:

   - The range WRAPS the loop, so frame 0 shows the last frame as its
     predecessor. Aseprite does not do this and it makes cycle-closing a
     guessing game; a walk cycle is a loop, so its onion skin should be too.
   - A ghost is drawn only where the CURRENT frame is empty. Overlapping cells
     would otherwise wash out the frame you are actually editing, which defeats
     the purpose.

   Depth-test-on / depth-write-off is handled by the shared material: the
   ghosts are drawn as ordinary opaque instances tinted toward red/blue, which
   reads correctly because they never overlap the live frame.
   ========================================================================== */

const ONION_PREV = { r: 1.0, g: 0.25, b: 0.25 };
const ONION_NEXT = { r: 0.3, g: 0.5, b: 1.0 };

// Where the current frame is DRAWN: during playback of a part flipbook the
// engine shows it in the part's slot; otherwise it sits at its model's own
// prefab offset (the editing view).
function frameDrawOffset() {
  if (playing) {
    const f = frameList()?.[frameIndex];
    if (f && f.part) {
      const mi = modelIndexByName(f.part);
      if (mi >= 0) return ed.getModels()[mi].offset;
    }
  }
  return ed.getModels()[frameModel(frameIndex)]?.offset;
}

function appendOnionInstances(cubes, n, cap, m4, col) {
  if (!onionOn) return n;
  const total = frameCount();
  if (total < 2) return n;

  const models = ed.getModels();
  const cur = models[frameModel(frameIndex)];
  if (!cur) return n;
  // Ghost frames are drawn IN the current frame's box, whatever offsets their
  // models are parked at in the file — flipbook frames replace each other in
  // place in the engine, so an aligned onion is the only useful one. Cells
  // compare in LOCAL coordinates for the same reason.
  const base = frameDrawOffset() || cur.offset;

  for (let d = 1; d <= onionRange; d++) {
    if (d >= total) break;
    for (const dir of [-1, 1]) {
      // Wrap around the active tag range (see banner).
      const fi = ((frameIndex + dir * d) % total + total) % total;
      if (fi === frameIndex) continue;
      const m = models[frameModel(fi)];
      if (!m || m === cur) continue;
      const tint = dir < 0 ? ONION_PREV : ONION_NEXT;
      const fade = 0.5 / d;              // opacity falloff by distance

      for (let z = 0; z < m.dim.z; z++)
        for (let y = 0; y < m.dim.y; y++)
          for (let x = 0; x < m.dim.x; x++) {
            if (n >= cap) return n;
            const v = m.grid.data[x + y * m.dim.x + z * m.dim.x * m.dim.y];
            if (!v) continue;
            // Only where the live frame is empty (same local cell).
            if (x < cur.dim.x && y < cur.dim.y && z < cur.dim.z &&
                cur.grid.data[x + y * cur.dim.x + z * cur.dim.x * cur.dim.y])
              continue;
            m4.makeTranslation(x + 0.5 + base.x, y + 0.5 + base.y,
                               z + 0.5 + base.z);
            cubes.setMatrixAt(n, m4);
            col.setRGB(tint.r * fade, tint.g * fade, tint.b * fade);
            cubes.setColorAt(n, col);
            n++;
          }
    }
  }
  return n;
}

/* ==========================================================================
   8. keyboard + wiring
   ========================================================================== */

// Returns true when the key was consumed. editor.js calls this first.
function onKey(ev) {
  const k = ev.key.toLowerCase();
  if (k === ' ' || ev.code === 'Space') { togglePlay(); return true; }
  if (k === 'o') { onionOn = !onionOn; ed.invalidate(); renderTimeline(); return true; }
  if (k === 'k') { setGait(!gaitOn); renderAllPanels(); return true; }
  if (k === 'p') { clipPlaying = !clipPlaying; reseedPose(); renderClipLane(); return true; }
  if (k === 'i') { writeKey(); return true; }
  if (k === '[') { gotoFrame(frameIndex - 1); return true; }
  if (k === ']') { gotoFrame(frameIndex + 1); return true; }
  if (k === 'd') { duplicateFrame(); return true; }
  if (k === 'delete') {
    // Del removes the selected KEY when the clip lane owns the selection,
    // otherwise a flipbook frame. Falls through to the editor's delete-selection
    // when neither applies.
    if (selectedKey) {
      const c = clipObj(), tr = c?.tracks?.[selectedKey.part];
      if (tr) {
        if (tr.rot) tr.rot = tr.rot.filter(x => (+x.t || 0) !== selectedKey.tMs);
        if (tr.pos) tr.pos = tr.pos.filter(x => (+x.t || 0) !== selectedKey.tMs);
        if (!tr.rot?.length) delete tr.rot;
        if (!tr.pos?.length) delete tr.pos;
        if (!tr.rot && !tr.pos) delete c.tracks[selectedKey.part];
        selectedKey = null;
        touched(); renderAllPanels();
        return true;
      }
    }
    const f = frameList();
    if (f && f.length > 1) { deleteFrame(); return true; }
    return false;
  }
  return false;
}

// Advance playback and the preview runtime. Called from editor.js's rAF loop.
function tick(dt) {
  if (playing) {
    // Playback only moves the frame POINTER and the composed view (viewPlan);
    // it must never call setActiveModel — that resets the per-model undo
    // stack and, on a rig, marches the edit target through the limbs.
    const n = frameCount();
    if (n > 1) {
      frameClockMs += dt * 1000;
      let guard = 0, moved = false;
      while (frameClockMs >= frameMs(frameIndex) && guard++ < 64) {
        frameClockMs -= frameMs(frameIndex);
        frameIndex = (frameIndex + 1) % n;
        moved = true;
      }
      if (moved) { ed.invalidate(); updateFrameStrip(); }
    }
  }

  if (clipPlaying) {
    // Advance the cursor ourselves so the scrubber tracks playback;
    // stepPreview() re-pins the runtime clip instance to the cursor each
    // step, which keeps one source of truth for "where are we in the clip".
    const c = clipObj();
    const dur = Math.max(1, num(c?.durationMs, 500));
    clipCursorMs += dt * 1000;
    let ended = false;
    if (clipCursorMs >= dur) {
      if (c?.loop) clipCursorMs -= dur;
      else { clipCursorMs = dur; clipPlaying = false; ended = true; }
    }
    updateClipCursorUI();
    // The full lane re-render is reserved for the discrete end-of-clip event;
    // per-frame it would rebuild inputs mid-typing and eat the play button.
    if (ended) { reseedPose(); renderClipLane(); }
  }

  // The effector follows the selected style even on a still rig — see
  // syncEffectorToSelection for why this is not inside the preview loop.
  syncEffectorToSelection();
  if (previewActive()) {
    stepPreview(dt);
    renderGaitReadout();
  }
  // The Attacks lane's live numbers move every tick and its inputs must not be
  // rebuilt under the author's cursor, so it refreshes the readout IN PLACE —
  // the same split the clip lane makes between updateClipCursorUI() and a full
  // renderClipLane().
  if (laneTab === 'attacks') ATK.tickUI();
  drawAngleGuide();
  drawPoseHandles();
  drawAimOrb();
}

/* --------------------------------------------------------------------------
   THE AIM ORB — the target the stroke is aimed at (aimTargetScene), with a
   faint line from the eyes that look at it. Toggled by the panel's `orb` chip.
   -------------------------------------------------------------------------- */
let aimOrbSig = '';
function drawAimOrb() {
  const lib = laneTab === 'attacks' && ATK.aimOrbEnabled?.() ? ATK.library() : null;
  const sty = lib ? lib.styles[ATK.styleIndex()] : null;
  const pos = sty && melee ? aimTargetScene() : null;
  const eye = pos ? aimEyeScene() : null;
  if (!pos || !eye) {
    if (aimOrbSig) { aimOrbSig = ''; ed.setAimOrb?.(null); ed.invalidate(); }
    return;
  }
  const S = restFor()?.S || rigScale();
  const sig = [pos.x, pos.y, pos.z, S].map(v => v.toFixed(3)).join('|');
  // Re-sent every frame for the ring's billboard; the redraw is only asked
  // for when the orb itself moved.
  ed.setAimOrb?.({ pos: [pos.x, pos.y, pos.z], radius: 0.35 * S,
                   from: [eye.x, eye.y, eye.z] });
  if (sig !== aimOrbSig) { aimOrbSig = sig; ed.invalidate(); }
}

/* --------------------------------------------------------------------------
   MANUAL ADJUSTMENT MODE — pose the HELD frame by dragging the arm.

   Handles (editor.js setPoseHandles) sit on the tip, the shoulder, the upper
   arm, the elbow and the wrist. A drag is turned into the frame's own numbers
   by a small SCREEN-SPACE SOLVE: for each number the handle controls, the
   point it moves is re-evaluated a hair further along (the driver's own
   geometry, analytically — no rig re-solve) and projected, which says how
   many pixels one unit of that number moves the handle on THIS camera; the
   change that best follows the mouse is then solved for. So the grabbed
   point tracks the cursor from any view, and every number that changes is a
   real field in the frame's row, undoable as one step per drag.

     tip        az + el          (the point follows the mouse)
     shoulder   click: shows az ↔ and el ↕, one number each
     upper arm  az + el
     reach      extension along the arm line
     elbow      elbow direction (swings round the shoulder-to-hand line)
     wrist      blade angle + lean angle (the hand swings round the tip)
   -------------------------------------------------------------------------- */
let poseShoulderOpen = false;
let poseHandlesOn = false;
function drawPoseHandles() {
  const on = laneTab === 'attacks' && ATK.manualMode?.() && melee && anim?.model?.length;
  const key = on ? ATK.goalFrame?.() : null;
  const lib = key ? ATK.library() : null;
  const sty = lib ? lib.styles[ATK.styleIndex()] : null;
  const seed = sty ? weaponArmSeed() : null;
  if (!key || !sty || !seed || resolveEffector().mode === 'aim') {
    if (poseHandlesOn) { ed.setPoseHandles?.(null); poseHandlesOn = false; }
    return;
  }
  poseHandlesOn = true;
  // A FRAME THAT IS A POSE is posed by its JOINTS (drawKeyedHandles below).
  if (heldKeyed) { drawKeyedHandles(key, sty); return; }
  const S = seed.S || rigScale();
  const off = lungeOffsetModel();
  const sh = AN.vadd(seed.shoulder, off);
  // The driver's L-space (x = the wielder's right, which is scene -X at
  // heading 0) to a scene point.
  const sc = v => [sh.x - v.x * S, sh.y + v.y * S, sh.z + v.z * S];
  const P = (v) => [v.x, v.y, v.z];
  const tipL = melee.tipL_, handL = melee.handL_;
  // Posed joints for the elbow and wrist handles (where the arm really is).
  const hand = { hand: -1 };
  const eff = resolveEffector();
  const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
  const ch = ci >= 0 ? skel.chains[ci] : null;
  const elbowPt = ch && anim.model[ch.parts[1]]
    ? AN.vadd(anim.model[ch.parts[1]].pos, off) : null;
  const upperMid = elbowPt ? AN.vmul(AN.vadd(sh, elbowPt), 0.5) : null;
  const handPt = sc(handL);
  const tipPt = sc(tipL);
  const reachPt = sc(AN.vmul(tipL, 1.12));
  const list = [
    { id: 'tip', pos: tipPt, text: '✥ tip', color: '#ffc857',
      title: 'drag: move the POINT (azimuth + elevation)' },
    { id: 'reach', pos: reachPt, text: '↔ reach', color: '#e8a0ff',
      title: 'drag along the arm: extension (how far out the tip is)' },
    { id: 'shoulder', pos: P(sh), text: poseShoulderOpen ? '● shoulder ▾' : '● shoulder',
      color: '#8fd3ff', active: poseShoulderOpen,
      title: 'click: show the az ↔ and el ↕ buttons (one number each)' },
  ];
  if (poseShoulderOpen) {
    list.push({ id: 'az', pos: [sh.x, sh.y + 1.6 * S, sh.z], text: 'az ↔', color: '#4fd1ff',
                title: 'drag: turn the arm left / right (azimuth only)' });
    list.push({ id: 'el', pos: [sh.x, sh.y - 1.6 * S, sh.z], text: 'el ↕', color: '#ff6bd6',
                title: 'drag: raise / lower the arm (elevation only)' });
  }
  if (upperMid) list.push({ id: 'upper', pos: P(upperMid), text: 'upper arm', color: '#8fd3ff',
    title: 'drag: move the arm (azimuth + elevation)' });
  if (elbowPt) list.push({ id: 'elbow', pos: P(elbowPt), text: '⟲ elbow', color: '#7cf03a',
    title: 'drag: which way the ELBOW points (swings it round the shoulder-to-hand line)' });
  list.push({ id: 'wrist', pos: handPt, text: '⟳ wrist', color: '#ff9f5a',
    title: 'drag: blade angle (off the arm line) + which side the hand sits' });
  ed.setPoseHandles?.(list, poseDragCb);
}

// ---- the screen-space solve ---------------------------------------------
const PX = (p) => ed.projectToScreen?.(p);
// Pixels per unit of each parameter: finite difference of a scene point.
function jac(fn, x0, eps) {
  const a = PX(fn(x0)), b = PX(fn(x0 + eps));
  return a && b ? [(b[0] - a[0]) / eps, (b[1] - a[1]) / eps] : null;
}
// ONE POINTER MOVE MAY TURN AN ANGLE THIS MUCH, at most. The solves below
// divide by how far the point moves on screen per radian, and near a
// degenerate pose (a blade along its own radius, an arm at the pole) that is
// almost nothing, so a pixel became tens of radians: the saved windup had a
// lean of -9.8 rad and its recover -571 and az -14.7, all of which the pose
// clamps or wraps into something harmless while the NUMBER keeps growing —
// until the program blends through it and spins the blade.
const kDragMaxStep = 0.15;
const capStep = v => Math.max(-kDragMaxStep, Math.min(kDragMaxStep, v || 0));
// Best change of one parameter for a drag (dx, dy).
function solve1(J, dx, dy) {
  const n = J ? J[0] * J[0] + J[1] * J[1] : 0;
  return n > 1e-6 ? (dx * J[0] + dy * J[1]) / n : 0;
}
// ...and of two (least squares on the 2x2).
function solve2(J1, J2, dx, dy) {
  if (!J1 || !J2) return [solve1(J1, dx, dy), solve1(J2, dx, dy)];
  const a = J1[0] * J1[0] + J1[1] * J1[1], b = J1[0] * J2[0] + J1[1] * J2[1];
  const c = J2[0] * J2[0] + J2[1] * J2[1];
  const r1 = J1[0] * dx + J1[1] * dy, r2 = J2[0] * dx + J2[1] * dy;
  const det = a * c - b * b;
  if (Math.abs(det) < 1e-9) return [solve1(J1, dx, dy), solve1(J2, dx, dy)];
  return [(c * r1 - b * r2) / det, (a * r2 - b * r1) / det];
}

// The driver's own conventions, in L-space.
const dirL = (az, el) => AN.v3(Math.cos(el) * Math.sin(az), Math.sin(el), Math.cos(el) * Math.cos(az));
// The driver's own reference (melee.js axisFrame), so a drag and the pose it
// edits measure the elbow and lean angles from the same "down".
const downSide = axis => MELEE.axisFrame(axis, melee.handSign_);
function rotAbout(axis, d, s, a) {
  return AN.vadd(AN.vmul(d, Math.cos(a)), AN.vmul(s, Math.sin(a)));
}

let poseDragMoved = 0;
const poseDragCb = {
  down: (id) => { poseDragMoved = 0; ATK.manualBegin?.(); },
  up: (id) => {
    if (id === 'shoulder' && poseDragMoved < 3) poseShoulderOpen = !poseShoulderOpen;
    ATK.manualEnd?.('pose: ' + id);
  },
  drag: (id, dx, dy) => {
    poseDragMoved += Math.abs(dx) + Math.abs(dy);
    const seed = weaponArmSeed();
    if (!seed || !melee) return;
    const S = seed.S || rigScale();
    const sh = AN.vadd(seed.shoulder, lungeOffsetModel());
    const sc = v => [sh.x - v.x * S, sh.y + v.y * S, sh.z + v.z * S];
    const tipL = melee.tipL_, handL = melee.handL_;
    const r = Math.max(AN.vlen(tipL), 1e-3);
    const az0 = melee.strokeAz(), el0 = melee.strokeEl();
    const E = 1e-3;
    if (id === 'tip' || id === 'upper' || id === 'az' || id === 'el') {
      // THE POINT, on its sphere about the shoulder.
      const Jaz = jac(a => sc(AN.vmul(dirL(a, el0), r)), az0, E);
      const Jel = jac(e => sc(AN.vmul(dirL(az0, e), r)), el0, E);
      let dAz = 0, dEl = 0;
      if (id === 'az') dAz = solve1(Jaz, dx, dy);
      else if (id === 'el') dEl = solve1(Jel, dx, dy);
      else [dAz, dEl] = solve2(Jaz, Jel, dx, dy);
      dAz = capStep(dAz); dEl = capStep(dEl);
      // KEPT INSIDE THE ARM'S WINDOW. The driver clamps the resolved bearing
      // to [azLo, azHi] x [elMin, elMax], so a number past it moves nothing
      // on screen and only grows; the frame's own value is bounded by the
      // window less whatever it is measured from.
      const t = melee.tuning;
      const azHi = melee.handSign_ > 0 ? t.azOut : t.azAcross;
      const azLo = melee.handSign_ > 0 ? -t.azAcross : -t.azOut;
      ATK.manualEdit?.((fr, parsed) => {
        const ref = parsed && parsed.fromBody ? { az: 0, el: 0 } : aimAtRest();
        const az = Math.max(azLo - ref.az, Math.min(azHi - ref.az, (+fr.az || 0) + dAz));
        const el = Math.max(t.elMin - ref.el, Math.min(t.elMax - ref.el, (+fr.el || 0) + dEl));
        fr.az = Math.round(az * 1e6) / 1e6;
        fr.el = Math.round(el * 1e6) / 1e6;
      });
    } else if (id === 'reach') {
      // EXTENSION: the point along its own line, in band units.
      const band = melee.reachBand();
      const span = Math.max(band.hi - band.lo, 1e-3);
      const dirT = AN.vnorm(tipL);
      const J = jac(x => sc(AN.vmul(dirT, r + x * span)), 0, E);
      const d = solve1(J, dx, dy);
      ATK.manualEdit?.(fr => {
        const lo = -MELEE.kNeutralReach, hi = 1 - MELEE.kNeutralReach;
        fr.reach = Math.round(Math.max(lo, Math.min(hi, (+fr.reach || 0) + d)) * 1e6) / 1e6;
      });
    } else if (id === 'elbow') {
      // THE ELBOW DIRECTION about the shoulder-to-hand line (melee.h).
      const hl = Math.max(AN.vlen(handL), 1e-3);
      const hDir = AN.vnorm(handL);
      const { down, side } = downSide(hDir);
      const reachOut = hl * 0.35;
      const at = a => sc(AN.vadd(AN.vmul(handL, 0.5),
                                 AN.vmul(rotAbout(hDir, down, side, a), reachOut)));
      const s0 = melee.elbowSwivelNow_;
      const d = capStep(solve1(jac(at, s0, E), dx, dy));
      ATK.manualEdit?.((fr, parsed) => {
        const base = parsed && parsed.elbowSet ? parsed.elbow : s0;
        let v = base + d;
        while (v > Math.PI) v -= 2 * Math.PI;
        while (v < -Math.PI) v += 2 * Math.PI;
        fr.elbow = Math.round(v * 1e6) / 1e6;
      });
    } else if (id === 'wrist') {
      // THE HAND round the tip: blade angle (off the arm line) + lean angle.
      const L = melee.bladeLen_;
      if (!(L > 1e-3)) return;
      const rad = AN.vnorm(tipL);
      const { down, side } = downSide(rad);
      const handAt = (th, ph) => sc(AN.vadd(AN.vsub(tipL, AN.vmul(rad, L * Math.cos(th))),
                                            AN.vmul(rotAbout(rad, down, side, ph), L * Math.sin(th))));
      const th0 = melee.bladeAngleNow_, ph0 = melee.leanAngleNow_;
      const Jth = jac(t => handAt(t, ph0), th0, E);
      const Jph = jac(p => handAt(th0, p), ph0, E);
      const [dTh, dPh] = solve2(Jth, Jph, dx, dy).map(capStep);
      ATK.manualEdit?.((fr, parsed) => {
        const th = (parsed && parsed.bladeAngle >= 0 ? parsed.bladeAngle : th0) + dTh;
        const ph = (parsed && parsed.lean === 'angle' ? parsed.leanAngle : ph0) + dPh;
        fr.bladeAngle = Math.round(Math.max(0, Math.min(Math.PI, th)) * 1e6) / 1e6;
        fr.lean = 'angle';
        fr.leanAngle = Math.round(MELEE.wrapPi(ph) * 1e6) / 1e6;
      });
    }
  },
};

/* --------------------------------------------------------------------------
   POSING A FRAME THAT IS A POSE (melee.js "A FRAME IS A POSE").

   The frame stores joint rotations, so its handles edit JOINTS, each drag
   solved against the STORED pose (keyedFK), never against the drawn model —
   the model is a preview tick behind the document, and solving against it
   would drop every pointer move that lands between two ticks.

     hand     move the hand; the arm re-solves (two bones, elbow kept on its
              side) and the BLADE KEEPS ITS DIRECTION
     tip      swing the blade about the hand (the wrist)
     elbow    swivel the elbow round the shoulder-to-hand line; hand and blade
              stay put
     roll     turn the blade about its own length (which way the edge faces)
     arm      turn the whole arm about the shoulder, blade and all
   -------------------------------------------------------------------------- */
// The rigid offset of part i from its parent, in the parent's frame, off the
// drawn model (it is a bone: it does not change with the pose).
function relPosOf(i) {
  const par = skel.parts[i].parent;
  if (par < 0 || !anim.model[par] || !anim.model[i]) return AN.v3();
  return AN.qrotinv(anim.model[par].rot, AN.vsub(anim.model[i].pos, anim.model[par].pos));
}
// The aim turn a held frame is shown with (keyedFramePose's rule).
function heldAimOf(fr) {
  if (!fr || fr.fromBody) return { yaw: 0, pitch: 0 };
  const a = aimAtRest();
  return { yaw: a.az, pitch: a.el };
}
/**
 * FORWARD KINEMATICS of a stored pose on the live rig: the three joints'
 * MODEL rotations (U upper arm, F forearm, W hand) and the points S shoulder,
 * E elbow, H hand, T blade tip. `ctx` carries what does not depend on the pose.
 */
function keyedCtx() {
  const parts = weaponArmParts();
  if (parts[0] < 0 || parts[1] < 0) return null;
  const par0 = skel.parts[parts[0]].parent;
  const P = par0 >= 0 && anim.model[par0] ? anim.model[par0].rot : AN.qid();
  const hp = parts[2];
  const handIdx = hp >= 0 ? hp : parts[1];
  // The blade tip in the HAND's frame, off the drawn model (rigid).
  const blade = resolveEffector().mode === 'held' ? bladeSegmentModel() : null;
  const hm = anim.model[handIdx];
  const tipRel = blade && hm ? AN.qrotinv(hm.rot, AN.vsub(blade.tip, hm.pos)) : AN.v3();
  const flatRel = blade && hm && blade.flat ? AN.qrotinv(hm.rot, blade.flat) : AN.v3(0, 0, 1);
  // THE WRIST'S HANDLE LEVER, in the hand's frame. With a blade it IS the
  // blade (tip + flat). An EMPTY hand -- a fist style, or a sword style
  // previewed unarmed -- has no blade, and the tip/roll handles were the only
  // way to turn the wrist, so it gets a stand-in: a pointer out of the fist
  // (toward the natural weapon's knuckles when there is one, else along the
  // forearm) and a flat square to it. HANDLES ONLY: tipRel stays zero, so
  // what a bake or a mirror measures (fk.T) is unchanged for a fist.
  let wristRel = tipRel, wristFlat = flatRel;
  // FIXED IN THE HAND'S FRAME, never re-read off the drawn forearm: a lever
  // that re-aimed itself along the arm every redraw would never show the bend.
  if (!blade && hp >= 0 && hm) {
    let local = null;
    const edge = effectorEdgeModel();
    if (edge && edge.part === hp) {
      const d = AN.qrotinv(hm.rot, AN.vsub(edge.tip, hm.pos));
      if (AN.vlen(d) > 1e-3) local = AN.vnorm(d);
    }
    if (!local) {
      // The forearm's line at REST, in the hand's own frame: the elbow-to-
      // wrist offset (rest.pos, forearm frame) un-turned by the hand's rest
      // rotation.
      const r = skel.parts[hp].rest;
      local = r && AN.vlen(r.pos) > 1e-3
        ? AN.qrotinv(r.rot || AN.qid(), AN.vnorm(r.pos)) : AN.v3(0, -1, 0);
    }
    wristRel = AN.vmul(local, 3 * rigScale());
    wristFlat = perps(local)[0];
  }
  return {
    parts, P, S: anim.model[parts[0]].pos,
    eOff: relPosOf(parts[1]),
    hOff: hp >= 0 ? relPosOf(hp) : skel.parts[parts[1]].rest.pos,
    wAnim: hp >= 0 ? weaponArmLocals(parts)[2] : null,
    tipRel, flatRel, wristRel, wristFlat, bladed: !!blade,
  };
}
function keyedFK(ctx, pose, aim) {
  const R = MELEE.aimQuat(aim.yaw, aim.pitch);
  const lu = AN.qnorm(AN.qmul(AN.qconj(ctx.P), AN.qmul(R, AN.qmul(ctx.P, pose.joint[0]))));
  const U = AN.qnorm(AN.qmul(ctx.P, lu));
  const F = AN.qnorm(AN.qmul(U, pose.joint[1]));
  const W = ctx.parts[2] >= 0
    ? AN.qnorm(AN.qmul(F, pose.joint[2] || ctx.wAnim || AN.qid())) : F;
  const E = AN.vadd(ctx.S, AN.qrot(U, ctx.eOff));
  const H = AN.vadd(E, AN.qrot(F, ctx.hOff));
  const T = AN.vadd(H, AN.qrot(W, ctx.tipRel));
  // The wrist handles' lever end: the blade tip, or an empty hand's stand-in.
  const Tw = AN.vadd(H, AN.qrot(W, ctx.wristRel || ctx.tipRel));
  return { U, F, W, S: ctx.S, E, H, T, Tw };
}
// Model rotations back to a STORED pose: parent-relative, the aim turn taken
// back off the shoulder.
function keyedStore(ctx, fk, aim, base) {
  const R = MELEE.aimQuat(aim.yaw, aim.pitch);
  const lu = AN.qnorm(AN.qmul(AN.qconj(ctx.P), fk.U));
  const stored = AN.qnorm(AN.qmul(AN.qconj(ctx.P),
    AN.qmul(AN.qconj(R), AN.qmul(ctx.P, lu))));
  return {
    joint: [stored, AN.qnorm(AN.qmul(AN.qconj(fk.U), fk.F)),
            ctx.parts[2] >= 0 ? AN.qnorm(AN.qmul(AN.qconj(fk.F), fk.W)) : null],
    twist: base.twist, pitch: base.pitch,
  };
}
/**
 * THE MIDDLE OF A CUT, for the bake (attacks.js bakeStyle): halfway between
 * two stored poses BY BEARING. Each end is turned rigidly about the shoulder
 * to the bearing halfway between the two tips (azimuth and elevation, the way
 * the old tip-target driver moved), and the two turned arms are then blended.
 * Slerping the two ends directly takes the shortest joint path, which for a
 * wide horizontal cut goes over the top of the head; a key made this way puts
 * the middle of the cut in front, where the old swing passed the target.
 */
function bakeMidPose(p0, p1) {
  const ctx = keyedCtx();
  if (!ctx || !p0 || !p1) return null;
  const zero = { yaw: 0, pitch: 0 };
  const f0 = keyedFK(ctx, p0, zero), f1 = keyedFK(ctx, p1, zero);
  const bearing = (fk) => {
    const d = AN.vsub(fk.T, fk.S);
    const r = Math.max(AN.vlen(d), 1e-4);
    return { az: Math.atan2(-d.x, d.z), el: Math.asin(clamp(d.y / r, -1, 1)) };
  };
  const b0 = bearing(f0), b1 = bearing(f1);
  const mid = { az: b0.az + MELEE.wrapPi(b1.az - b0.az) / 2, el: (b0.el + b1.el) / 2 };
  const toMid = (b, s) => {
    const R = AN.qmul(MELEE.aimQuat(mid.az, mid.el), AN.qconj(MELEE.aimQuat(b.az, b.el)));
    return AN.qnorm(AN.qmul(AN.qconj(ctx.P), AN.qmul(R, AN.qmul(ctx.P, s))));
  };
  const s0 = toMid(b0, p0.joint[0]), s1 = toMid(b1, p1.joint[0]);
  const sl = (a, b) => (a && b ? AN.qnorm(AN.qslerp(a, b, 0.5)) : (a || b || null));
  return {
    joint: [sl(s0, s1), sl(p0.joint[1], p1.joint[1]), sl(p0.joint[2], p1.joint[2])],
    twist: (p0.twist + p1.twist) / 2, pitch: (p0.pitch + p1.pitch) / 2,
  };
}
/**
 * A STORED POSE MIRRORED LEFT-RIGHT, still on the SAME arm (attacks.js
 * deriveStyle): the forehand's top-right windup becomes the backhand's
 * top-left one. Reflected through the SHOULDER's own vertical plane, not the
 * body's midline: a target frame's aim is measured from the shoulder, so a
 * stroke centred on the shoulder's "ahead" stays centred on it (mirrored about
 * the chest, the saved diagonal's whole sweep landed left of the target).
 *
 * A reflection S is not a rotation, so each bone's model rotation M becomes
 * S·M·D, D a reflection in the bone's OWN frame across a plane that contains
 * the bone (the elbow and hand stay on the reflected points, exactly, no
 * solve). D is the same for a bone in every frame, so the turn from one frame
 * to the next keeps its angle and the mirrored swing interpolates like the
 * original. (Re-solving each frame's arm by the shortest turn from the
 * original instead left the upper arm's twist un-mirrored, and the recover
 * slerped the wrist through ~180 degrees in two ticks — measured.) For the
 * upper arm and forearm the plane's normal is the elbow's hinge (local z, the
 * axis every authored elbow bends about), so the elbow bends the same way it
 * did; for the hand it is the blade's flat, so the blade keeps its line and
 * its cutting plane (which edge leads does not matter: melee.cpp
 * MeleeEdgeAlign takes |dot|). Torso twist flips; pitch stays. Zero aim: the
 * mirror is about "ahead", and the runtime turns the arm to the target after.
 */
function mirrorPose(pose) {
  const ctx = keyedCtx();
  if (!ctx || !pose) return null;
  const zero = { yaw: 0, pitch: 0 };
  const fk = keyedFK(ctx, pose, zero);
  // The plane's normal n: `want` made square to the bone `along`.
  const normal = (want, along) => {
    const t = AN.vlen(along) > 1e-4 ? AN.vnorm(along) : AN.v3(0, 1, 0);
    const n = AN.vsub(want, AN.vmul(t, AN.vdot(want, t)));
    return AN.vlen(n) > 1e-4 ? AN.vnorm(n) : perps(t)[0];
  };
  // S·M·S is (x, -y, -z, w); S times D (D across the plane of normal n) is
  // the rotation x̂·n̂ = (x̂ × n̂, -x̂·n̂).
  const reflect = (M, n) => AN.qnorm(AN.qmul({ x: M.x, y: -M.y, z: -M.z, w: M.w },
                                              { x: 0, y: -n.z, z: n.y, w: -n.x }));
  const hinge = AN.v3(0, 0, 1);
  const out = {
    U: reflect(fk.U, normal(hinge, ctx.eOff)),
    F: reflect(fk.F, normal(hinge, ctx.hOff)),
  };
  out.W = ctx.parts[2] >= 0 ? reflect(fk.W, normal(ctx.flatRel, ctx.tipRel)) : out.F;
  const stored = keyedStore(ctx, out, zero, pose);
  stored.twist = -(pose.twist || 0);
  stored.pitch = pose.pitch || 0;
  return stored;
}

/**
 * THE SAME ARM, THE OTHER ELBOW SOLUTION (attacks.js pose row "⇅ flip
 * elbow"). Dragging the hand around can leave a pose whose elbow bends
 * BACKWARDS with the upper arm twisted 180° to hide it: it reads fine as a
 * still, but the slerp to a neighbouring frame that is not flipped spins the
 * whole shoulder through the twist. This turns the upper arm and the forearm
 * each half a turn about their OWN bone: the elbow, hand and blade stay on
 * exactly the same points (no solve), the elbow's stored bend changes sign
 * and the shoulder loses (or gains) the 180° twist. The hand's model rotation
 * is kept, so the wrist absorbs the forearm's half turn. Applying it twice
 * gives back the original pose.
 */
function flipElbowPose(pose) {
  const ctx = keyedCtx();
  if (!ctx || !pose) return null;
  const zero = { yaw: 0, pitch: 0 };
  const fk = keyedFK(ctx, pose, zero);
  const halfTurn = (Q, along) => AN.vlen(along) > 1e-4
    ? AN.qnorm(AN.qmul(Q, AN.qaxisangle(AN.vnorm(along), Math.PI))) : Q;
  const out = { U: halfTurn(fk.U, ctx.eOff), F: halfTurn(fk.F, ctx.hOff), W: fk.W };
  // No hand bone: the forearm IS the hand, and it keeps its half turn.
  if (ctx.parts[2] < 0) out.W = out.F;
  return keyedStore(ctx, out, zero, pose);
}

const keyedPoseOfHeld = () => {
  const lib = ATK.library();
  const sty = lib ? lib.styles[ATK.styleIndex()] : null;
  const key = ATK.goalFrame?.();
  const k = typeof key === 'string' && key[0] === 'f' ? +key.slice(1) : -1;
  const fr = sty && k >= 0 ? sty.frames[k] : null;
  return fr && fr.pose ? fr : null;
};

function drawKeyedHandles() {
  const fr = keyedPoseOfHeld();
  const ctx = fr ? keyedCtx() : null;
  if (!ctx) { ed.setPoseHandles?.(null); return; }
  const fk = keyedFK(ctx, fr.pose, heldAimOf(fr));
  const P = v => [v.x, v.y, v.z];
  const S = rigScale();
  const list = [
    { id: 'khand', pos: P(fk.H), text: '✥ hand', color: '#ff9f5a',
      title: 'drag: MOVE THE HAND — the arm re-solves, the blade keeps pointing the same way' },
    { id: 'kelbow', pos: P(fk.E), text: '⟲ elbow', color: '#7cf03a',
      title: 'drag: swing the ELBOW round the shoulder-to-hand line (hand and blade stay put)' },
    { id: 'karm', pos: P(AN.vmul(AN.vadd(fk.S, fk.E), 0.5)), text: 'arm', color: '#8fd3ff',
      title: 'drag: turn the WHOLE ARM about the shoulder, blade and all' },
  ];
  // The WRIST: on the blade, or on an empty hand's stand-in pointer (keyedCtx
  // wristRel) so a fist can be bent and rolled too.
  if (AN.vlen(AN.vsub(fk.Tw, fk.H)) > 0.5 * S) {
    const bl = ctx.bladed;
    list.push({ id: 'ktip', pos: P(fk.Tw), text: bl ? '✥ tip' : '✥ wrist', color: '#ffc857',
      title: bl ? 'drag: point the BLADE (the wrist turns; the hand stays put)'
                : 'drag: BEND THE WRIST — point the fist (the hand stays put)' });
    const mid = AN.vmul(AN.vadd(fk.H, fk.Tw), 0.5);
    const flat = AN.qrot(fk.W, ctx.wristFlat);
    list.push({ id: 'kroll', pos: P(AN.vadd(mid, AN.vmul(AN.vnorm(flat), 1.5 * S))),
      text: '⟳ roll', color: '#e8a0ff',
      title: bl ? 'drag: ROLL the blade about its own length (which way the edge faces)'
                : 'drag: ROLL the fist about the forearm line (which way the knuckles face)' });
  }
  ed.setPoseHandles?.(list, keyedDragCb);
}

// Least-squares move of a 3D point for a screen drag: the minimum-norm
// displacement whose projection is (dx, dy).
function screenMove3(p, dx, dy) {
  const E = 1e-2;
  const at = d => [p.x + d.x, p.y + d.y, p.z + d.z];
  const J = [AN.v3(1, 0, 0), AN.v3(0, 1, 0), AN.v3(0, 0, 1)].map(ax => {
    const a = PX([p.x, p.y, p.z]), b = PX(at(AN.vmul(ax, E)));
    return a && b ? [(b[0] - a[0]) / E, (b[1] - a[1]) / E] : [0, 0];
  });
  // M = J J^T (2x2), solve M y = [dx, dy], d = J^T y.
  const m00 = J.reduce((s, j) => s + j[0] * j[0], 0);
  const m01 = J.reduce((s, j) => s + j[0] * j[1], 0);
  const m11 = J.reduce((s, j) => s + j[1] * j[1], 0);
  const det = m00 * m11 - m01 * m01;
  if (Math.abs(det) < 1e-9) return AN.v3();
  const y0 = (m11 * dx - m01 * dy) / det, y1 = (m00 * dy - m01 * dx) / det;
  return AN.v3(J[0][0] * y0 + J[0][1] * y1, J[1][0] * y0 + J[1][1] * y1,
               J[2][0] * y0 + J[2][1] * y1);
}
// Two perpendiculars of a unit direction.
function perps(d) {
  const a = Math.abs(d.y) < 0.9 ? AN.v3(0, 1, 0) : AN.v3(1, 0, 0);
  const p1 = AN.vnorm(AN.vcross(d, a));
  return [p1, AN.vnorm(AN.vcross(d, p1))];
}

const keyedDragCb = {
  down: () => { ATK.manualBegin?.(); },
  up: (id) => { ATK.manualEnd?.('pose: ' + id); },
  drag: (id, dx, dy) => {
    const ctx = keyedCtx();
    if (!ctx) return;
    ATK.manualEdit?.((raw, parsed) => {
      if (!parsed || !parsed.pose) return;
      const aim = heldAimOf(parsed);
      const fk = keyedFK(ctx, parsed.pose, aim);
      const rotAboutPt = (q, pivot, pt) => AN.vadd(pivot, AN.qrot(q, AN.vsub(pt, pivot)));
      if (id === 'ktip' || id === 'karm') {
        // Two rotations about axes square to the pivot-to-point line.
        const pivot = id === 'ktip' ? fk.H : fk.S;
        const pt = id === 'ktip' ? fk.Tw : AN.vmul(AN.vadd(fk.S, fk.E), 0.5);
        const dir = AN.vsub(pt, pivot);
        if (AN.vlen(dir) < 1e-4) return;
        const [a1, a2] = perps(AN.vnorm(dir));
        const f = (a, b) => AN.qmul(AN.qaxisangle(a1, a), AN.qaxisangle(a2, b));
        const J1 = jac(a => { const v = rotAboutPt(f(a, 0), pivot, pt); return [v.x, v.y, v.z]; }, 0, 1e-3);
        const J2 = jac(b => { const v = rotAboutPt(f(0, b), pivot, pt); return [v.x, v.y, v.z]; }, 0, 1e-3);
        const [da, db] = solve2(J1, J2, dx, dy).map(capStep);
        const Q = f(da, db);
        if (id === 'ktip') fk.W = AN.qnorm(AN.qmul(Q, fk.W));
        else {
          fk.U = AN.qnorm(AN.qmul(Q, fk.U));
          fk.F = AN.qnorm(AN.qmul(Q, fk.F));
          fk.W = AN.qnorm(AN.qmul(Q, fk.W));
        }
      } else if (id === 'kroll') {
        const ax = AN.vsub(fk.Tw, fk.H);
        if (AN.vlen(ax) < 1e-4) return;
        const axis = AN.vnorm(ax);
        const mid = AN.vmul(AN.vadd(fk.H, fk.Tw), 0.5);
        const pt = AN.vadd(mid, AN.vmul(AN.vnorm(AN.qrot(fk.W, ctx.wristFlat)), 1.5 * rigScale()));
        const J = jac(a => { const v = rotAboutPt(AN.qaxisangle(axis, a), mid, pt); return [v.x, v.y, v.z]; }, 0, 1e-3);
        const d = capStep(solve1(J, dx, dy));
        fk.W = AN.qnorm(AN.qmul(AN.qaxisangle(axis, d), fk.W));
      } else if (id === 'kelbow') {
        // Swivel the upper arm and forearm together about S->H; the hand's
        // orientation is held, so only the shoulder and wrist locals change.
        const ax = AN.vsub(fk.H, fk.S);
        if (AN.vlen(ax) < 1e-4) return;
        const axis = AN.vnorm(ax);
        const J = jac(a => { const v = rotAboutPt(AN.qaxisangle(axis, a), fk.S, fk.E); return [v.x, v.y, v.z]; }, 0, 1e-3);
        const d = capStep(solve1(J, dx, dy));
        const Q = AN.qaxisangle(axis, d);
        fk.U = AN.qnorm(AN.qmul(Q, fk.U));
        fk.F = AN.qnorm(AN.qmul(Q, fk.F));
      } else if (id === 'khand') {
        // Two-bone solve to the moved hand, the elbow kept on its side of the
        // shoulder-to-hand line; the blade keeps its model orientation.
        const want = AN.vadd(fk.H, screenMove3(fk.H, dx, dy));
        const L1 = AN.vlen(ctx.eOff), L2 = AN.vlen(ctx.hOff);
        if (L1 < 1e-4 || L2 < 1e-4) return;
        const to = AN.vsub(want, fk.S);
        const d0 = AN.vlen(to);
        if (d0 < 1e-4) return;
        const d = Math.max(Math.abs(L1 - L2) + 1e-3, Math.min(L1 + L2 - 1e-3, d0));
        const dir = AN.vmul(to, 1 / d0);
        const oldAxis = AN.vsub(fk.H, fk.S);
        let pole = AN.vsub(fk.E, AN.vadd(fk.S, AN.vmul(AN.vnorm(oldAxis),
          AN.vdot(AN.vsub(fk.E, fk.S), AN.vnorm(oldAxis)))));
        pole = AN.vsub(pole, AN.vmul(dir, AN.vdot(pole, dir)));
        if (AN.vlen(pole) < 1e-4) pole = perps(dir)[0];
        pole = AN.vnorm(pole);
        const x = (L1 * L1 - L2 * L2 + d * d) / (2 * d);
        const h = Math.sqrt(Math.max(0, L1 * L1 - x * x));
        const E2 = AN.vadd(fk.S, AN.vadd(AN.vmul(dir, x), AN.vmul(pole, h)));
        const H2 = AN.vadd(fk.S, AN.vmul(dir, d));
        // The smallest turns that carry each bone onto its new line.
        const qU = AN.qfromto(AN.vsub(fk.E, fk.S), AN.vsub(E2, fk.S));
        const U2 = AN.qnorm(AN.qmul(qU, fk.U));
        const F1 = AN.qnorm(AN.qmul(U2, AN.qmul(AN.qconj(fk.U), fk.F)));
        const lowerNow = AN.qrot(F1, ctx.hOff);
        const qF = AN.qfromto(lowerNow, AN.vsub(H2, E2));
        fk.U = U2;
        fk.F = AN.qnorm(AN.qmul(qF, F1));
        // fk.W unchanged: the blade keeps its direction.
      } else return;
      raw.pose = MELEE.poseToJson(keyedStore(ctx, fk, aim, parsed.pose));
    });
  },
};

/* --------------------------------------------------------------------------
   THE ANGLE GUIDE — what an az / el / reach box MEANS, drawn on the rig.

   While the pointer or focus is on a program row's az / el / reach box
   (attacks.js guideKey), this draws from the weapon arm's pivot:
     * the REFERENCE the row is measured from — the target line for the
       windup, where the previous leg ended for a cut leg, straight ahead for
       the recover (which is absolute);
     * the row's GOAL line, out to its resolved reach;
     * the AZIMUTH as a horizontal arc and the ELEVATION as a vertical arc
       between the two, each labelled with its signed angle.
   Same arithmetic as the runner (melee.js strokeGoalPose), so the picture is
   the pose the program steers to, not a second opinion about it.
   -------------------------------------------------------------------------- */
let guideSig = '';
function drawAngleGuide() {
  const key = laneTab === 'attacks' ? ATK.guideKey?.() : null;
  const lib = key ? ATK.library() : null;
  const sty = lib ? lib.styles[ATK.styleIndex()] : null;
  const seed = sty && melee ? weaponArmSeed() : null;
  if (!key || !sty || !seed) {
    if (guideSig) { guideSig = ''; ed.setAngleGuide?.(null, null); }
    return;
  }
  const aim = aimFrom(AN.vadd(seed.shoulder, lungeOffsetModel()), seed.S || rigScale());
  const add = (a, b) => ({ az: a.az + b.az, el: a.el + b.el });
  // A FRAME'S GUIDE: from what it is measured from (the target, or the
  // body's own "ahead"), to where its tip ends.
  const fk = typeof key === 'string' && key[0] === 'f' ? +key.slice(1) : -1;
  const fr = sty.frames && fk >= 0 && fk < sty.frames.length ? sty.frames[fk] : null;
  if (!fr) return;
  const ref = fr.fromBody ? { az: 0, el: 0 } : { az: aim.az, el: aim.el };
  const goal = add(ref, fr);
  const refName = fr.fromBody ? 'ahead' : 'target';
  const frameName = fr.name || `frame ${fk + 1}`;
  const g = MELEE.strokeGoalPose(sty, melee, key, aim.az, aim.el);
  const S = seed.S || 1;
  const reachVox = g ? g.reach : 5;
  const R = Math.max(2, reachVox) * S;
  const sig = [key, aim.az, aim.el, ref.az, ref.el, goal.az, goal.el, reachVox,
               seed.shoulder.x, seed.shoulder.y, seed.shoulder.z]
    .map(v => (typeof v === 'number' ? v.toFixed(3) : v)).join('|');
  if (sig === guideSig) return;
  guideSig = sig;

  // The rig's facing basis at heading 0 (weaponTick states it): right = -X.
  const dir = (az, el) => AN.v3(-Math.cos(el) * Math.sin(az), Math.sin(el),
                                Math.cos(el) * Math.cos(az));
  const P = AN.vadd(seed.shoulder, lungeOffsetModel());
  const at = (az, el, r) => AN.vadd(P, AN.vmul(dir(az, el), r));
  const arr = v => [v.x, v.y, v.z];
  const segs = [], labels = [];
  const line = (a, b, color, alpha = 1) => segs.push({ a: arr(a), b: arr(b), color, alpha });
  const arc = (fn, n, color) => {
    let prev = fn(0);
    for (let i = 1; i <= n; i++) { const p = fn(i / n); line(prev, p, color); prev = p; }
  };
  const C_REF = 0xdfe6ee, C_GOAL = 0xffc857, C_AZ = 0x4fd1ff, C_EL = 0xff6bd6;
  const hex = c => '#' + c.toString(16).padStart(6, '0');
  const deg = v => (v >= 0 ? '+' : '−') + Math.abs(Math.round(v * 180 / Math.PI)) + '°';
  // reference and goal lines
  line(P, at(ref.az, ref.el, R * 1.25), C_REF, 0.8);
  labels.push({ pos: arr(at(ref.az, ref.el, R * 1.32)), text: refName, color: hex(C_REF) });
  line(P, at(goal.az, goal.el, R), C_GOAL);
  labels.push({ pos: arr(at(goal.az, goal.el, R * 1.08)),
                text: `${frameName} · ${reachVox.toFixed(1)} vox`,
                color: hex(C_GOAL) });
  // azimuth: horizontal arc at the reference's elevation
  const dAz = goal.az - ref.az, dEl = goal.el - ref.el;
  const rAz = R * 0.55, rEl = R * 0.8;
  const nAz = Math.max(4, Math.ceil(Math.abs(dAz) / 0.08));
  if (Math.abs(dAz) > 1e-3) {
    arc(t => at(ref.az + dAz * t, ref.el, rAz), nAz, C_AZ);
    line(P, at(ref.az, ref.el, rAz), C_AZ);
    line(P, at(goal.az, ref.el, rAz), C_AZ);
  }
  labels.push({ pos: arr(at(ref.az + dAz * 0.5, ref.el, rAz * 1.12)),
                text: `az ${deg(dAz)}` + (Math.abs(dAz) > 1e-3 ? (dAz > 0 ? ' right' : ' left') : ''),
                color: hex(C_AZ) });
  // elevation: vertical arc at the goal's azimuth
  const nEl = Math.max(4, Math.ceil(Math.abs(dEl) / 0.08));
  if (Math.abs(dEl) > 1e-3) {
    arc(t => at(goal.az, ref.el + dEl * t, rEl), nEl, C_EL);
    line(P, at(goal.az, ref.el, rEl), C_EL, 0.6);
  }
  labels.push({ pos: arr(at(goal.az, ref.el + dEl * 0.5, rEl * 1.1)),
                text: `el ${deg(dEl)}` + (Math.abs(dEl) > 1e-3 ? (dEl > 0 ? ' up' : ' down') : ''),
                color: hex(C_EL) });
  ed.setAngleGuide?.(segs, labels);
}

function renderAllPanels() {
  renderRigPanel();
  renderTimeline();
}

/** Called by editor.js once its DOM exists. */
export function attach(opts) {
  el = opts.el;
  toast = opts.toast || (() => {});
  // The HOST'S live tuning document, not a fetch: the page may have unsaved
  // melee edits and previewing the version on disk instead of the one on
  // screen is exactly the drift this whole lane exists to remove. Optional —
  // a host that supplies neither leaves the Attacks lane on melee.h's own
  // defaults and says so.
  tuningGet = opts.tuning || null;
  tuningTouchFn = opts.touchTuning || null;
  const p = ed.panels();
  sideEl = p.side;
  timelineEl = p.timeline;
  bindAttacks();
  installTestSeam();
  rebuildSkeleton();
  renderAllPanels();
  // The shelf itself is scanned by the onActivate hook, which fires on every
  // tab entry including the first — doing it here as well would double every
  // page load's listing for nothing.
}

/* ==========================================================================
   THE TEST SEAM (assets/attacks_test.html, scripts/check_attacks.sh)

   Everything a headless harness needs to ASK QUESTIONS about the preview, and
   nothing it needs to break it. It exists because the two claims that matter
   here cannot be reached any other way:

     "when the preview walks, do the arms swing"  is a fact about posed
       transforms over time, which no unit test of anim.js can see — every
       function in that file was individually correct while the preview showed
       rest arms, because nothing ever STARTED a walk clip.
     "does the Attacks lane drive a real swing"   is a fact about a button
       reaching a driver through three modules and a rAF loop.

   Read-only apart from setPreviewSpeed / setStyleTicks, both of which the
   harness restores. Attached to `window` rather than exported because the
   harness drives tuner.html in an iframe and cannot import a module out of it.
   ========================================================================== */
function installTestSeam() {
  if (typeof window === 'undefined') return;
  window.__tunerRigForTest = {
    // Which clip instances are live, formatted as the gait readout formats
    // them ("walk×1.24@0.87"). The bare name is the prefix before × or @.
    playingClips: () => (anim?.clips || []).map(i => {
      const c = skel?.clips?.[i.clip];
      if (!c) return '?';
      const r = Math.abs((i.rate ?? 1) - 1) > 0.01 ? `×${i.rate.toFixed(2)}` : '';
      return `${c.name}${r}${i.stopping ? '↓' : ''}@${(i.weight * (i.fade ?? 1)).toFixed(2)}`;
    }),
    // The POSED model-space rotation of a limb — what the viewport draws, not
    // an internal that might not reach it.
    limbRot: name => {
      if (!skel || !anim?.model?.length) return null;
      const i = skel.findPart(name);
      return i >= 0 && anim.model[i] ? { ...anim.model[i].rot } : null;
    },
    limbPos: name => {
      if (!skel || !anim?.model?.length) return null;
      const i = skel.findPart(name);
      return i >= 0 && anim.model[i] ? { ...anim.model[i].pos } : null;
    },
    setPreviewSpeed: v => { gaitSpeedScale = +v || 0; renderGaitReadout(); },
    strideRate: () => anim?.strideRate ?? 0,
    gaitDebug: () => {
      if (!skel || !anim) return {};
      const legs = skel.chains.map((c, i) => [c, i]).filter(([c]) => c.tag === 'leg');
      return {
        touchdowns: {...dbgTouchdowns},
        swinging: anim.feet.map(f => f.swinging ? 1 : (f.valid ? 0 : -1)),
        chainTags: skel.chains.map(c => c.tag),
        stepPeriod: +(anim.stepPeriod ?? 0).toFixed(4),
        strideRate: +(anim.strideRate ?? 0).toFixed(3),
        legLength: +(anim.feet[legs[0]?.[1]]?.legLength ?? 0).toFixed(2),
        stepDuration: skel.gait.stepDuration,
        stepThresholdVox: +((skel.gait.stepThreshold *
          (anim.feet[legs[0]?.[1]]?.legLength ?? 0)).toFixed(2)),
        speedNow: +(previewCtx?.speedNow ?? 0).toFixed(2),
        defSpeed: +(previewCtx?.defSpeed ?? 0).toFixed(2),
        // NO `drift` FIELD. The obvious one — |planted - origin| — is the hip
        // offset plus the stride bias and NOT what updateGait thresholds on,
        // and reporting it read ~30 against a threshold of 16 and pointed at a
        // stepping bug that did not exist. The number that actually explains
        // the clock is the TOUCHDOWN COUNT above: two of them means the EMA is
        // still on its garbage first sample, not that the feet are stuck.
      };
    },
    counters: () => ({ preview: dbgPreviewSteps, weaponTicks: dbgWeaponTicks,
                       noStyle: dbgNoStyle, chain: wpChain,
                       hasMelee: !!melee, strokeStyle,
                       libStyles: ATK.library()?.styles.length ?? -1,
                       accum: +strokeAccum.toFixed(4) }),
    autoLoco: () => autoLoco,
    styleNames: () => (ATK.library()?.styles || []).map(s => s.name),
    // THE BAKE (attacks.js bakeAll): every target-spelled style to poses, and
    // the document it wrote, for a harness to save.
    bakeAll: () => ATK.bakeAll(),
    // A NEW STYLE from an old one mirrored and/or reversed (attacks.js
    // deriveStyle): the generated diagonals.
    deriveStyle: (src, name, opts) => ATK.deriveStyle(src, name, opts),
    // A POSE-HANDLE DRAG on the held keyed frame, as the pointer would make it
    // (keyedDragCb): down, one move of (dx, dy) pixels, up.
    dragKeyedHandle: (id, dx, dy) => {
      keyedDragCb.down(id);
      keyedDragCb.drag(id, dx, dy);
      keyedDragCb.up(id);
    },
    heldFramePose: () => {
      const fr = keyedPoseOfHeld();
      return fr ? JSON.parse(JSON.stringify(fr.pose)) : null;
    },
    handPos: () => { const p = weaponArmParts(); const h = p[2] >= 0 ? p[2] : p[1];
                     return h >= 0 && anim.model[h] ? { ...anim.model[h].pos } : null; },
    tickPreview: (n = 4) => { for (let i = 0; i < n; i++) stepPreviewFixed(kPreviewDt); },
    stylesDoc: () => JSON.stringify(ATK.rawDoc(), null, 2),
    keyedNames: () => (ATK.library()?.styles || []).filter(MELEE.styleKeyed).map(s => s.name),
    // The SAME fields the panel's own armInfo reports, so the harness and the
    // readout cannot disagree about whether a blade is in the fist. They were
    // two different shapes for one tick and the sword assertion read undefined
    // as zero, which is the "bare count" failure in miniature.
    armInfo: () => {
      const ch = (skel && wpChain >= 0) ? skel.chains[wpChain] : null;
      const seed = weaponArmSeed();
      const band = melee ? melee.reachBand() : { lo: 0, hi: 0 };
      return {
        chain: wpChain,
        handPart: wpHandPart >= 0 ? skel?.parts[wpHandPart]?.name : '',
        armName: ch ? ch.parts.map(i => skel.parts[i]?.name).join(' → ') : '',
        reach: seed?.reach ?? 0,
        bladeLen: melee?.bladeLen_ ?? 0,
        bandLo: band.lo, bandHi: band.hi,
        blade: !!bladeSegmentModel(),
        socket: weaponSocket()?.name ?? '',
        item: heldItemId || '',
        // WHICH PART THE DRIVER IS MOVING (game/impact.h StrikeEffectorMode).
        // The harness asserts a natural-weapon style drives the part it names,
        // which is the one claim the arithmetic tests cannot make: melee.js
        // has no rig and cannot know what "jaws" resolves to.
        effPart: (() => { const e = resolveEffector();
          return e.part >= 0 ? (skel?.parts[e.part]?.name || '') : ''; })(),
        effMode: resolveEffector().mode,
        effWeapon: (() => { const e = resolveEffector();
          return e.natural >= 0 ? (naturalList()[e.natural]?.name || '') : ''; })(),
        // What the rig declares, so a failure above says whether the style
        // named something wrong or the rig is missing it.
        natural: naturalList().map(w => (w?.name || '?') + '@' + (w?.part || '?')),
        aimRan: !!lastAim,
      };
    },
    strokeState: () => {
      const s = ATK.library();
      return {
        live: strokeLive(),
        phase: MELEE.STROKE_PHASE_NAME[strokeCur?.phase ?? 0],
        meleePhase: melee ? melee.phaseName() : 'idle',
        az: melee?.strokeAz() ?? 0,
        el: melee?.strokeEl() ?? 0,
        radius: melee?.strokeRadius() ?? 0,
        weight: melee?.poseWeight() ?? 0,
        style: strokeStyle >= 0 ? s?.styles[strokeStyle]?.name : null,
      };
    },
    // ONE SWING, STEPPED BY HAND and recorded per preview step (60 Hz, the
    // rate the rig is posed at): what the viewport draws, with no rAF, no
    // wall clock and no virtual-time budget in the way. `item` equips a held
    // item first ('' = none, undefined = leave it). Each row: the stroke's
    // phase / frame / weight and the posed arm joints + the weapon's edge, in
    // model space. The question it answers is "which tick jumps, and which
    // joint", which no driver-only probe can, because the joint brakes, the
    // pose clamps and the body clip all live in this file.
    traceStroke: async (styleName, steps = 90, item) => {
      if (item !== undefined && item !== (heldItemId || '')) await loadHeldItem(item || null);
      const lib = ATK.library();
      const i = lib ? lib.styles.findIndex(s => s.name === styleName) : -1;
      if (i < 0) return { error: 'no style ' + styleName };
      stopStroke();
      for (let k = 0; k < 8; k++) stepPreviewFixed(kPreviewDt);   // settle at rest
      beginStroke(i);
      const hand = { hand: -1 };
      const eff = resolveEffector();
      const ci = eff.part >= 0 ? chainForEffector(eff.part, hand) : wpChain;
      const ch = ci >= 0 ? skel.chains[ci] : null;
      const P = k => (k >= 0 && anim.model[k] ? { ...anim.model[k].pos } : null);
      const Q = k => (k >= 0 && anim.model[k] ? { ...anim.model[k].rot } : null);
      const rows = [];
      for (let s = 0; s < steps; s++) {
        const ticksBefore = dbgWeaponTicks;
        stepPreviewFixed(kPreviewDt);
        const edge = effectorEdgeModel();
        rows.push({
          step: s, ticked: dbgWeaponTicks !== ticksBefore, strokeTick,
          live: strokeLive(),
          phase: MELEE.STROKE_PHASE_NAME[strokeCur?.phase ?? 0],
          frame: strokeCur?.frame ?? -1, releasing: !!strokeCur?.releasing,
          weight: melee ? melee.poseWeight() : 0,
          shoulder: ch ? P(ch.parts[0]) : null, elbow: ch ? P(ch.parts[1]) : null,
          hand: P(hand.hand >= 0 ? hand.hand : wpHandPart),
          qUpper: ch ? Q(ch.parts[0]) : null, qLower: ch ? Q(ch.parts[1]) : null,
          qHand: Q(hand.hand >= 0 ? hand.hand : wpHandPart),
          tip: edge ? { ...edge.tip } : null, base: edge ? { ...edge.base } : null,
          driverHand: melee ? { ...melee.hand_ } : null,
          lag: armSmooth.valid ? armSmooth.lag : null,
        });
      }
      stopStroke();
      return { style: styleName, item: heldItemId || '', rows };
    },
    styleTicks: name => {
      const s = ATK.library()?.styles.find(x => x.name === name);
      // `cut` is the WHOLE path's ticks, every leg (strokes.h "A CUT IS A
      // PATH") — the phase is one phase however many corners it has.
      return s ? { windup: s.windup.ticks, cut: MELEE.cutTravel(s).ticks,
                   recover: s.recover.ticks } : null;
    },
    // Drives the box the AUTHOR drives, so the undo entry the panel pushes is
    // the one under test. A seam that wrote the JSON directly would prove the
    // data is mutable and nothing about whether Ctrl+Z reaches it.
    setStyleTicksUI: (name, seg, ticks) => {
      const i = ATK.library()?.styles.findIndex(x => x.name === name);
      if (i === undefined || i < 0) return false;
      ATK.selectStyle(i);
      const box = [...(timelineEl?.querySelectorAll('.atkseg input.atknum') || [])];
      // Column order is ticks, az, el, reach; rows are windup, cut, recover.
      const row = { windup: 0, cut: 4, recover: 8 }[seg];
      const inp = box[row];
      if (!inp) return false;
      inp.value = String(ticks);
      inp.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    },
    // Stage 6 is only meaningful if the rig authors a limit for it to enforce.
    clampRan: () => !!skel?.parts.some(p => p.hasPoseLimit || p.poseBall.has),
    elbowBendDeg: () => {
      if (!skel || !anim?.model?.length || wpChain < 0) return null;
      const i1 = skel.chains[wpChain].parts[1];
      const p = skel.parts[i1];
      if (!p?.hasPoseLimit || p.parent < 0) return null;
      const delta = AN.qmul(AN.qconj(p.rest.rot),
        AN.qmul(AN.qconj(anim.model[p.parent].rot), anim.model[i1].rot));
      // About the axis the CLAMP used, not the authored one — while the driver
      // steers the bend plane those are different questions. See the note on
      // lastElbowOverride.
      const ov = (lastElbowOverride && lastElbowOverride.part === i1 &&
                  lastElbowOverride.blend > 0) ? lastElbowOverride : null;
      const axis = ov ? AN.vnorm(ov.axis) : AN.vnorm(p.poseAxis);
      const ang = AN.animHingeAngleAbout(delta, axis);
      return ang === null ? null : ang * 180 / Math.PI;
    },
  };
}

/** The hooks editor.js consumes. */
export const hooks = {
  selectedPart: () => selectedPart,
  selectedParts: () => selectedParts,
  modelTransform,
  viewPlan,
  appendOnionInstances,
  appendExtraInstances,
  onKey,
  tick,
  // bindGizmo re-reads the anchor: the move brush shifts a limb's anchor as
  // it drags, so without this the orange ball would sit at the pre-move
  // position until the part was reselected.
  onModelsChanged: () => { rebuildSkeleton(); bindGizmo(); renderAllPanels(); },
  onSidecarChanged: () => {
    selectedPart = null; selectedParts.clear(); selectedSocket = null; activeTag = null; frameIndex = 0;
    activeClip = null; selectedKey = null; poseEdit = null;
    clipCursorMs = 0; clipPlaying = false; playing = false;
    clearTimeout(_touchTimer); ed.discardSidecarUndo();
    rebuildSkeleton();
    bindGizmo(); renderAllPanels();
  },
  onSelectionChanged: () => { renderRigPanel(); },
  onSave: () => { if (itemDirty) return saveHeldItem(); },
  // Another session (or another tab) may have saved a limb since this page
  // loaded; rescanning on tab entry is what keeps the shelf from going stale
  // without making the user find the ↻ button.
  // The creature shelf is only rescanned if it has already been looked at
  // once: its first scan reads one sidecar per mob file, and paying that on
  // every tab entry for a shelf nobody opened is the sort of cost that gets
  // blamed on the tab being slow.
  onActivate: () => {
    refreshLibrary().then(renderAllPanels);
    if (crParts !== null) refreshCreatures().then(renderAllPanels);
  },
};
