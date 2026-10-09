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
 *  - `carve` (per limb, `limbs.<name>.carve`) is what turns a bone CORE into a
 *    SKELETON: depth alone makes an ellipsoid shell, which has no eye sockets
 *    and no ribs. Each rule is
 *        { "material": "flesh", "where": "bone",
 *          "box":   { "x": [lo, hi], "y": [lo, hi], "z": [lo, hi] },
 *          "depth": [min, max],
 *          "stripe": { "axis": "y", "period": 4, "width": 2 } }
 *    and every field but `material` is optional. A cell matches when the
 *    schedule put `where` there, its centre lies in the box (FRACTIONS of the
 *    limb's own box, lo inclusive, hi exclusive, so one rule fits every
 *    generated body; +z is the FRONT, +y is up), its depth is in range, and
 *    (limb-local index along `axis`) % period < width. Rules run in order and
 *    the LAST match wins, so a rule list reads as "hollow it, then put the
 *    ribs back". Kept layers are never carved: the surface is the character.
 *  - `"core": k` also requires the cell to lie within k of the DEEPEST cell of
 *    its own cross-section, slicing across the limb's longest box axis
 *    (`coreAxis`). It is what makes a long bone a SHAFT: "everything deeper
 *    than the muscle layer" grows with the limb, so a muscular arm was a bone
 *    log with a skin on it, while the deepest cells of each slice are a thin
 *    rod however thick the meat around it is.
 *  - A recipe-level `carve` is the DEFAULT for every limb with neither a
 *    `carve` nor `layers` of its own (`"carve": []` opts a limb out).
 */

export const ANATOMY_VERSION = 1;

/** THE STOCK HUMAN RECIPE, and the ONE copy of it.
 *
 * `mobgen.anatomyRecipe()` returns this with the authoring notes a sidecar
 * carries attached; `human.json` carries that; the editor applies this to a mob
 * whose sidecar has no `anatomy` block; `scripts/test_anatomy.mjs` pins it.
 * Four readers, one definition — which it was not until 2026-09-20, when this
 * constant still had `flesh` inside the skull where every other copy said
 * `brain`, and the test pinned the stale value. A recipe with two copies is the
 * same class of bug as a body with two copies, one layer up. */
