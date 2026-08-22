/* limblib.js — the limb LIBRARY: save a body part once, wear it on anything.

   WHY THIS EXISTS. A creature in this engine is already a bag of independent
   parts: one .vox MODEL per limb (named by its scene-graph nTRN node), plus one
   sidecar `limbs[]` entry per model that says what the part IS — hp, joint,
   tag, severable, and the `anchor` where it pivots on its parent. mob.cpp:308
   joins the two BY NAME and by nothing else. Nothing anywhere binds a limb to
   the creature it was drawn on.

   So a limb is already a self-contained thing, and this file is mostly the
   admission of that. A library part is the same .vox+.json pair a mob uses,
   one rung down the hierarchy:

     assets/limbs/arm_scythe.vox    geometry — one .vox model per limb in the
                                    part, plus the "<name>.col" art layer, in
                                    exactly the format a mob file uses
     assets/limbs/arm_scythe.json   the rig metadata (see PART FORMAT below)

   which means MagicaVoxel opens a limb file, the round-trip test covers it,
   and `readVox`/`writeVox` need no new code paths. It also means the format
   degrades honestly: a limb .vox with no .json is still a usable lump of
   voxels, it just arrives unrigged.

   THE TWO COORDINATE TRAPS. Both cost real debugging time elsewhere in this
   codebase and both are structural here, not conventions:

   1. `anchor` in a mob sidecar is in PREFAB space — measured from the whole
      creature's min corner. That number is meaningless in another creature,
      whose torso is a different size. A saved part therefore stores
      `anchorLocal`, measured from the PART's own min corner, and converts on
      the way in and out (see toLocalAnchor / toPrefabAnchor). Storing the
      prefab anchor is the bug that puts a swapped arm's shoulder joint out in
      the air a torso-width away.

   2. A multi-limb part (a whole arm: armU > armL > hand) has to keep its
      limbs' RELATIVE placement. Each model's `offset` is stored relative to
      the part's min corner, so the part travels as a rigid group and lands as
      one. `partFromPrefab` rebases; `placePart` re-offsets.

   PART FORMAT (the .json). Version 1:

     { "v": 1,
       "root": "armU.L",            // which limb is the attachment point
       "limbs": [                   // one per .vox model, root first
         { "name": "armU.L", "hp": 16, "joint": "ball", "tag": "arm",
           "severable": true, "severImpactSpeed": 15,
           "anchorLocal": [4, 30, 8],       // rel. to the PART's min corner
           "offset": [0, 0, 0] },           // model box, rel. to the same
         { "name": "armL.L", "parent": "armU.L", ... } ],
       "meta": { "from": "asha", "dim": [10, 34, 10], "saved": 1755840000 } }

   Every key inside a limb entry other than `anchorLocal`/`offset` is copied
   VERBATIM from (and back into) the mob sidecar, so a field this file has
   never heard of — a `spring` block, some future flag — survives the round
   trip. That is the same mutate-in-place rule rig.js follows for the sidecar
   as a whole, for the same reason.
*/

import * as VOX from './vox.js';

export const LIMB_DIR = 'limbs';
export const PART_VERSION = 1;

/* Sidecar limb keys this module manages itself and must NOT blind-copy.
   `anchor` becomes anchorLocal; `parent` is rewritten on placement because the
   part's root re-parents onto whatever it is dropped on. */
const MANAGED = new Set(['anchor', 'parent', 'name']);

/* A library part's file stem: what the user types, what appears in the list.
   Kept to the same alphabet the server's other name-shaped routes accept, so
   a name that works here cannot fail to write. */
export const nameOk = s => /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(s || '');

export const partVoxPath = name => `${LIMB_DIR}/${name}.vox`;
export const partJsonPath = name => `${LIMB_DIR}/${name}.json`;

/* ---------------------------------------------------------------------------
   Reading the creature: which limbs make up a part
   ------------------------------------------------------------------------ */

/** Every limb whose parent chain reaches `root`, root first, parents before
 *  children. Order matters: `placePart` relies on a parent existing before the
 *  child that names it, and the .vox model order must match `limbs[]`. */
export function subtreeNames(limbs, root) {
  const kids = new Map();
  for (const l of limbs) {
    if (!l.parent) continue;
    if (!kids.has(l.parent)) kids.set(l.parent, []);
    kids.get(l.parent).push(l.name);
  }
  const out = [], seen = new Set();
  const walk = n => {
    if (seen.has(n)) return;        // a cycle in hand-edited JSON must not hang
    seen.add(n);
    out.push(n);
    for (const c of kids.get(n) || []) walk(c);
  };
  walk(root);
  return out;
}

/** The limbs of `names` in that order, dropping any that name no model. */
const modelsFor = (models, names) =>
  names.map(n => models.find(m => m.name === n)).filter(Boolean);

/* ---------------------------------------------------------------------------
   Save: creature -> part
   ------------------------------------------------------------------------ */

/**
 * Build a saveable part from the live document.
 *
 * @param {{models:Array}} doc      the editor document (prefab)
 * @param {Array} limbs             sidecar limbs[]
 * @param {string} root             the limb to save (with its descendants)
 * @param {{group:boolean, from:string}} opts
 * @returns {{prefab, json, names:string[]}} prefab is ready for writeVox,
 *          json is the .json body.
 */
