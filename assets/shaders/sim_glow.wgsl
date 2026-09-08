// sim_glow.wgsl — the GLOW FIELD's producer (src/sim/world.h kGlowBytes;
// common.wgsl THE GLOW FIELD for the layout contract every reader shares).
//
// WHAT PROBLEM THIS SOLVES. Emission already becomes light correctly in this
// engine — P3 of docs/PLAN_gi.md deleted `heatSpill` (a four-tap per-pixel
// stand-in) and the per-ray ember probe, and routed lava, embers and burning
// foliage into the irradiance grid through `irrSample`, where `giGather` picks
// them up. But that path is only reachable from a FRAGMENT shader with
// `voxels`, `occupancy` and `pageTable` bound, and only for nine coarse rays
// that stop after TUNE_GI_GATHER_BLOCKS blocks (1.2 m at the shipped 3).
//
// Three consumers are outside it and all three are the same complaint:
//
//   * debris.wgsl's rigid-body and particle cubes shade in the VERTEX stage,
//     where renderBGL_ marks `voxels` and `pageTable` Fragment-only. The crown
//     that falls off a burning tree is a rigid body.
//   * microbody.wgsl's mob limbs shade per fragment but cannot pay nine rays
//     per limb voxel.
//   * Neither contributes to, or receives from, the irradiance grid at all —
//     both injectors read the voxel grid only. A mob standing in a lava pit is
//     lit by the sky.
//
// So this pass bakes a POSITION-KEYED field those paths can read with one
// buffer load and no ray: `glowAtCell` in common.wgsl is the whole consumer
// side. That is voxelbit's idea (build/voxelbit_study) generalised — its "4-bit
// glow field" is in fact a per-PIXEL G-buffer channel filled by a downward
// probe ray for lava and an eight-light loop for fireflies, which is a per-pixel
// search of exactly the kind this repo has twice deleted. A world-space field
// is the version that scales.
//
// TWO STAGES, TWO DISPATCHES, ONE BARRIER BETWEEN THEM.
//
//   `src`   one workgroup per DIRTY chunk. Reads every cell of the chunk once,
//           sums the radiance its emitters leave, and stores ONE word: the
//           chunk's emitted radiance. Also stores the slot stamp, the emitter
//           count (a diagnostic the gate reads) and a `changed` flag.
//   `field` one workgroup per DIRTY chunk. Gathers the 3x3x3 chunk
//           neighbourhood of source words with a distance falloff and writes
//           the per-4^3-block field. `src` must have finished for the whole
//           list first, which is what the two rows in pass_table.def and the
//           RW(Glow) -> RW(Glow) edge the recorder derives from them are for.
//   `refresh` a rolling slice, both stages in one workgroup. The backstop.
//
// WHY THE SOURCE IS PER CHUNK AND THE FIELD PER BLOCK. The field at a point is
// a sum over emitters within render.glowReach, and gather cost goes as the cube
// of the reach in source cells. Per-VOXEL sources at a 2.4 m reach would be a
// 48^3 = 110,592-tap gather per block. Quantising the SOURCE to a chunk (1.6 m)
// makes the same reach a 27-tap gather; evaluating the FALLOFF per 40 cm block
// is what keeps the result looking like light rather than like a stack of
// 1.6 m cubes. The source quantisation costs spatial precision of WHERE the
// emitter is, which at 1.6 m against a 2.4 m falloff is invisible; a coarse
// FIELD would cost the shape of the falloff itself, which is not.
//
// HOW IT CANNOT LATCH — the [[gotcha-irradiance-charger-with-no-expiry]] case.
// The irradiance grid is an EMA with two writers and only one that can
// discharge, and a face its walk declines to measure keeps yesterday's sun
// forever. This field has no memory at all: every visit RECOMPUTES the source
// word from the voxels standing in the chunk right now and OVERWRITES it, and
// every field word is recomputed from source words the same way. Remove the
// lava and the removal dirties the chunk, `src` writes 0, `changed` fires, and
// `field` rewrites the whole 3x3x3 ring to 0 on the same tick. There is no
// decay constant to tune because there is nothing to decay: "could not measure
// it" cannot arise when the measurement is unconditional.
//
// HOW IT IS BOUNDED — rule 2. Three separate ceilings, none of them a threshold
// anybody has to tune:
//
//   1. The source is a MEAN over the emitters times a saturating fill factor,
//      so a chunk solid with lava and a chunk with one lava voxel differ by the
//      fill factor and NEITHER can exceed the brightest material's own
//      radiance. Doubling the amount of lava in the world cannot double the
//      field. A sum would have had no ceiling at all.
//   2. The ring rewrite — the 27-chunk scatter, the only expensive branch — is
//      taken only by a workgroup whose source word MOVED this visit, and the
//      value is quantised through RGB9E5 first, so a lava pool whose surface
//      ripples does not move it. A settled world takes it zero times.
//   3. Even when everything changes at once (an explosion turning a wood grove
//      to fire), only the first TUNE_GLOW_RING_BUDGET workgroups of the
//      compacted dirty list take the branch. The rest write their own chunk and
//      are picked up on a later tick or by `refresh`. That is a hard per-tick
//      ceiling of budget x 27 x OPEN_BLOCKS stores with no atomic and no
//      allocation.
//
// AND IT CANNOT WAKE A CHUNK. Nothing here writes `voxels`, `dirtyIn`,
// `dirtyOut`, `occupancy` or `pageFaults`. That is the whole of its claim to
// being render-only and it is checkable — the `sleep` gate would notice.

