/* anatomy.js — what is UNDER the skin of a mob, as a function of depth.
 *
 * A mob limb is a dense voxel grid and the engine keeps every enclosed cell
 * (voxload keeps them, the micro brick is dense, a carve or a burn exposes
 * them), so "muscle under the skin and bone under that" is not an engine
 * feature — it is an AUTHORING operation over the limb .vox files: give each
 * interior voxel a material by how deep it sits. This module is that
 * operation, pure and Node-runnable, and it is the ONLY place the rule lives:
 * the tuner's Models tab calls it for the "Apply recipe" button and
 * scripts/anatomize_mob.mjs calls it to bake a committed asset.
 *
 * DEPTH is measured over the UNION of every model in the prefab, EXCEPT that
 * a limb's own faces are always depth 0. Limbs abut at their joints (the top
 * of a thigh sits against the hips, the head sits on the torso) and the union
 * alone calls that joint face interior — which put vertebrae on the top of
 * the neck and a shoulder socket on the arm, both on show the moment the rig
 * turned a head or raised an arm. So: depth 0 is any solid voxel with an
 * empty 6-neighbour in ITS OWN MODEL (outside the model box counts as empty),
 * which makes every limb skin all the way round; everything else takes the
 * union's depth, n 6-connected steps in from a union-exposed face, so the
 * bone core still runs through the joint and a cut through the middle of a
 * limb shows the schedule. A joint that is severed shows its skin cap and the
 * blood the wound soaks into it (Mob::StainWound), not an anatomy plate.
 *
 * A RECIPE is data in the mob sidecar (`"anatomy": {...}`), materials by NAME
 * (CLAUDE.md design guideline 4: author by name, resolve at load), so a cyborg
 * is a different recipe over the same tool, never a different tool:
 *
 *   {
 *     "layers": [                       // outermost first
 *       { "material": "skin",   "depth": 1, "keep": true },
 *       { "material": "flesh",  "depth": 1 },
 *       { "material": "muscle", "depth": 1,
 *         "speckle": { "material": "blood", "fraction": 0.06 } },
 *       { "material": "bone" }          // no depth: everything deeper
 *     ],
 *     "garments": ["linen"],            // surface voxels that are CLOTHES
 *     "limbs": { "head": { "layers": [ ... ] } }   // per-limb override
 *   }
 *
 *  - `keep` leaves the voxels of that layer exactly as they are — material AND
 *    art colour. The surface layer is the painted character (eyes, mouth, the
 *    shading rows) and a recipe must never repaint it. The one exception: a
 *    kept voxel whose material is one the recipe puts UNDER the surface
 *    (flesh, bone, the speckle) is not paint, it is a face this recipe once
 *    baked as interior (the joint faces before own-face depth existed), and
 *    it is rewritten to the layer's own material with its art cleared.
 *  - Every rewritten voxel has its art colour CLEARED. Art overrides the
 *    material colour in microbody.wgsl, so a painted interior would show the
 *    skin's paint on the flesh it exposes.
 *  - `speckle` swaps a hash-selected fraction of a layer's cells for another
 *    material (blood vessels through the muscle). The hash is on prefab
 *    coordinates, so applying the recipe twice is a no-op.
 *  - A GARMENT is a surface voxel that is clothing, not body. The voxel
 *    directly under it takes the first layer's material (skin under the
 *    shorts, not flesh under the shorts); every deeper voxel follows the
 *    schedule by depth unchanged, so the bone core is where it would be on a
 *    bare limb.
 */

export const ANATOMY_VERSION = 1;

/** The stock human recipe: what `human.json` carries. Exported so the editor
 *  has something to apply to a mob whose sidecar has no `anatomy` block, and
 *  so the test can pin its shape. */
export const DEFAULT_ANATOMY = {
  layers: [
    { material: 'skin', depth: 1, keep: true },
    { material: 'flesh', depth: 1 },
    { material: 'muscle', depth: 1, speckle: { material: 'blood', fraction: 0.06 } },
    { material: 'bone' },
  ],
  garments: ['linen'],
  limbs: {
    // The skull is a shell, and what it protects is not bone.
    head: {
      layers: [
        { material: 'skin', depth: 1, keep: true },
        { material: 'flesh', depth: 1 },
        { material: 'bone', depth: 2 },
        { material: 'flesh' },
      ],
    },
  },
};

export const DEPTH_EMPTY = 255;   // sentinel in the depth field: no voxel here

