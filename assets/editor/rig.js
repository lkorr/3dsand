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
import * as VOX from './vox.js';

/* ==========================================================================
   1. state + helpers
   ========================================================================== */

let el = null, toast = () => {};
let sideEl = null, timelineEl = null;
let clipWrap = null;          // renderClipLane()'s own container, see below

let selectedPart = null;      // limb name, or null
// Socket being dragged, by INDEX into sidecar.sockets, or null. Selecting a
// socket takes the gizmo over from the joint anchor — both are "a point on a
// part", so they share one gizmo rather than fighting over the viewport.
let selectedSocket = null;

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
let gaitSpeedScale = 1;

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
    itemList = arr.map(it => ({ id: it.id || it.name, name: it.name || it.id }))
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
    itemSc = JSON.parse(await rs.text());

    const modelName = itemSc.model || id;
    const rv = await fetch('/api/model?path=' + encodeURIComponent('items/' + modelName + '.vox'));
    if (!rv.ok) throw new Error(modelName + '.vox ' + rv.status);
    // `.prefab` is the EDITOR-shaped parse (dim/grid/offset per model), the
    // same one openPath() builds the document from — not the raw .vox models.
    const parsed = VOX.readVox(await rv.arrayBuffer());
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
 */

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
  const sock = activeSocket();
  if (!sock || !itemDoc || !itemSc) return null;
  const grip = itemSc.grip?.[gripCtx];
  if (!grip) return null;                 // item cannot be held this way

  const itemScale = +itemSc.scale > 0 ? +itemSc.scale : 1;
  const rigScale = +(sc().skinScale ?? sc().scale) || 1;
  // The viewport unit is one RIG file voxel. One item file voxel covers
  // (rigScale / itemScale) viewport units — so a sword at scale 4 on a rig at
  // skinScale 8 draws each item voxel as a 2-unit cube, matching the real size
  // ratio the engine applies.
  const voxRatio = rigScale / itemScale * (+grip.scale > 0 ? +grip.scale : 1);

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
  const hilt = itemSc.hilt;
  if (hilt && Array.isArray(hilt.min) && Array.isArray(hilt.size)) {
    const c = {
      x: (+hilt.min[0] + +hilt.size[0] / 2) * voxRatio,
      y: (+hilt.min[1] + +hilt.size[1] / 2) * voxRatio,
      z: (+hilt.min[2] + +hilt.size[2] / 2) * voxRatio,
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
  const hilt = itemSc?.hilt;
  if (!xf || !hilt || !Array.isArray(hilt.min) || !Array.isArray(hilt.size)) {
    ed.setHiltBox?.(null);
    return;
  }
  const s = xf.scale;
  const cx = (+hilt.min[0] + +hilt.size[0] / 2) * s;
  const cy = (+hilt.min[1] + +hilt.size[1] / 2) * s;
  const cz = (+hilt.min[2] + +hilt.size[2] / 2) * s;
  const r = qrot(xf.quat, { x: cx, y: cy, z: cz });
  ed.setHiltBox?.({
    pos: [r.x + xf.pos.x, r.y + xf.pos.y, r.z + xf.pos.z],
    size: [+hilt.size[0] * s, +hilt.size[1] * s, +hilt.size[2] * s],
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
  // Item .vox files carry no RGBA chunk (gen_sword_item.py writes indices, not
  // colours), so the material table is the normal path and the palette branch
  // below only fires for a hand-made file that does embed one.
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
function touched() {
  ed.touchSidecar();
  rebuildSkeleton();
}

/**
 * Upscale the whole document 2×: geometry (editor.js doubles every model and
 * offset), then everything in the sidecar that is measured in voxels —
 * anchors and clip pos keys — and finally `skinScale`, so the creature keeps
 * its world size and gains detail. This is THE way to give an existing mob
 * finer microvoxels: skinScale 4 → 8 halves the voxel size without moving a
 * joint or changing a clip's meaning.
 *
 * This function is also the authoritative INVENTORY of which sidecar fields
 * are measured in micro units: limbs[].anchor, clips[*].tracks[*].pos[].v,
 * editor.parts[].box, plus the model grids and offsets editor.js handles.
 * gait.rideHeight, gait.stepHeight and states[].bodyYOffset are WORLD units
 * and are deliberately untouched — doubling them would raise the creature off
 * the ground by exactly the amount it just gained in detail.
 */
function upscale2x() {
  const s = sc();
  const scl = +(s.skinScale ?? s.scale) || 1;
  const isItem = !isRigged() && s.scale != null;
  if ((isRigged() || isItem) && scl >= 8) {
    toast('already at scale 8 — the finest the engine accepts', true);
    return;
  }
  const scaleNote = isRigged() ? `, anchors/keys double, and skinScale goes ${scl} → ${scl * 2}`
    : isItem ? `, scale goes ${scl} → ${scl * 2} (same world size, finer detail)`
    : '';
  if (!confirm('Upscale 2×? Every voxel becomes a 2×2×2 block' + scaleNote +
      '. This clears the undo history.')) return;
  if (!ed.upscaleDoc()) return;          // toasts its own reason on failure
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
  if (isRigged()) {
    delete s.scale;             // one key wins (see the scale dropdown)
    s.skinScale = scl * 2;
  } else if (s.scale != null) {
    // Standalone item (sword, torch, …): `scale` is item micro units per world
    // voxel. Doubling geometry + scale keeps the same world size, same as
    // skinScale does for mobs. Also double hilt.min since it is in micro units.
    s.scale = scl * 2;
    if (s.hilt && Array.isArray(s.hilt.min))
      s.hilt.min = s.hilt.min.map(v => v * 2);
    if (s.hilt && Array.isArray(s.hilt.size))
      s.hilt.size = s.hilt.size.map(v => v * 2);
    if (s.edge) {
      if (s.edge.from != null) s.edge.from *= 2;
      if (s.edge.to != null) s.edge.to *= 2;
      if (s.edge.halfWidth != null) s.edge.halfWidth *= 2;
    }
    for (const ctx of Object.values(s.grip || {}))
      if (Array.isArray(ctx.translation))
        ctx.translation = ctx.translation.map(v => v * 2);
  }
  touched();
  bindGizmo();
  ed.refreshMicroGhost?.();
  ed.invalidate();
  renderAllPanels();
  toast('upscaled 2×' + (isRigged()
    ? ` — skinScale ${scl * 2}: same world size, ${scl * 2}× voxel density`
    : isItem
      ? ` — scale ${scl * 2}: same world size, ${scl * 2}× voxel density`
      : ' — the model is twice the resolution (and twice the world size)'));
}

function growSize2x() {
  const s = sc();
  const scl = +(s.skinScale ?? s.scale) || 1;
  if (scl <= 1) {
    toast('already at scale 1 — cannot grow further (would need scale < 1)', true);
    return;
  }
  if (isRigged()) {
    delete s.scale;
    s.skinScale = scl / 2;
    if (s.skinScale <= 1) delete s.skinScale;
  } else if (s.scale != null) {
    s.scale = scl / 2;
    if (s.scale <= 1) s.scale = 1;
  }
  touched(); ed.refreshMicroGhost?.(); ed.invalidate();
  renderAllPanels();
  toast(`scale ${scl} → ${scl / 2}: same voxels, twice the world size`);
}

function shrinkSize2x() {
  const s = sc();
  const scl = +(s.skinScale ?? s.scale) || 1;
  if (scl >= 8) {
    toast('already at scale 8 — the finest the engine accepts', true);
    return;
  }
  if (isRigged()) {
    delete s.scale;
    s.skinScale = scl * 2;
  } else if (s.scale != null) {
    s.scale = scl * 2;
  }
  touched(); ed.refreshMicroGhost?.(); ed.invalidate();
  renderAllPanels();
  toast(`scale ${scl} → ${scl * 2}: same voxels, half the world size`);
}

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
    if (!ed.moveModel(mi, d)) return;
    boxes.forEach(b => { b.value = '0'; });
    touched();
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
     further down. */
  const list = el('div', { class: 'riglist' });
  models.forEach((m, i) => {
    const limbHere = limbByName(m.name);
    const openHere = !!limbHere && limbHere.name === selectedPart;
    const row = el('div', {
      class: 'rigrow' + (i === ed.getActiveModel() ? ' on' : ''),
      onclick: () => {
        ed.setActiveModel(i);
        // Select the whole model so Ctrl+C / Del / fill work on it immediately.
        ed.setSelection({
          lo: [0, 0, 0],
          hi: [m.dim.x - 1, m.dim.y - 1, m.dim.z - 1],
          model: i,
        });
        // Toggle the inline limb editor. A model with no limb entry still
        // selects (for painting); "sync" in Limbs is what gives it an entry.
        selectedPart = (limbHere && !openHere) ? m.name : null;
        selectedSocket = null;   // picking a limb drops a socket drag
        bindGizmo();
        ed.invalidate();
        renderAllPanels();
      },
    });
    const nm = el('input', { class: 'cell id', value: m.name });
    nm.addEventListener('click', e => e.stopPropagation());
    nm.addEventListener('change', () => {
      const old = m.name;
      if (!ed.renameModel(i, nm.value)) { nm.value = old; return; }
      // Keep the limb entry and any parent references pointing at it.
      const l = limbByName(old);
      if (l) { l.name = m.name; touched(); }
      for (const o of limbs()) if (o.parent === old) { o.parent = m.name; touched(); }
      renderAllPanels();
    });
    row.append(
      dimInput(m, i),
      nm,
      el('button', {
        class: 'icon' + (ed.isModelSolo(m.name) ? ' on' : ''),
        title: 'solo — show only this limb (and any other soloed)',
        onclick: e => { e.stopPropagation(); ed.toggleModelSolo(m.name); renderAllPanels(); },
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
    list.append(row);
    // The limb editor lives directly under its own row — this is the merge of
    // the old Models and Limbs lists.
    if (openHere) list.append(limbBody(limbHere));
  });

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
        title: 'double the resolution: every voxel becomes 2×2×2, anchors and ' +
          'pos keys double, scale bumps — same world size, finer voxels',
        onclick: upscale2x,
      }, '2× detail'),
      el('button', {
        class: 'small',
        title: 'double world size: halve the scale so each voxel is bigger — no geometry change, fully reversible',
        onclick: growSize2x,
      }, '2× size'),
      el('button', {
        class: 'small',
        title: 'halve world size: double the scale so each voxel is smaller — no geometry change, fully reversible',
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
function renderRigTail() {
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
      }, gaitOn ? 'stop [K]' : 'walk [K]')));

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
    })()));

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
    st.textContent = `phase ${anim.gaitPhase.toFixed(2)}  bodyY ${anim.bodyY.toFixed(2)}  ` +
      `feet ${sw}   (▲ swinging ▼ planted · gone)`;
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

   Engine stage order (mob.cpp:756-865 UpdateAnimation):
     velocity smoothing -> gaitPhase advance -> AnimSampleAndBlend (1-3)
     -> procedural layer (legacy swing, bob/sway/roll/spine, springs)
     -> AnimFlatten (4) -> UpdateGait -> AnimSolveTwoBone per chain (5)
   Stage 6 (physics/ragdoll blend) is omitted: it needs Jolt.
   ========================================================================== */