export function partFromPrefab(doc, limbs, root, opts = {}) {
  const names = opts.group === false ? [root] : subtreeNames(limbs, root);
  const models = modelsFor(doc.models, names);
  if (!models.length)
    throw new Error(`"${root}" has no .vox model of that name`);

  // The part's own frame: min corner of the models it actually contains.
  const base = { x: Infinity, y: Infinity, z: Infinity };
  for (const m of models) {
    base.x = Math.min(base.x, m.offset.x);
    base.y = Math.min(base.y, m.offset.y);
    base.z = Math.min(base.z, m.offset.z);
  }

  const size = { x: 0, y: 0, z: 0 };
  const outModels = models.map(m => {
    const offset = { x: m.offset.x - base.x, y: m.offset.y - base.y,
                     z: m.offset.z - base.z };
    size.x = Math.max(size.x, offset.x + m.dim.x);
    size.y = Math.max(size.y, offset.y + m.dim.y);
    size.z = Math.max(size.z, offset.z + m.dim.z);
    // The grid is shared with the live document, not copied: writeVox only
    // reads it, and partFromPrefab is called on the save path where the
    // document is not being mutated underneath us.
    return { name: m.name, offset, dim: m.dim, grid: m.grid };
  });

  const byName = new Map(limbs.map(l => [l.name, l]));
  const jsonLimbs = models.map(m => {
    const src = byName.get(m.name) || {};
    const e = { name: m.name };
    if (src.parent && names.includes(src.parent)) e.parent = src.parent;
    for (const [k, v] of Object.entries(src))
      if (!MANAGED.has(k)) e[k] = v;         // verbatim, unknown keys included
    if (Array.isArray(src.anchor))
      e.anchorLocal = [src.anchor[0] - base.x, src.anchor[1] - base.y,
                       src.anchor[2] - base.z];
    const o = outModels.find(x => x.name === m.name).offset;
    e.offset = [o.x, o.y, o.z];
    return e;
  });

  return {
    prefab: { size, models: outModels },
    names: models.map(m => m.name),
    json: {
      v: PART_VERSION,
      root,
      limbs: jsonLimbs,
      meta: { from: opts.from || '', dim: [size.x, size.y, size.z] },
    },
  };
}

/* ---------------------------------------------------------------------------
   Load: part -> creature
   ------------------------------------------------------------------------ */

/** Parse a fetched part. `vox` is the ArrayBuffer, `json` the parsed sidecar
 *  (or null — a bare .vox is legal and arrives unrigged). */
export function readPart(vox, json) {
  const parsed = VOX.readVox(vox);
  const prefab = parsed.prefab;
  const meta = json && Array.isArray(json.limbs) ? json : null;
  // A part written by this module carries per-limb `offset`; a hand-made or
  // MagicaVoxel-authored one does not, and the .vox scene graph is then the
  // only truth. Trusting the .vox in both cases keeps one code path.
  const limbs = (meta ? meta.limbs : prefab.models.map(m => ({ name: m.name })))
      .filter(e => prefab.models.some(m => m.name === e.name));
  return {
    prefab,
    palette: parsed.palette,
    root: (meta && meta.root) || (limbs[0] && limbs[0].name) || '',
    limbs,
    meta: (meta && meta.meta) || {},
    warnings: parsed.warnings || [],
  };
}

/**
 * Where a part's min corner must land so its root anchor sits on `target`.
 *
 * `target` is a prefab-space joint position (the anchor of the limb being
 * replaced, or of the parent it is being attached to). When the part has no
 * root anchor there is nothing to align, so it lands with its min corner at
 * the target and the user nudges — better than silently landing at the origin.
 */
export function placementFor(part, target) {
  const root = part.limbs.find(l => l.name === part.root) || part.limbs[0];
  const a = root && root.anchorLocal;
  if (!Array.isArray(a) || !target) return { x: target ? target[0] : 0,
                                             y: target ? target[1] : 0,
                                             z: target ? target[2] : 0 };
  return { x: target[0] - a[0], y: target[1] - a[1], z: target[2] - a[2] };
}

/** The part's limb entries as MOB sidecar entries: anchorLocal -> prefab
 *  `anchor` at `at`, and the root re-parented onto `parent`. */
export function sidecarEntries(part, at, parent) {
  return part.limbs.map(l => {
    const e = {};
    for (const [k, v] of Object.entries(l))
      if (k !== 'anchorLocal' && k !== 'offset') e[k] = v;
    if (l.name === part.root) {
      if (parent) e.parent = parent; else delete e.parent;
    }
    if (Array.isArray(l.anchorLocal))
      e.anchor = [l.anchorLocal[0] + at.x, l.anchorLocal[1] + at.y,
                  l.anchorLocal[2] + at.z];
    return e;
  });
}

/* ---------------------------------------------------------------------------
   Name collisions

   Two creatures both have "hand.R". Dropping one onto the other must not
   silently merge them into one model, so an incoming name that is already
   taken (by a model the part is not itself replacing) gets a numeric suffix,
   and every reference to it inside the part is rewritten to match.
   ------------------------------------------------------------------------ */

export function uniqueNames(part, taken) {
  const map = new Map();
  for (const l of part.limbs) {
    let n = l.name;
    if (taken.has(n)) {
      const stem = n.replace(/\.(\d+)$/, '');
      let i = 2;
      while (taken.has(`${stem}.${i}`)) i++;
      n = `${stem}.${i}`;
    }
    taken.add(n);
    map.set(l.name, n);
  }
  return map;
}

/** Apply a rename map to a part in place (both the limb list and the models). */
export function renameWithin(part, map) {
  const to = n => map.get(n) || n;
  for (const l of part.limbs) {
    if (l.parent) l.parent = to(l.parent);
    l.name = to(l.name);
  }
  for (const m of part.prefab.models) {
    if (VOX.isArtLayerName(m.name)) {          // "<base>.col" tracks its base
      m.name = VOX.artLayerName(to(VOX.artLayerBase(m.name)));
      continue;
    }
    m.name = to(m.name);
  }
  part.root = to(part.root);
}