/**
 * Depth-from-surface over the union of every model in `prefab`, in prefab
 * space. Returns { dim, depth: Uint8Array } where depth[i] is DEPTH_EMPTY for
 * empty cells and the 6-connected step count from the nearest exposed face
 * otherwise (0 = has an empty neighbour). Multi-source BFS; O(cells).
 *
 * `visible(mi)` optionally excludes models from the union — the editor does
 * NOT pass it (a hidden limb still shades its neighbours' joint faces), but a
 * caller composing a partial creature may want to.
 */
export function unionDepth(prefab, visible = null) {
  const dim = { x: prefab.size.x, y: prefab.size.y, z: prefab.size.z };
  const n = dim.x * dim.y * dim.z;
  const depth = new Uint8Array(n).fill(DEPTH_EMPTY);
  const sx = 1, sy = dim.x, sz = dim.x * dim.y;

  // Occupancy, from every model at its prefab offset.
  const solid = new Uint8Array(n);
  prefab.models.forEach((m, mi) => {
    if (visible && !visible(mi)) return;
    const d = m.dim, data = m.grid.data, o = m.offset;
    for (let z = 0; z < d.z; z++)
      for (let y = 0; y < d.y; y++) {
        const row = y * d.x + z * d.x * d.y;
        const prow = (o.x) + (y + o.y) * sy + (z + o.z) * sz;
        for (let x = 0; x < d.x; x++)
          if (data[row + x]) solid[prow + x] = 1;
      }
  });

  // Seeds: solid cells with an empty (or out-of-box) 6-neighbour in the UNION.
  const queue = new Int32Array(n);
  let head = 0, tail = 0;
  for (let z = 0; z < dim.z; z++)
    for (let y = 0; y < dim.y; y++)
      for (let x = 0; x < dim.x; x++) {
        const i = x + y * sy + z * sz;
        if (!solid[i]) continue;
        const exposed =
          x === 0 || !solid[i - sx] || x === dim.x - 1 || !solid[i + sx] ||
          y === 0 || !solid[i - sy] || y === dim.y - 1 || !solid[i + sy] ||
          z === 0 || !solid[i - sz] || z === dim.z - 1 || !solid[i + sz];
        if (exposed) { depth[i] = 0; queue[tail++] = i; }
      }

  // BFS inward. Depth saturates at 254 so the sentinel stays unambiguous —
  // a model 500 voxels thick is not a mob.
  while (head < tail) {
    const i = queue[head++];
    const d = depth[i];
    const nd = d < 254 ? d + 1 : 254;
    const x = i % dim.x, y = ((i / dim.x) | 0) % dim.y, z = (i / sz) | 0;
    const step = (j) => {
      if (solid[j] && depth[j] === DEPTH_EMPTY) { depth[j] = nd; queue[tail++] = j; }
    };
    if (x > 0) step(i - sx);
    if (x < dim.x - 1) step(i + sx);
    if (y > 0) step(i - sy);
    if (y < dim.y - 1) step(i + sy);
    if (z > 0) step(i - sz);
    if (z < dim.z - 1) step(i + sz);
  }

  // A LIMB'S OWN FACES ARE SURFACE. Applied after the BFS rather than seeded
  // into it on purpose: seeding would measure the rows under a joint face
  // from that face too, and a neck five voxels tall would then be skin /
  // flesh / muscle / flesh / skin with no bone in it. Overriding only the
  // face keeps the union's depth underneath, so the core is continuous
  // through the joint and only the face itself changes.
  prefab.models.forEach((m, mi) => {
    if (visible && !visible(mi)) return;
    const d = m.dim, data = m.grid.data, o = m.offset;
    const at = (x, y, z) =>
      x < 0 || y < 0 || z < 0 || x >= d.x || y >= d.y || z >= d.z
        ? 0 : data[x + y * d.x + z * d.x * d.y];
    for (let z = 0; z < d.z; z++)
      for (let y = 0; y < d.y; y++)
        for (let x = 0; x < d.x; x++) {
          if (!at(x, y, z)) continue;
          if (!at(x - 1, y, z) || !at(x + 1, y, z) ||
              !at(x, y - 1, z) || !at(x, y + 1, z) ||
              !at(x, y, z - 1) || !at(x, y, z + 1))
            depth[(x + o.x) + (y + o.y) * sy + (z + o.z) * sz] = 0;
        }
  });
  return { dim, depth };
}