let skel = null;              // built by rebuildSkeleton()
let anim = null;              // AnimState mirror
let previewCtx = null;
let previewOrigin = { x: 0, y: 0, z: 0 };
let restModel = null;         // model-space rest pose, for the delta transform

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
function stepPreview(dt) {
  if (!skel || !anim) return;
  const sidecar = ed.getSidecar() || {};
  previewCtx.defSpeed = num(sidecar.speed, 5);
  previewCtx.prefabSize = ed.getDoc()?.size || { x: 1, y: 1, z: 1 };
  previewCtx.rootLimb = skel.rootLimb;

  // Pin the runtime clip instance to the editor's cursor every step.
  // animSampleAndBlend is the ENGINE's own loop and advances timeMs by
  // dt*1000 itself, so pre-compensate: after its advance the sample lands
  // exactly on clipCursorMs — paused (the pose must not drift off the scrub
  // cursor), scrubbing, or playing (tick() owns the cursor) alike. This also
  // survives rebuildSkeleton(), which resets anim.clips on every rig edit.
  {
    const ci = activeClip ? skel.clips.findIndex(k => k.name === activeClip) : -1;
    anim.clips = ci >= 0
      ? [{ clip: ci, timeMs: clipCursorMs - dt * 1000, weight: 1,
           stopping: false, fade: 1 }]
      : [];
  }

  const speed = previewCtx.defSpeed * gaitSpeedScale;

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

  // mob.cpp:773 — gaitPhase advance, wrapped to [0,1)
  const g = skel.gait;
  const speedFactor = Math.min(Math.max(previewCtx.speedNow /
    Math.max(previewCtx.defSpeed, 0.01), 0), 1.5);
  anim.gaitPhase += dt * (g.present ? g.cadence : 2.2) * speedFactor;
  if (anim.gaitPhase > 1) anim.gaitPhase -= Math.floor(anim.gaitPhase);

  // stages 1-3
  AN.animSampleAndBlend(skel, anim, dt);
  // Live pose being authored: applied on top of the sampled clip so the
  // rotation rings show their effect before the key is committed. This is an
  // EDITOR-ONLY layer — nothing in the engine corresponds to it, which is why
  // it goes here rather than inside anim.js.
  applyPoseEdit();
  // procedural layer (legacy swing + bob/sway/roll/spine + springs)
  AN.applyProceduralLayer(skel, anim, previewCtx, dt);
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
  ed.invalidate();
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

  return {
    pivot: { x: anchor.x, y: anchor.y, z: anchor.z },
    quat: dq,
    pos: { x: dp.x, y: dp.y + dy, z: dp.z },
  };
}