export const DEFAULT_ANATOMY = {
  layers: [
    { material: 'skin', depth: 1, keep: true },
    { material: 'flesh', depth: 1 },
    { material: 'muscle', depth: 1, speckle: { material: 'blood', fraction: 0.06 } },
    { material: 'bone' },
  ],
  // The human's shorts are the `linen` MATERIAL and not paint, because burning
  // a voxel clears its art colour and painted-on clothing would char into bare
  // flesh. So the clause is live: it is what puts skin rather than flesh under
  // them. (Generated bodies paint their shorts instead and have no linen at
  // all, which makes it inert there — and means a generated character has
  // flesh where the human has skin. A real asymmetry, in the ART and not in
  // this recipe.)
  garments: ['linen'],
  // LONG BONES ARE SHAFTS. The default carve for every limb without its own
  // (arms, legs, hands, feet, tails): bone where the schedule put it ONLY in
  // the deepest cells of each cross-section (`core`, anatomy.js header); the
  // rest of the core is muscle. Bone thickness follows the limb's LENGTH, not
  // its bulk -- a femur is ~6% of its length across, two voxels on every body
  // here -- and "everything deeper than the muscle" made a muscular upper arm
  // 1,800 cells of bone against the stock human's 78. A stock limb is
  // unchanged: its 2x2 core already IS the deepest cells.
  carve: [
    { material: 'muscle', where: 'bone' },
    { material: 'bone', where: 'bone', core: 0 },
  ],
  limbs: {
    // THE SKULL IS A SHELL AND WHAT IT PROTECTS IS NOT BONE. The flesh ring
    // over it is the SCALP and is not decoration: without it bone sits one
    // voxel under the paint, so a graze that should have opened a scalp rings
    // off the skull and the wound model's depth weights (skin/flesh 1,
    // muscle 2, bone 3) start at 3 for every head hit. The bone is two deep
    // because the rot chews it at half rate (bone.rotRate), so breaching takes
    // time and what is behind goes fast; `brain` is open-ended, so it is by
    // definition everything the shell encloses — there is no geometric brain
    // test anywhere in the engine and this recipe is the only thing that
    // decides what is one.
    head: {
      layers: [
        { material: 'skin', depth: 1, keep: true },
        { material: 'flesh', depth: 1 },
        { material: 'bone', depth: 2 },
        { material: 'brain' },
      ],
      // A SKULL, NOT AN EGG. With the skin and scalp gone the shell has to read
      // as a face: two orbits behind the painted eyes and a nasal hole between
      // and below them, filled with soft tissue that goes before the bone does.
      //
      // THE ORBITS ARE MUSCLE, NOT FLESH. Flesh is also the scalp, so with the
      // skin gone a flesh-filled orbit was the same pink as the whole face, and
      // after the scalp went it was a pale pink dot in pale bone a third of a
      // world voxel across: a player who melted a face with acid saw no eye
      // sockets at all (2026-09-23). Dark red against bone reads as a hole.
      //
      // Boxes are fractions of the head box. Painted eyes sit at x 4-5 / 14-15
      // on the CURVE of the face, where the bone is three layers back; the
      // z >= 0.6 floor is what reaches it. The generated heads paint their eyes
      // a fixed count down from the TOP (y 16-17 of 32 on the human, 14-15 of
      // 30 on newcomer), so no two-row fraction from the bottom fits both: the
      // y box is three rows tall on purpose (an orbit is bigger than the eye).
      carve: [
        { material: 'muscle', where: 'bone',
          box: { x: [0.2, 0.4], y: [0.47, 0.57], z: [0.6, 1] } },
        { material: 'muscle', where: 'bone',
          box: { x: [0.6, 0.8], y: [0.47, 0.57], z: [0.6, 1] } },
        { material: 'flesh', where: 'bone',
          box: { x: [0.45, 0.55], y: [0.4, 0.47], z: [0.7, 1] } },
      ],
    },
    // A RIBCAGE, NOT A BONE BRICK. Depth alone fills the chest with solid bone
    // from depth 3 inward. Instead: the organs (flesh) inside, a muscle wall at
    // depth 3, then bone put back as ribs (2 rows on, 2 off, over the chest), a
    // sternum down the front of the cage and a spine down the whole back.
    torso: {
      carve: [
        { material: 'flesh', where: 'bone' },
        { material: 'muscle', where: 'bone', depth: [3, 3] },
        { material: 'bone', where: 'bone', depth: [3, 3],
          box: { y: [0.42, 0.94] }, stripe: { axis: 'y', period: 4, width: 2 } },
        { material: 'bone', where: 'bone', depth: [3, 3],
          box: { x: [0.45, 0.55], y: [0.42, 0.94], z: [0.5, 1] } },
        { material: 'bone', where: 'bone',
          box: { x: [0.4, 0.6], z: [0, 0.5] } },
      ],
    },
    // A PELVIS, NOT A BONE BRICK. The hips are the widest limb, so depth alone
    // made them a solid 14x6 bone block -- ~1,050 cells against a femur's ~100,
    // where a real pelvis is a RING of plates about 1.5 femurs by mass. Same
    // move as the chest: the pelvic organs (flesh) inside, a muscle wall at
    // depth 3, then bone put back where a pelvis has it. Bands are measured up
    // from the femoral heads (hips y ~0.1): acetabula solid over them; the
    // brim a full ring just above; the iliac wings a plate round the sides and
    // back, OPEN at the front (the belly), to the crest ~10 cm above the hip
    // joint (y 0.65) -- everything higher is waist, lumbar spine only; a pubic
    // symphysis at the front between the sockets; and the sacrum carrying the
    // torso's spine column down into the ring. ~390 cells, ~85 of them spine.
    hips: {
      carve: [
        { material: 'flesh', where: 'bone' },
        { material: 'muscle', where: 'bone', depth: [3, 3] },
        { material: 'bone', where: 'bone', depth: [3, 3],
          box: { y: [0.35, 0.65], z: [0, 0.7] } },
        { material: 'bone', where: 'bone', depth: [3, 3],
          box: { y: [0.25, 0.35] } },
        { material: 'bone', where: 'bone',
          box: { x: [0, 0.35], y: [0, 0.25] } },
        { material: 'bone', where: 'bone',
          box: { x: [0.65, 1], y: [0, 0.25] } },
        { material: 'bone', where: 'bone', depth: [3, 3],
          box: { x: [0.4, 0.6], y: [0, 0.25], z: [0.5, 1] } },
        { material: 'bone', where: 'bone',
          box: { x: [0.4, 0.6], y: [0.15, 1], z: [0, 0.5] } },
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

/** True when every layer is `keep`: the recipe rewrites nothing in this
 *  limb, so it is not body and planAnatomy leaves it out of the depth union. */
export function keepOnly(layers) {
  return layers.length > 0 && layers.every(l => l.keep);
}

/**
 * A limb's `carve` rules (see the module comment), normalised: each
 * {material, where|null, box: {x,y,z: [lo,hi]|null}, depth: [min,max],
 * stripe: {axis:0|1|2, period, width}|null}. Empty when the limb has none.
 */
export function carveFor(recipe, limbName) {
  // A limb's own `carve`, else the recipe's DEFAULT `carve` (the long bones)
  // -- but only for a limb on the DEFAULT `layers`: the default carve is
  // written against that schedule, and a limb with layers of its own (a skull,
  // a snout, a tail) is a different structure. An explicit `"carve": []` is a
  // limb opting out.
  const L = recipe.limbs && limbName && recipe.limbs[limbName];
  const own = L && L.carve;
  const src = Array.isArray(own) ? own
            : (L && L.layers) ? []
            : Array.isArray(recipe.carve) ? recipe.carve : [];
  const range = r => Array.isArray(r) && r.length === 2 ? [+r[0], +r[1]] : null;
  const out = [];
  for (const c of src) {
    if (!c || !c.material) continue;
    const b = c.box || {};
    const dep = Array.isArray(c.depth) ? c.depth : [];
    const s = c.stripe;
    const axis = s ? ['x', 'y', 'z'].indexOf(s.axis) : -1;
    out.push({
      material: c.material,
      where: c.where || null,
      box: { x: range(b.x), y: range(b.y), z: range(b.z) },
      depth: [dep[0] == null ? 0 : dep[0] | 0, dep[1] == null ? 254 : dep[1] | 0],
      stripe: axis >= 0 && (s.period | 0) > 0
        ? { axis, period: s.period | 0, width: s.width | 0 } : null,
      core: Number.isInteger(c.core) && c.core >= 0 ? c.core : null,
    });
  }
  return out;
}

/** The axis a limb's `core` slices across: its LONGEST box dimension (an arm
 *  or leg stands along y, a foot lies along z), ties going to y, then x. */
export function coreAxis(dim) {
  if (dim.y >= dim.x && dim.y >= dim.z) return 1;
  return dim.x >= dim.z ? 0 : 2;
}

/** The `core` test's field for one model: every voxel's CORE DEPTH and each
 *  slice's deepest one, slicing across `coreAxis`.
 *
 *  Core depth is IN-SLICE depth: 4-connected steps to the slice's own outline
 *  (the model's cells in that plane), not the union depth. Union depth has a
 *  flat top wherever the limb's END is the nearest surface -- under a shoulder
 *  every cell of the slice is four from the top face, so "the deepest cells"
 *  were the whole slice and the humerus grew an 88-cell head. The cross-section
 *  always has a centre. (The rule's `where: "bone"` still gates on the union
 *  schedule, so this never puts bone nearer a surface than the layers allow.)
 *
 *  Returns { axis, depth: Int32Array (model-local index, -1 for empty), max:
 *  Int32Array per slice index }. */
export function coreField(field, model) {
  const d = model.dim, data = model.grid.data, axis = coreAxis(d);
  const sx = 1, sy = d.x, sz = d.x * d.y;
  const n = d.x * d.y * d.z;
  // The two in-plane axes and their strides.
  const [ua, va] = [[1, 2], [0, 2], [0, 1]][axis];
  const dimA = [d.x, d.y, d.z], strA = [sx, sy, sz];
  const flat = new Int32Array(n).fill(-1);
  const queue = new Int32Array(n);
  let head = 0, tail = 0;
  const coord = i => [i % d.x, ((i / d.x) | 0) % d.y, (i / sz) | 0];
  for (let i = 0; i < n; i++) {
    if (!data[i]) continue;
    const c = coord(i);
    let edge = false;
    for (const a of [ua, va]) {
      if (c[a] === 0 || !data[i - strA[a]]) edge = true;
      if (c[a] === dimA[a] - 1 || !data[i + strA[a]]) edge = true;
    }
    if (edge) { flat[i] = 0; queue[tail++] = i; }
  }
  while (head < tail) {
    const i = queue[head++];
    const c = coord(i);
    for (const a of [ua, va])
      for (const s of [-1, 1]) {
        const k = c[a] + s;
        if (k < 0 || k >= dimA[a]) continue;
        const j = i + s * strA[a];
        if (data[j] && flat[j] < 0) { flat[j] = flat[i] + 1; queue[tail++] = j; }
      }
  }
  const depth = new Int32Array(n).fill(-1);
  const max = new Int32Array(dimA[axis]);
  for (let i = 0; i < n; i++) {
    if (!data[i]) continue;
    const c = coord(i);
    if (depthAt(field, model, c[0], c[1], c[2]) === DEPTH_EMPTY) continue;
    const e = flat[i];
    depth[i] = e;
    if (e > max[c[axis]]) max[c[axis]] = e;
  }
  return { axis, depth, max };
}

/** Does carve rule `c` take this model-local cell? `layerId` is the material
 *  the depth schedule put there, `whereId` the rule's resolved `where`. */
export function carveMatches(c, whereId, layerId, lx, ly, lz, dim, depth,
                             core = null) {
  if (c.where && whereId !== layerId) return false;
  if (depth < c.depth[0] || depth > c.depth[1]) return false;
  // `core`: { depth, max } of this cell from `coreField`.
  if (c.core != null && (!core || core.depth < core.max - c.core)) return false;
  const l = [lx, ly, lz], n = [dim.x, dim.y, dim.z];
  for (let a = 0; a < 3; a++) {
    const r = c.box[['x', 'y', 'z'][a]];
    if (!r) continue;
    const f = (l[a] + 0.5) / n[a];
    if (f < r[0] || f >= r[1]) return false;
  }
  if (c.stripe && (l[c.stripe.axis] % c.stripe.period) >= c.stripe.width)
    return false;
  return true;
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
  // A KEEP-ONLY LIMB IS NOT BODY. Its override keeps every voxel as it is
  // (hair: `limbs.hair.layers = [{material, keep}]`), so it has no interior
  // to schedule -- and if it stayed in the union it would bury the surface it
  // lies on. Measured on an afro, the hair made every scalp cell "interior"
  // and moved the skull eight cells into the head, shrinking the brain the
  // head's lethality is authored against. Left out, the skull sits where it
  // would on a bald head. anatomy_resolve.cpp makes the same exclusion.
  const field = unionDepth(prefab,
    mi => !keepOnly(layersFor(recipe, prefab.models[mi].name)));
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
    const carve = carveFor(recipe, m.name);
    const carveId = carve.map(c => resolve(c.material));
    const carveWhere = carve.map(c => c.where ? resolve(c.where) : 0);
    const cf = carve.some(c => c.core != null) ? coreField(field, m) : null;
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
    for (const id of carveId) if (id) interior.add(id);
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
          // The skeleton: last matching carve rule wins (never on a kept layer).
          if (!L.keep)
            for (let k = 0; k < carve.length; k++)
              if (carveId[k] && carveMatches(carve[k], carveWhere[k], ids[li],
                                             x, y, z, d, depth,
                                             cf && { depth: cf.depth[i],
                                                     max: cf.max[[x, y, z][cf.axis]] })) {
                mat = carveId[k]; label = carve[k].material;
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