/** Depth of a MODEL-LOCAL cell, or DEPTH_EMPTY. */
export function depthAt(field, model, lx, ly, lz) {
  const x = lx + model.offset.x, y = ly + model.offset.y, z = lz + model.offset.z;
  const d = field.dim;
  if (x < 0 || y < 0 || z < 0 || x >= d.x || y >= d.y || z >= d.z) return DEPTH_EMPTY;
  return field.depth[x + y * d.x + z * d.x * d.y];
}

/** The deepest depth any voxel reaches (the number of peelable layers). */
export function maxDepth(field) {
  let m = 0;
  for (let i = 0; i < field.depth.length; i++) {
    const d = field.depth[i];
    if (d !== DEPTH_EMPTY && d > m) m = d;
  }
  return m;
}

/**
 * The layer list a limb uses: its override in `recipe.limbs[name]`, else the
 * recipe's own. Returns a normalised copy: each entry {material, depth|null,
 * keep, speckle|null}, and the last entry is the CORE (depth null).
 */
export function layersFor(recipe, limbName) {
  const src = (recipe.limbs && limbName && recipe.limbs[limbName] &&
               recipe.limbs[limbName].layers) || recipe.layers || [];
  const out = src.map(l => ({
    material: l.material ?? null,
    depth: l.depth == null ? null : Math.max(0, l.depth | 0),
    keep: !!l.keep,
    speckle: l.speckle && l.speckle.material
      ? { material: l.speckle.material,
          fraction: Math.max(0, Math.min(1, +l.speckle.fraction || 0)) }
      : null,
  }));
  return out;
}

/**
 * Which layer index a depth falls in. Layers are consumed outermost first,
 * each `depth` voxels thick; the last layer (or any with depth null) takes
 * the rest. Returns -1 when the schedule runs out before the core, which
 * cannot happen when the last layer is open-ended.
 */
export function layerIndexAt(layers, depth) {
  let d = depth;
  for (let i = 0; i < layers.length; i++) {
    const l = layers[i];
    if (l.depth == null || i === layers.length - 1) return i;
    if (d < l.depth) return i;
    d -= l.depth;
  }
  return -1;
}

// Deterministic per-cell hash on prefab coordinates (the sim's hash3 shape,
// not its constants — nothing here reaches the world grid, this only has to
// be stable so re-applying a recipe changes nothing).
export function cellHash(x, y, z, salt = 0) {
  let h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791) ^ (salt * 2654435761);
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
  return (h ^ (h >>> 16)) >>> 0;
}

/**
 * Plan a recipe over a prefab. Nothing is written: the result is a list of
 * per-model edits the caller applies (through the editor's undo log, or
 * straight into the grids for a bake).
 *
 * @param prefab   { size, models:[{name, offset, dim, grid:{data,color}}] }
 * @param recipe   see the module comment; missing fields default sanely
 * @param matId    name -> material id (1..127), e.g. from materials.json
 * @returns {{
 *   edits: Array<{mi:number, cells:Int32Array, mats:Uint8Array}>,  // art -> 0
 *   field: {dim, depth},
 *   report: { perLimb: {[name]: {[material]: count}}, total:number,
 *             unresolved: string[] }
 * }}
 */