// The preview runs whenever something wants a posed rig: the gait walk, clip
// playback, or simply having a clip open (so scrubbing and ring-dragging show
// their result on a paused rig).
const previewActive = () => gaitOn || clipPlaying || !!activeClip;

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

function renderTimeline() {
  if (!timelineEl) return;
  timelineEl.innerHTML = '';

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

const CLIP_PX_PER_MS = 0.45;      // lane zoom; 420ms clip ≈ 190px

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

function renderClipLane() {
  if (!clipWrap) return;
  clipWrap.innerHTML = '';
  const C = clips();
  const names = Object.keys(C);

  const bar = el('div', { class: 'tagbar' }, el('span', { class: 'hint' }, 'clips'));
  for (const n of names) {
    bar.append(el('button', {
      class: 'small' + (activeClip === n ? ' on' : ''),
      onclick: () => {
        activeClip = activeClip === n ? null : n;
        clipCursorMs = 0; selectedKey = null;
        clipPlaying = false;
        reseedPose();
        renderAllPanels();
      },
    }, n));
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
    }, clipPlaying ? '■ stop [P]' : '▶ play [P]') : null);
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
    if (!times.length && limb.name !== selectedPart) continue;   // keep it tight
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
    'auto-key writes on release'));
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

  if (previewActive()) {
    stepPreview(dt);
    renderGaitReadout();
  }
}

function renderAllPanels() {
  renderRigPanel();
  renderTimeline();
}

/** Called by editor.js once its DOM exists. */
export function attach(opts) {
  el = opts.el;
  toast = opts.toast || (() => {});
  const p = ed.panels();
  sideEl = p.side;
  timelineEl = p.timeline;
  rebuildSkeleton();
  renderAllPanels();
}

/** The hooks editor.js consumes. */
export const hooks = {
  selectedPart: () => selectedPart,
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
    selectedPart = null; selectedSocket = null; activeTag = null; frameIndex = 0;
    activeClip = null; selectedKey = null; poseEdit = null;
    clipCursorMs = 0; clipPlaying = false; playing = false;
    rebuildSkeleton();
    bindGizmo(); renderAllPanels();
  },
  onSelectionChanged: () => { renderRigPanel(); },
  onSave: () => { if (itemDirty) return saveHeldItem(); },
};