// `> voxels` with EXACTLY ONE SPACE, and that is not cosmetic: LoadShader
// (gpu/resources.cpp BodyAddressesVoxels) decides whether to keep or strip
// common.wgsl's page-table block by searching this body for that literal, and
// voxWordAt lives inside that block. Aligned columns here cost every voxel read
// in this file an `unresolved call target`.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(12) var<storage, read> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(32) var<storage, read_write> glow : array<u32>;

// ONE THREAD PER 4^3 BLOCK. 64 at kSubOccShift = 2 — two full warps, and the
// same count both stages need: `src` gives each thread one block's 64 cells to
// scan, `field` gives it one block's field word to write. It follows
// kSubOccShift automatically (8 threads at shift 3, which is small but correct).
const GLOW_THREADS : u32 = OPEN_BLOCKS;

// The 3x3x3 chunk stencil, as a flat 0..26 index. Centre is 13.
const GLOW_STENCIL : u32 = 27u;

// The workgroup's per-block partial sums, reduced by thread 0 into the chunk's
// one source word. vec3 rather than a scalar because the field carries COLOUR —
// see the RGB9E5 argument in world.h.
var<workgroup> gPart : array<vec3f, GLOW_THREADS>;
var<workgroup> gCount : array<u32, GLOW_THREADS>;

// ---------------------------------------------------------------- source ----