export function planAnatomy(prefab, recipe, matId) {
  const field = unionDepth(prefab);
  const garmentIds = new Set((recipe.garments || []).map(n => matId[n]).filter(Boolean));
  const unresolved = new Set();
  const resolve = (name) => {
    if (!name) return 0;
    const id = matId[name];
    if (!id) unresolved.add(name);
    return id | 0;
  };

  const { dim } = field;
  const sy = dim.x, sz = dim.x * dim.y;
  // Garment cells in prefab space, so "under a garment" can be asked across
  // model boundaries (the shorts are the hips model; the thigh is another).
  const garment = new Uint8Array(dim.x * dim.y * dim.z);
  if (garmentIds.size) {
    for (const m of prefab.models) {
      const d = m.dim, data = m.grid.data, o = m.offset;
      for (let z = 0; z < d.z; z++)
        for (let y = 0; y < d.y; y++) {
          const row = y * d.x + z * d.x * d.y;
          const prow = o.x + (y + o.y) * sy + (z + o.z) * sz;
          for (let x = 0; x < d.x; x++) {
            const v = data[row + x];
            if (v && garmentIds.has(v) && field.depth[prow + x] === 0)
              garment[prow + x] = 1;
          }
        }
    }
  }
  const underGarment = (px, py, pz) => {
    const i = px + py * sy + pz * sz;
    return (px > 0 && garment[i - 1]) || (px < dim.x - 1 && garment[i + 1]) ||
           (py > 0 && garment[i - sy]) || (py < dim.y - 1 && garment[i + sy]) ||
           (pz > 0 && garment[i - sz]) || (pz < dim.z - 1 && garment[i + sz]);
  };

  const edits = [];
  const perLimb = {};
  let total = 0;
  prefab.models.forEach((m, mi) => {
    const layers = layersFor(recipe, m.name);
    if (!layers.length) return;
    const ids = layers.map(l => resolve(l.material));
    const speck = layers.map(l => l.speckle ? resolve(l.speckle.material) : 0);
    const surfaceId = ids[0];
    // What this recipe puts under the surface. A `keep` voxel made of one of
    // these is a face that was once baked as interior, not the painted
    // character, and it gets the kept layer's material back (see the module
    // comment on `keep`).
    const interior = new Set();
    layers.forEach((l, li) => {
      if (l.keep) return;
      if (ids[li]) interior.add(ids[li]);
      if (speck[li]) interior.add(speck[li]);
    });
    const d = m.dim, data = m.grid.data, color = m.grid.color, o = m.offset;
    const cells = [], mats = [];
    const hist = {};
    for (let z = 0; z < d.z; z++)
      for (let y = 0; y < d.y; y++) {
        const row = y * d.x + z * d.x * d.y;
        for (let x = 0; x < d.x; x++) {
          const i = row + x;
          const cur = data[i];
          if (!cur) continue;
          const px = x + o.x, py = y + o.y, pz = z + o.z;
          const depth = field.depth[px + py * sy + pz * sz];
          const li = layerIndexAt(layers, depth);
          if (li < 0) continue;
          const L = layers[li];
          if (L.keep && !interior.has(cur)) continue;
          let mat = ids[li], label = L.material;
          // Skin under the shorts: the first layer's material, one cell deep.
          if (depth === 1 && surfaceId && garmentIds.size && underGarment(px, py, pz)) {
            mat = surfaceId; label = layers[0].material;
          } else if (speck[li] && L.speckle.fraction > 0 &&
                     (cellHash(px, py, pz, li + 1) % 10000) < L.speckle.fraction * 10000) {
            mat = speck[li]; label = L.speckle.material;
          }
          if (!mat) continue;                      // unresolved name: leave it
          const art = color ? color[i] : 0;
          if (cur === mat && art === 0) continue;  // already there: idempotent
          cells.push(i); mats.push(mat);
          hist[label] = (hist[label] || 0) + 1;
        }
      }
    if (cells.length) {
      edits.push({ mi, cells: Int32Array.from(cells), mats: Uint8Array.from(mats) });
      perLimb[m.name || String(mi)] = hist;
      total += cells.length;
    }
  });
  return { edits, field, report: { perLimb, total, unresolved: [...unresolved] } };
}

/** Apply a plan straight into the grids (the bake path; the editor routes
 *  the same edits through its undo log instead). Clears art on every cell. */
export function applyPlan(prefab, plan) {
  for (const e of plan.edits) {
    const g = prefab.models[e.mi].grid;
    for (let k = 0; k < e.cells.length; k++) {
      const i = e.cells[k];
      g.data[i] = e.mats[k];
      if (g.color) g.color[i] = 0;
    }
  }
  return plan.report.total;
}

/**
 * Material histogram of one model's voxels at exactly `depth` (the layer a
 * peel of `depth` exposes), keyed by material id. Used by the editor's
 * status line and the Fill-layer button.
 */
export function layerCells(field, model, depth) {
  const d = model.dim, data = model.grid.data, o = model.offset;
  const { dim } = field;
  const sy = dim.x, sz = dim.x * dim.y;
  const cells = [];
  const hist = {};
  for (let z = 0; z < d.z; z++)
    for (let y = 0; y < d.y; y++) {
      const row = y * d.x + z * d.x * d.y;
      const prow = o.x + (y + o.y) * sy + (z + o.z) * sz;
      for (let x = 0; x < d.x; x++) {
        const v = data[row + x];
        if (!v || field.depth[prow + x] !== depth) continue;
        cells.push(row + x);
        hist[v] = (hist[v] || 0) + 1;
      }
    }
  return { cells, hist };
}

/** name -> id map from a materials.json `materials` array (id = index + 1). */
export function materialIds(materials) {
  const m = {};
  materials.forEach((e, i) => { if (e && e.id) m[e.id] = i + 1; });
  return m;
}