// The world CHUNK coord of a slot, from an origin the caller read ONCE.
// T.origin is read directly rather than through common.wgsl's
// worldCellOfSlotLocal for sim_openness.wgsl's reason: the generated ptOrigin()
// accessor is invisible to check_pass_table.py's rooted walk, so a row that
// correctly declares U(TickUBO) for it gets reported as a spurious barrier.
fn glowWorldChunk(slot : u32, origin : vec3<i32>) -> vec3<i32> {
  let sc = vec3<i32>(i32(slot % NCHUNK), i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  return slotToWorldChunk(sc, origin);
}

// One 4^3 block's contribution: the radiance its emitting cells leave, summed,
// plus how many there were.
//
// THE EMISSION GOES THROUGH burnTint AND burnTintMean, not through a raw
// `emission / 255`. `check_burn_tint_sites` in scripts/check_invariants.py
// refuses the raw idiom outside a burnTint() call and it is right to: leaf,
// pine and autumn `_burning` are authored in the GREEN of the leaf they were
// and the renderer supplies the fire, so a raw read here would light a whole
// forest fire's surroundings leaf-green — which is the exact bug that memory
// note records happening four times in one day. The MEAN weight, not the
// clocked one, for irrSample's reason: this field is read every frame and must
// not beat with the burn breath.
fn glowBlockSum(blockMin : vec3<i32>, part : ptr<function, vec3f>,
                count : ptr<function, u32>) {
  let n = i32(SUBOCC_BLOCK);
  for (var z = 0; z < n; z++) {
    for (var y = 0; y < n; y++) {
      for (var x = 0; x < n; x++) {
        let w = voxWordAt(blockMin + vec3<i32>(x, y, z));
        let mi = voxMat(w);
        if (mi == MAT_AIR) { continue; }
        let m = materials[mi];
        if (m.emission == 0u) { continue; }
        let bt = burnTint(m, paletteColor(m, voxState(w), &materials),
                          f32(m.emission) / 255.0, burnTintMean());
        // The radiance the cell leaves, at the SAME strength the primary hit
        // shades it with (TUNE_EMISSIVE_STRENGTH) and by the same expression
        // irrSample uses with the sun term zeroed. Two conventions for "how
        // bright is this emitter" is exactly the divergence the eight-places
        // note is about.
        *part += bt.albedo * bt.emis * TUNE_EMISSIVE_STRENGTH;
        *count += 1u;
      }
    }
  }
}

// Fold the workgroup's partials into the chunk's ONE source word and store it.
// Returns whether the word moved, which is what gates the ring rewrite.
//
// THE FILL FACTOR IS THE CEILING. `mean` is the average radiance of the cells
// that emit — bounded by the brightest material in the world, whatever the
// count. `fill` is what fraction of the chunk emits, scaled by
// render.glowFill and SATURATED at 1: a chunk that is 1/glowFill emitting
// already reads as a full-strength light source, and more lava in it changes
// nothing. So `src <= max material radiance`, structurally, and a lava lake
// cannot make this field brighter than one lava voxel's own surface. That is
// the bound rule 2 asks for, and it is a property of the expression rather than
// a clamp somebody has to keep in step with the content.
fn glowStoreSrc(slot : u32, stamp : u32, sum : vec3f, count : u32) -> bool {
  let base = glowSrcIndex(slot);
  var packed = 0u;
  if (count > 0u) {
    let mean = sum / f32(count);
    let fill = clamp(f32(count) / f32(CHUNK_VOL) * TUNE_GLOW_FILL, 0.0, 1.0);
    packed = packRgb9e5(mean * fill);
  }
  // COMPARE BEFORE THE STAMP IS REFRESHED. A slot the window has just reused
  // holds another chunk's word, and "unchanged" against it would be a lie — the
  // ring around it is still lit by a chunk that has gone. A stamp mismatch is
  // therefore always a change.
  let stampOk = glow[base + 1u] == stamp;
  let moved = !stampOk || glow[base] != packed;
  glow[base] = packed;
  glow[base + 1u] = stamp;
  glow[base + 2u] = count;
  glow[base + 3u] = select(0u, 1u, moved);
  return moved;
}

// ----------------------------------------------------------------- field ----

// The source word of a neighbouring chunk, or zero if that slot describes a
// different chunk than the one we want (the window is toroidal). Taking the
// stamp seriously here is what stops a lava pit lighting a wall 51.2 m away
// through the wrap.
fn glowSrcOf(wc : vec3<i32>) -> vec3f {
  let slot = chunkSlotIndex(wc);
  let base = glowSrcIndex(slot);
  if (glow[base + 1u] != opennessStamp(wc)) { return vec3f(0.0); }
  return unpackRgb9e5(glow[base]);
}

// The field for ONE 4^3 block of ONE chunk: the 27 source words around it,
// weighted by a squared linear falloff on the distance from the block's centre
// to each source chunk's centre.
//
// A SQUARED LINEAR FALLOFF, NOT 1/d^2. Inverse-square is the physics of a POINT
// source and this source is a 1.6 m box: at the distances that matter here the
// receiver is inside or adjacent to the emitter's own box, where 1/d^2 goes to
// infinity and the field would be a hard white core with a ring of banding
// around it. `(1 - d/reach)^2` reaches exactly zero AT the reach — so the 3x3x3
// stencil is not a truncation artifact, it is the whole support of the kernel
// as long as render.glowReach stays under one chunk plus half a block diagonal
// (LoadTuning clamps it there) — and it has the soft shoulder a box light
// actually has.
fn glowFieldAt(wc : vec3<i32>, block : u32) -> vec3f {
  let bx = block % SUBOCC_DIM;
  let by = (block / SUBOCC_DIM) % SUBOCC_DIM;
  let bz = block / (SUBOCC_DIM * SUBOCC_DIM);
  let half = f32(SUBOCC_BLOCK) * 0.5;
  let centre = vec3f(wc * i32(CHUNK)) +
               vec3f(f32(bx), f32(by), f32(bz)) * f32(SUBOCC_BLOCK) +
               vec3f(half);
  let reachVox = max(TUNE_GLOW_REACH / VOXEL_METERS, 1.0);
  let chunkHalf = f32(CHUNK) * 0.5;
  var acc = vec3f(0.0);
  for (var i = 0u; i < GLOW_STENCIL; i++) {
    let o = vec3<i32>(i32(i % 3u) - 1, i32((i / 3u) % 3u) - 1,
                      i32(i / 9u) - 1);
    let nwc = wc + o;
    let s = glowSrcOf(nwc);
    // An early-out on the common case: most of the 27 are empty of emitters and
    // the falloff is the expensive half.
    if (all(s <= vec3f(0.0))) { continue; }
    let ncentre = vec3f(nwc * i32(CHUNK)) + vec3f(chunkHalf);
    let d = length(centre - ncentre);
    let w = max(0.0, 1.0 - d / reachVox);
    acc += s * (w * w);
  }
  return acc;
}

// One chunk's 64 field words, one per thread.
fn glowWriteField(wc : vec3<i32>, block : u32) {
  glow[glowFieldIndex(chunkSlotIndex(wc), block)] =
      packRgb9e5(glowFieldAt(wc, block));
}

// ---------------------------------------------------------------- passes ----

// STAGE 1, over the tick's compacted dirty list — the same list and the same
// indirect args `occupancyDirty` and the openness walk ride. This is the half
// that makes an edit visible: pour lava and the chunk it lands in is on the
// list that tick, so its source word is right before the frame that draws it.
@compute @workgroup_size(GLOW_THREADS)
fn src(@builtin(workgroup_id) wg : vec3<u32>,
       @builtin(local_invocation_index) li : u32) {
  let slot = dirtyList[wg.x];
  let wc = glowWorldChunk(slot, T.origin);
  let bx = li % SUBOCC_DIM;
  let by = (li / SUBOCC_DIM) % SUBOCC_DIM;
  let bz = li / (SUBOCC_DIM * SUBOCC_DIM);
  var part = vec3f(0.0);
  var count = 0u;
  glowBlockSum(wc * i32(CHUNK) +
                   vec3<i32>(i32(bx), i32(by), i32(bz)) * i32(SUBOCC_BLOCK),
               &part, &count);
  gPart[li] = part;
  gCount[li] = count;
  workgroupBarrier();
  if (li == 0u) {
    var sum = vec3f(0.0);
    var n = 0u;
    for (var k = 0u; k < GLOW_THREADS; k++) { sum += gPart[k]; n += gCount[k]; }
    glowStoreSrc(slot, opennessStamp(wc), sum, n);
  }
}

// STAGE 2, over the SAME list, after stage 1 has finished for all of it.
//
// THE RING IS THE WHOLE REASON THIS IS A SECOND DISPATCH. The field at a block
// depends on source words up to one chunk away, so when a chunk's emitters
// change the 26 chunks around it are stale too — and they are not on the dirty
// list, because nothing in them moved. Writing only our own chunk would mean a
// torch lit a wall and nothing else until the rolling refresh came round, which
// at any sane budget is seconds.
//
// So a workgroup whose source MOVED writes all 27 chunks' fields. Two adjacent
// dirty chunks then write the same neighbour's words twice — with the SAME
// value, because `glowFieldAt` is a pure function of the source region, so the
// overlap is benign and needs no arbitration. (It is also render-only: even a
// torn word would be one frame of one 40 cm block.)
//
// The budget is a prefix of the dirty list rather than an atomic counter, which
// keeps this allocation-free and needs no extra buffer; the compaction is in
// slot order, so which chunks win is stable rather than scheduling-dependent.
@compute @workgroup_size(GLOW_THREADS)
fn field(@builtin(workgroup_id) wg : vec3<u32>,
         @builtin(local_invocation_index) li : u32) {
  let slot = dirtyList[wg.x];
  let wc = glowWorldChunk(slot, T.origin);
  let ring = glow[glowSrcIndex(slot) + 3u] != 0u &&
             wg.x < u32(max(TUNE_GLOW_RING_BUDGET, 0));
  if (!ring) {
    glowWriteField(wc, li);
    return;
  }
  for (var i = 0u; i < GLOW_STENCIL; i++) {
    let o = vec3<i32>(i32(i % 3u) - 1, i32((i / 3u) % 3u) - 1,
                      i32(i / 9u) - 1);
    glowWriteField(wc + o, li);
  }
}

// THE BACKSTOP: a flat TUNE_GLOW_CHUNKS slots per tick, round robin, doing both
// stages in one workgroup. The cursor is derived from T.tick rather than stored,
// for the reason sim_openness.wgsl's refresh states — it cannot go stale, and a
// tick this row was not recorded on simply skips its slice.
//
// It reads its neighbours' source words in the same dispatch that may be
// writing them, so its field is computed from a MIX of this tick's and last
// tick's sources. That is deliberate and harmless: this row exists to heal a
// slot the dirty walk could not reach (a cold start, or a chunk that streamed
// into a slot whose old words are still there), the error is one tick of one
// neighbour's contribution, and the next visit corrects it. Paying for a third
// dispatch and a second barrier to make a backstop exact would be paying the
// whole window's cost for a case the dirty walk already handles.
@compute @workgroup_size(GLOW_THREADS)
fn refresh(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let cursor = (T.tick * u32(max(TUNE_GLOW_CHUNKS, 1))) % NUM_CHUNKS;
  let slot = (cursor + wg.x) % NUM_CHUNKS;
  let wc = glowWorldChunk(slot, T.origin);
  let bx = li % SUBOCC_DIM;
  let by = (li / SUBOCC_DIM) % SUBOCC_DIM;
  let bz = li / (SUBOCC_DIM * SUBOCC_DIM);
  var part = vec3f(0.0);
  var count = 0u;
  glowBlockSum(wc * i32(CHUNK) +
                   vec3<i32>(i32(bx), i32(by), i32(bz)) * i32(SUBOCC_BLOCK),
               &part, &count);
  gPart[li] = part;
  gCount[li] = count;
  workgroupBarrier();
  if (li == 0u) {
    var sum = vec3f(0.0);
    var n = 0u;
    for (var k = 0u; k < GLOW_THREADS; k++) { sum += gPart[k]; n += gCount[k]; }
    glowStoreSrc(slot, opennessStamp(wc), sum, n);
  }
  workgroupBarrier();
  glowWriteField(wc, li);
}
