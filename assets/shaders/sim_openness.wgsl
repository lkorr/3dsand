// sim_openness.wgsl — the openness (sky-visibility) grid, phase 0 of indirect
// light (docs/PLAN_gi.md §2; src/sim/world.h, the kOpenFaces block).
//
// WHAT PROBLEM THIS SOLVES. Shading is `albedo * face * (ambientAt(n) * ao +
// sun)` and `ambientAt` is a hemisphere lerp on n.y — a pure function of the
// NORMAL, with no spatial term anywhere in it. So a cave floor is lit exactly
// as brightly as a meadow, a room is as bright as the field outside its door,
// and grass under a canopy is as bright as grass beside it. `voxelAO` is three
// taps in the plane of the face and cannot see a ceiling 3 m up. This pass is
// the missing term: for every 4^3 block face that has matter behind it, how
// much of that face's hemisphere is unblocked within render.opennessReach.
//
// WHAT IT WRITES. One BYTE per (chunk slot, 4^3 block, face), byte index
// ((slot * SUBOCC_DIM^3 + block) * OPEN_FACES + face), plus one stamp word per
// slot in `opennessGen`. Render-only derived data: the sim has no binding for
// either buffer, neither is hashed or saved, and `--gate determinism` is the
// cheapest proof that stayed true.
//
// THE MARCH IS traceOpaque, NOT A NEW DDA, and that is the load-bearing choice
// here. common.wgsl already carries one media-blind opaque DDA that every
// secondary ray in the engine shares (W2-A), and it already has a COARSE mode
// that steps 4^3 blocks over the blockers-class sub-occupancy mask instead of
// voxels — which is exactly, to the bit, the traversal this pass wants. Passing
// `coarseFromT = 0` makes it coarse from the first step. Writing a second
// block-stepping DDA here would be the classic two-implementations-of-one-
// question bug that file's header exists to warn about, and it would have to be
// kept in agreement with the shadow ray forever for no gain.
//
// WHY ONE WHOLE WORD PER THREAD. 6 bytes per block does not divide a u32, so a
// thread that owned one (block, face) would have to read-modify-write a word
// three other threads are also writing. The cures are atomics (a workgroup's
// worth of contention for a pure store) or 25% padding (16.7 MiB instead of
// 12). Instead a thread owns one WORD — four consecutive (block, face) pairs,
// which may straddle a block boundary — computes all four values and does one
// plain store. `% OPEN_FACES` in the writer buys a race-free grid.
//
// THE WORKGROUP IS OPEN_WORDS_PER_CHUNK THREADS (96 at kSubOccShift = 2, three
// full warps), so the mapping is thread -> word with no loop and no imbalance.
// It follows kSubOccShift automatically; at shift 3 it would be 12 threads,
// which is small but correct.

// `> voxels` with EXACTLY ONE SPACE, and that is not cosmetic: LoadShader
// (gpu/resources.cpp BodyAddressesVoxels) decides whether to keep or strip
// common.wgsl's page-table block by searching this body for that literal, and
// traceOpaque lives inside that block. Aligned columns here cost the whole
// march with an `unresolved call target` a hundred lines later.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read> occupancy : array<u32>;
@group(0) @binding(12) var<storage, read> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
// The key light, for P1's off-screen injection (below): the same RenderParams
// the sim group already carries for sim_pick.wgsl, so a compute pass on the
// tick table can ask which way the sun points without a second uniform.
@group(0) @binding(10) var<uniform> R : RenderParams;
@group(0) @binding(27) var<storage, read_write> openness    : array<u32>;
@group(0) @binding(28) var<storage, read_write> opennessGen : array<u32>;
// P1 (docs/PLAN_gi.md §3; common.wgsl IRRADIANCE GRID): the same walk that
// measures a face's openness now also keeps its irradiance word honest — zero
// where there is no surface, decayed where it cannot look, one coarse sun
// sample where it can.
@group(0) @binding(29) var<storage, read_write> irradiance  : array<u32>;

// ------------------------------------------------------------------ rays ----
// FIVE directions in the face's hemisphere: the normal, and four at 45 degrees
// toward the hemisphere's tangents. The normal is weighted DOUBLE (total weight
// 6), because a face whose straight-out view is blocked is in shade no matter
// how open its grazing directions are, and the opposite is not true.
//
// Five is a deliberate floor, not a placeholder. The values are quantised to a
// byte, read through a bilinear filter over 40 cm blocks, and multiplied into an
// ambient term — the eye reads the LOW-frequency field, and more rays per face
// would refine a signal the filter is about to smooth anyway. If this ever needs
// to be finer, spend it on more FACES or smaller BLOCKS, not more rays.
const OPEN_RAYS : u32 = 5u;
const OPEN_WEIGHT_TOTAL : f32 = 6.0;

// The face encoding, shared with the shadow cache: face = axis * 2 + (sgn > 0).
// Stated once here because the writer and the two readers (raymarch's
// `ambientAt` arm and common.wgsl's `opennessScaleAt`) must agree about it, and
// `shadowFaceNormal` in common.wgsl is the same function.
fn openFaceNormal(face : u32) -> vec3f {
  let axis = face >> 1u;
  let s = select(-1.0, 1.0, (face & 1u) != 0u);
  return vec3f(select(0.0, s, axis == 0u), select(0.0, s, axis == 1u),
               select(0.0, s, axis == 2u));
}

// "Does the 4^3 block containing this world cell hold any ray blocker?" One
// word read against the blockers class — the same bit traceOpaque's coarse mode
// tests, so this pre-test and the march can never disagree about a block.
fn openBlockBlocked(c : vec3<i32>) -> bool {
  let idx = chunkIndexW(c);
  let lo = vec3<u32>(c & vec3<i32>(i32(CHUNK) - 1));
  let sbit = subOccBitLocal(lo);
  return (occupancy[subOccIndex(idx, 1u, sbit >> 5u)] & (1u << (sbit & 31u))) != 0u;
}

// The slot's world CHUNK coord, and a cell inside it, from an origin the caller
// read ONCE. common.wgsl has worldCellOfSlotLocal for this, but it reaches the
// window origin through the per-shader GENERATED accessor ptOrigin() — which
// check_pass_table.py's rooted walk cannot see, so a row that (correctly)
// declares U(TickUBO) for it gets reported as a spurious barrier. Reading
// T.origin here makes the dependency visible to the checker AND hoists a
// uniform read out of 384 call sites per workgroup, so the honest version is
// also the faster one.
fn openWorldChunk(slot : u32, origin : vec3<i32>) -> vec3<i32> {
  let sc = vec3<i32>(i32(slot % NCHUNK), i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  return slotToWorldChunk(sc, origin);
}

// The openness of one (block, face), as a 0..255 byte.
//
// `blockMin` is the block's minimum world cell. The block is known to hold at
// least one blocker (the caller tested the bit), which is what makes this a
// SURFACE face worth measuring rather than a face of empty air.
fn openValueAt(blockMin : vec3<i32>, face : u32) -> u32 {
  let n = openFaceNormal(face);
  let ni = vec3<i32>(round(n));
  let axis = face >> 1u;
  // Tangents of the face, as unit axes. A 45-degree ray is normalize(n +- t).
  let t0 = vec3f(select(0.0, 1.0, axis == 1u), select(0.0, 1.0, axis == 2u),
                 select(0.0, 1.0, axis == 0u));
  let t1 = vec3f(select(0.0, 1.0, axis == 2u), select(0.0, 1.0, axis == 0u),
                 select(0.0, 1.0, axis == 1u));
  let t0i = vec3<i32>(round(t0));
  let t1i = vec3<i32>(round(t1));
  let half = f32(SUBOCC_BLOCK) * 0.5;
  let centre = vec3f(blockMin) + vec3f(half);

  // THE ORIGIN IS AN EXPOSED AIR CELL, FOUND BY LOOKING, and that replaced the
  // early-out that decided this file's first two versions. They started every
  // ray half a voxel outside the block's face PLANE and, if the neighbouring
  // block held any blocker at all, wrote "no opinion" -- because at 40 cm
  // blocks against 10 cm voxels the mask cannot tell a buried face from one
  // behind a one-voxel terrace step. That was right about the mask and wrong
  // about the answer: "no opinion" reached the reader as either full daylight
  // (v1) or a dropped tap (v2), and a dug tunnel -- every face of which has a
  // rough wall in the block in front of it -- came out as hard-edged
  // alternating slabs of full sky and pitch black (2026-09-02).
  //
  // So look at the voxels. Four columns of the face (the 2x2 sub-centres),
  // each walked inward from the cell just outside the block: the first
  // air-over-blocker transition is a surface somebody can see, and the air
  // cell of it is where the rays start. At most 4 x (SUBOCC_BLOCK + 1) word
  // reads, none for a block whose neighbour is open air (the old origin is
  // exact then). Only a face with NO exposed column in those four is
  // unmeasured (255), and that face is buried for every practical purpose.
  let outer = blockMin + select(vec3<i32>(0), abs(ni) * (i32(SUBOCC_BLOCK) - 1),
                                (face & 1u) != 0u);
  var ro = centre + n * (half + 0.5);
  if (openBlockBlocked(vec3<i32>(floor(ro)))) {
    var found = false;
    let q = i32(SUBOCC_BLOCK) / 4;
    for (var ui = 0; ui < 2 && !found; ui++) {
      for (var vi = 0; vi < 2 && !found; vi++) {
        let u = q + ui * (i32(SUBOCC_BLOCK) / 2);
        let v = q + vi * (i32(SUBOCC_BLOCK) / 2);
        let c0 = outer + t0i * u + t1i * v + ni;   // just outside the block
        var prevAir = !isRayBlocker(materials[voxMat(voxWordAt(c0))]);
        for (var k = 1; k <= i32(SUBOCC_BLOCK); k++) {
          let c = c0 - ni * k;
          let blocker = isRayBlocker(materials[voxMat(voxWordAt(c))]);
          if (blocker && prevAir) {
            ro = vec3f(c + ni) + vec3f(0.5);
            found = true;
            break;
          }
          prevAir = !blocker;
        }
      }
    }
    // 256, not 255, so the caller can tell "no opinion" from a face that
    // marched: P1's irradiance keeps a different word for each. The caller
    // clamps it into the byte.
    if (!found) { return 256u; }
  }

  let reachVox = TUNE_OPENNESS_REACH / VOXEL_METERS;
  // FINE FOR THE FIRST TWO BLOCKS, COARSE AFTER. A ray that started inside a
  // rough wall's own block would terminate on the block mask in its first
  // coarse step no matter where it was going; voxel steps for the first
  // 2 x SUBOCC_BLOCK cells let it thread the cavity it actually stands in.
  // The step budget covers those fine cells on a diagonal (sqrt 3 each) plus
  // the coarse remainder; the `t` test below is what makes reach exact.
  let fineT = f32(SUBOCC_BLOCK) * 2.0;
  let maxSteps = i32(fineT) * 2 + i32(reachVox / f32(SUBOCC_BLOCK)) + 8;

  // AN UNBLOCKED RAY IS NOT SKY. "Nothing within opennessReach" was the whole
  // test at first, and a cave chamber wider than 12 m read as full daylight:
  // every face of it sat at 255 while the faces near a pillar darkened, which
  // by eye was daylight-grey walls with soft dark splotches on them
  // (2026-09-02). So a ray that clears the reach asks one more question from
  // where it stopped: a straight-UP coarse march to the top of the window.
  // Blocked there means the endpoint is under something -- a roof, a hill, a
  // canopy -- and the ray did not find the sky. Under an overhang or a tree
  // the sideways rays' endpoints are past the edge and still count as open;
  // inside a hill every endpoint is under rock and nothing does. The up-ray
  // is bounded by the window height (WORLD_N / SUBOCC_BLOCK coarse steps).
  let upSteps = i32(WORLD_N / SUBOCC_BLOCK) + 4;
  let up = vec3f(0.0, 1.0, 0.0);
  var openW = 0.0;
  for (var r = 0u; r < OPEN_RAYS; r++) {
    var d = n;
    if (r == 1u) { d = normalize(n + t0); }
    else if (r == 2u) { d = normalize(n - t0); }
    else if (r == 3u) { d = normalize(n + t1); }
    else if (r == 4u) { d = normalize(n - t1); }
    // A VERTICAL FACE DOES NOT ASK THE GROUND. One of its four diagonals
    // points 45 degrees DOWN and hits the ground it stands on within a
    // metre, every time, on every step face of every hillside -- one sixth of
    // the weight gone before the sky was consulted, and with the two
    // horizontal rays stopped by any one-voxel rise inside reach the meadow's
    // risers measured near zero and went black at night. The ground half of
    // the hemisphere is the ambient's own ground-bounce term, not sky; so
    // that ray is re-aimed steeply upward (63 degrees) and the fan samples
    // the sky it exists to measure. Floors and ceilings keep the symmetric fan.
    if (axis != 1u && d.y < -0.3) { d = normalize(n + up * 2.0); }
    let s = traceOpaque(ro, d, maxSteps, fineT, &occupancy, &materials);
    var blocked = s.hit && (s.t * VOXEL_METERS) <= TUNE_OPENNESS_REACH;
    if (!blocked) {
      let e = ro + d * reachVox;
      let u = traceOpaque(e, up, upSteps, 0.0, &occupancy, &materials);
      blocked = u.hit;
    }
    let w = select(1.0, 2.0, r == 0u);
    if (!blocked) { openW += w; }
  }
  // 0..OPEN_MAX (254): 255 is the reader's "not measured" sentinel
  // (opennessByteAt, common.wgsl), never a measurement.
  return u32(clamp(openW / OPEN_WEIGHT_TOTAL, 0.0, 1.0) * OPEN_MAX + 0.5);
}

// ---- P1: the walk's coarse sun sample (docs/PLAN_gi.md §3) -----------------
// The off-screen half of direct injection. The shadow resolve pass deposits
// light only for patches somebody is looking at; a face nobody has seen since
// the sun moved would keep yesterday's light forever. So a face this walk
// MARCHES (its front block is air, so a ray can leave it) also gets ONE coarse
// sun ray from its centre and the first blocker voxel behind its plane, and
// blends that into its word at GI_WALK_ALPHA. Coarse in both senses — a block
// mask shadow and one voxel's albedo for a 16-voxel face — which is why the
// resolve pass's fine deposits outrank it wherever they exist (they land later
// in the same frame's command stream).
//
// FIVE COLUMNS, NOT ONE, and the same five openValueAt probes. A block is 4
// voxels wide and the surface inside it need not pass through its centre
// column: anywhere terrain rises or falls by a block within a 4x4 footprint --
// a slope, a cliff, a bank, a trunk, a ruin edge -- the block holds matter,
// the face MARCHES (openValueAt finds its origin with a 2x2 quincunx), and the
// centre column alone is empty air. Sampling only the centre found no albedo
// on exactly those faces and the caller then LEFT THE WORD ALONE, so a face
// the shadow resolve pass had charged with noon sunlight on some earlier frame
// kept that value forever: at dusk the whole grid faded except those, and they
// went on lighting their neighbours green (grass albedo) all night, scattered
// wherever the ground was steep. Returns w = 0 only when none of the five
// columns holds a blocker, and the caller FADES the word in that case -- see
// the decay branch in openChunk. "Could not measure it" must never mean "keep
// yesterday's sun".
fn openSunSample(blockMin : vec3<i32>, face : u32, open : f32) -> vec4f {
  let n = openFaceNormal(face);
  let L = keyLightDirP(R);
  let half = f32(SUBOCC_BLOCK) * 0.5;
  let centre = vec3f(blockMin) + vec3f(half);
  // The first blocker voxel under the face centre, stepping inward from the
  // face plane through at most the block's own depth. Its albedo AND its
  // emission (P3): a lava face deposits its glow here whether or not the sun
  // is up, which is why a face turned away from the sun still runs this.
  let ni = vec3<i32>(round(n));
  let axis = face >> 1u;
  let t0i = vec3<i32>(select(0, 1, axis == 1u), select(0, 1, axis == 2u),
                      select(0, 1, axis == 0u));
  let t1i = vec3<i32>(select(0, 1, axis == 2u), select(0, 1, axis == 0u),
                      select(0, 1, axis == 1u));
  let colBase = vec3<i32>(floor(centre + n * (half - 0.5)));
  var albedo = vec3f(0.0);
  var emis = 0.0;
  var found = false;
  // col 0 is the centre column -- a solid face hits on its first read, so the
  // four fallbacks cost nothing on the common case. col 1..4 are the 2x2
  // sub-centres, offset +-1 from the centre in each tangent.
  for (var col = 0u; col < 5u && !found; col++) {
    let k = col - 1u;
    let du = select(select(-1, 1, (k & 1u) != 0u), 0, col == 0u);
    let dv = select(select(-1, 1, (k & 2u) != 0u), 0, col == 0u);
    var c = colBase + t0i * du + t1i * dv;
    for (var i = 0u; i < SUBOCC_BLOCK; i++) {
      let w = voxWordAt(c);
      let m = materials[voxMat(w)];
      if (isRayBlocker(m)) {
        // Burning foliage deposits the MEAN of its breath (burnTintMean):
        // this grid is an EMA over frames and must not beat with the pulse.
        let bt = burnTint(m, paletteColor(m, voxState(w), &materials),
                          f32(m.emission) / 255.0, burnTintMean());
        albedo = bt.albedo;
        emis = bt.emis;
        found = true;
        break;
      }
      c -= ni;
    }
  }
  if (!found) { return vec4f(0.0); }
  // The sun ray, only for a face the sun can reach: the same origin the
  // openness rays use (half a voxel into the air block in front), coarse from
  // the first step, softened by distance to the blocker exactly as
  // shadow_resolve.wgsl softens its ray, so the two injection paths agree
  // about what "lit" means.
  var lit = 0.0;
  if (dot(n, L) > 0.0) {
    let ro = centre + n * (half + 0.5);
    let s = traceOpaque(ro, L, TUNE_SHADOW_STEPS, 0.0, &occupancy, &materials);
    lit = 1.0;
    if (s.hit) {
      let dM = s.t * VOXEL_METERS;
      lit = clamp(smoothstep(TUNE_SHADOW_SOFT_NEAR, TUNE_SHADOW_SOFT_FAR, dM) *
                  TUNE_SHADOW_LIFT, 0.0, 1.0);
    }
    // The lift cannot enter an enclosed space (shadowLiftCap, common.wgsl);
    // `open` is the openness this same visit just measured for the face.
    lit = shadowLiftCap(lit, open);
  }
  return vec4f(irrSample(albedo, n, L, keyLightColorP(R), lit, emis), 1.0);
}

// ---- the touch plane (common.wgsl OPEN_TOUCH_BASE) -------------------------
// Stamp T.tick on every slot column within reach of this slot's column: the
// refresh reads it to decide whether a chunk's faces could have changed since
// they were last marched. Reach in chunk columns, from the same knob the rays
// use, rounded up and capped at half the window (past that every column is
// within reach anyway). 17x17 plain stores per call at the default 12 m /
// 1.6 m chunks, spread over the workgroup: a few stores per thread, on the
// activity side of the ledger (rule 2), and a benign race -- every writer
// stores the same tick.
const OPEN_TOUCH_R : i32 =
    min(i32(ceil(TUNE_OPENNESS_REACH / VOXEL_METERS / f32(CHUNK))), i32(NCHUNK) / 2);
const OPEN_TOUCH_SPAN : u32 = u32(2 * OPEN_TOUCH_R + 1);
fn openTouchAround(slot : u32, li : u32) {
  let sx = i32(slot % NCHUNK);
  let sz = i32(slot / (NCHUNK * NCHUNK));
  let n = OPEN_TOUCH_SPAN * OPEN_TOUCH_SPAN;
  for (var i = li; i < n; i += OPEN_WORDS_PER_CHUNK) {
    let cx = (sx + i32(i % OPEN_TOUCH_SPAN) - OPEN_TOUCH_R) & NCHUNK_MASK;
    let cz = (sz + i32(i / OPEN_TOUCH_SPAN) - OPEN_TOUCH_R) & NCHUNK_MASK;
    opennessGen[OPEN_TOUCH_BASE + u32(cz) * NCHUNK + u32(cx)] = T.tick;
  }
}

// One chunk slot. `li` is both the thread index and the WORD index within the
// chunk's OPEN_WORDS_PER_CHUNK-word run.
//
// `fullIn` asks for the five-ray march on every surface face. false -- the
// refresh's SKIP visit -- keeps each face's stored byte and does only the
// irradiance maintenance: the coarse sun re-sample for a face that marched
// and the decay for one that could not. That half must never stop: a face lit
// at noon, off screen at dusk, is an emitter the gather reads, and "keep
// yesterday's sun" was the bug the decay exists for (the charger-with-no-
// expiry, 2026-09-03). A stale stamp forces a full walk whatever was asked.
fn openChunk(slot : u32, li : u32, origin : vec3<i32>, fullIn : bool) {
  let wc = openWorldChunk(slot, origin);
  let chunkMin = wc * i32(CHUNK);
  let stamp = opennessStamp(wc);
  // P1: every thread decides from the OLD stamp whether the irradiance words it
  // owns describe this chunk (blend into them) or a chunk that has since
  // streamed out of this slot (start from zero). Read by all before thread 0
  // stores the new one, with the barrier between, so no thread can see its own
  // workgroup's store.
  let stampOk = opennessGen[slot] == stamp;
  let full = fullIn || !stampOk;
  // The word this thread stored last time, for the skip visit. Read before the
  // barrier for the same reason as the stamp (nothing else writes it, but the
  // habit is the point).
  let prev = openness[slot * OPEN_WORDS_PER_CHUNK + li];
  workgroupBarrier();
  // The stamp, from thread 0. Written unconditionally, including for a chunk
  // with no matter in it at all: "computed, and the answer is open" is a
  // different statement from "never computed", and only the stamp carries it.
  // The walked tick beside it, only on a full walk: the refresh compares it
  // with the column's touch tick to decide whether the next visit may skip.
  if (li == 0u) {
    opennessGen[slot] = stamp;
    if (full) { opennessGen[OPEN_WALKED_BASE + slot] = T.tick; }
  }
  // A chunk that ARRIVED (stale stamp) is geometry its neighbours' faces were
  // never marched against -- the slot held something else when they were --
  // so it touches the columns around it exactly as a dirty walk does. Without
  // this a hill streaming in beside a walked floor would leave that floor
  // measured against the sky that used to be in the hill's slot.
  if (!stampOk) { openTouchAround(slot, li); }

  // A SOLID SENTINEL CHUNK (UNIFORM / JITTER of a ray blocker) is full to its
  // edges: every face of an interior block is buried, and the four-column
  // origin search would read twenty voxels per face to find that out -- 384
  // faces of it per chunk, on the many buried stone chunks the refresh walks
  // every tick. Only a face ON THE CHUNK BOUNDARY can be exposed (its front
  // cell is in the neighbour), so the rest are settled here without a read.
  // PT_EMPTY is not a special case: no block has a blocker, so the loop below
  // already takes its cheap branch.
  let pt = pageTable[slot];
  let solidSentinel = (pt & PT_SENTINEL_BIT) != 0u && pt != PT_EMPTY &&
                      isRayBlocker(materials[pt & PT_MAT_MASK]);

  var packed = 0u;
  for (var k = 0u; k < 4u; k++) {
    let byteIdx = li * 4u + k;
    let block = byteIdx / OPEN_FACES;
    let face = byteIdx % OPEN_FACES;
    let idx = irrIndex(slot, block, face);

    // No blocker anywhere in this block: there is no surface here, so no reader
    // will ever look at this byte (a reader indexes the block of a cell it just
    // HIT, which by construction has one). 255 rather than 0 so that if one
    // ever does -- a bilinear tap rolling off the edge of a wall -- it reads as
    // open air, which is what a block with nothing in it is.
    let bx = block % SUBOCC_DIM;
    let by = (block / SUBOCC_DIM) % SUBOCC_DIM;
    let bz = block / (SUBOCC_DIM * SUBOCC_DIM);
    let sw = occupancy[subOccIndex(slot, 1u, block >> 5u)];
    var v = 255u;
    if ((sw & (1u << (block & 31u))) != 0u) {
      let blockMin = chunkMin + vec3<i32>(i32(bx), i32(by), i32(bz)) *
                                    i32(SUBOCC_BLOCK);
      // 256 is "no opinion" (could not march); measured values are <= OPEN_MAX.
      var ov = 256u;
      // On the boundary of the chunk on this face's side?
      let axis = face >> 1u;
      let coord = select(select(bx, by, axis == 1u), bz, axis == 2u);
      let onEdge = select(coord == 0u, coord == SUBOCC_DIM - 1u, (face & 1u) != 0u);
      if (!(solidSentinel && !onEdge)) {
        if (full) {
          ov = openValueAt(blockMin, face);
        } else {
          // The skip visit: the byte stands. 255 in the byte is 256 here.
          let pb = (prev >> (k * 8u)) & 0xFFu;
          ov = select(pb, 256u, pb == 255u);
        }
      }
      // Measured values are already <= OPEN_MAX; 256 ("no opinion") lands on
      // 255, which the reader drops from its filter rather than reading as
      // open sky -- the splotch fix, see opennessByteAt.
      v = min(ov, 255u);
      // ---- P1: keep the face's irradiance word honest ----
      // Marched: blend in one coarse sun sample. Could not march (a blocker
      // in front -- a terrace riser, or buried rock): nothing here can measure
      // it, so its light fades at TUNE_GI_DECAY per visit, and the resolve
      // pass re-deposits it every frame while any patch of it is on screen.
      if (TUNE_GI_STRENGTH > 0.0) {
        var smp = vec4f(0.0);
        if (ov < 256u) {
          smp = openSunSample(blockMin, face, f32(ov) * (1.0 / OPEN_MAX));
        }
        if (smp.w > 0.0) {
          irrDeposit(idx, smp.xyz, GI_WALK_ALPHA, stampOk, &irradiance);
        } else {
          // NOTHING HERE CAN MEASURE THE FACE -- it could not march (a blocker
          // in front: a terrace riser, or buried rock), or it marched and none
          // of openSunSample's five columns holds a blocker. Either way the
          // walk has no answer, and the ONE thing it must not do is keep the
          // last one: the shadow resolve pass charges these words with full
          // sun whenever a patch of the face is on screen and never discharges
          // them once the camera looks away, so a word the walk skips is a
          // daylight value with no expiry. Fade at TUNE_GI_DECAY per visit
          // (~128 ticks apart) and let the resolve pass re-charge it every
          // frame it is actually visible. A word already at 0 (most buried
          // faces, forever) is left alone rather than rewritten as 0.
          let oldW = irradiance[idx];
          if (!stampOk) {
            irradiance[idx] = 0u;
          } else if (oldW != 0u) {
            irradiance[idx] = packRgb9e5(unpackRgb9e5(oldW) * (1.0 - TUNE_GI_DECAY));
          }
        }
        // The gather cache (common.wgsl GI_CACHE_BASE): a full walk means the
        // geometry around this face may have moved, so what its nine rays
        // saw is void. 0 = "never gathered"; the raymarch refills it on the
        // first frame it looks here. A skip visit leaves it be.
        if (full) { irradiance[GI_CACHE_BASE + idx] = 0u; }
      }
    } else if (TUNE_GI_STRENGTH > 0.0) {
      // No surface in the block: no light leaves it. Zero, so a gather ray
      // that lands here (a block that just lost its last blocker) reads dark
      // rather than the light of whatever stood here before. The cache word
      // with it -- nothing can receive here either.
      irradiance[idx] = 0u;
      irradiance[GI_CACHE_BASE + idx] = 0u;
    }
    packed |= v << (k * 8u);
  }
  openness[slot * OPEN_WORDS_PER_CHUNK + li] = packed;
}

// ---------------------------------------------------------------- passes ----

// dirty: the chunks written this tick, indirect over the compacted dirty list --
// the same list and the same args `occupancyDirty` rides, recorded immediately
// after it so the blockers mask this pass marches is the one that tick just
// rewrote. This is the half that makes an edit visible: dig a hole in a roof
// and the floor under it brightens on the next tick, with no invalidation
// machinery anywhere. Always a FULL walk, and it touches the columns within
// reach so the refresh re-marches the neighbours the edit could have changed
// (a roof stamped over a floor darkens that floor on the floor's next visit,
// instead of never).
@compute @workgroup_size(OPEN_WORDS_PER_CHUNK)
fn dirty(@builtin(workgroup_id) wg : vec3<u32>,
         @builtin(local_invocation_index) li : u32) {
  let slot = dirtyList[wg.x];
  openTouchAround(slot, li);
  openChunk(slot, li, T.origin, true);
}

// refresh: a flat TUNE_OPENNESS_CHUNKS slots per tick, round robin. The cursor
// is derived from T.tick rather than stored in a params word, which costs
// nothing and cannot go stale: tick * budget advances by exactly one budget per
// tick by construction, and a tick the pass was not recorded on simply skips
// its slice -- the next pass over that slot is at most kNumChunks / budget ticks
// away either way.
//
// The dirty walk above covers every EDIT, so this exists for the two cases it
// cannot see: the cold start (a slot whose stamp is still 0) and a chunk that
// streamed into a slot some other chunk's bytes are still sitting in. Neither
// is urgent -- a stale-stamped slot reads as "unknown" and shades from the old
// hemisphere lerp until this reaches it.
//
// FULL OR SKIP (PLAN_frame_perf.md s3 item 4). A slot whose column was touched
// since its last full walk marches again; one that was not keeps its bytes
// and does the irradiance maintenance only. Compared MODULO 2^32 -- "touched
// more recently than walked" as (now - touched) < (now - walked) -- so a tick
// clock that jumps backwards between harness fixtures still re-walks, and so
// a slice the pass was not recorded on (an idle tick, s3.4) cannot age a
// touch past a fixed window: the touch stands until a full walk follows it.
@compute @workgroup_size(OPEN_WORDS_PER_CHUNK)
fn refresh(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let cursor = (T.tick * u32(max(TUNE_OPENNESS_CHUNKS, 1))) % NUM_CHUNKS;
  let slot = (cursor + wg.x) % NUM_CHUNKS;
  let touched = opennessGen[OPEN_TOUCH_BASE + openColumnOfSlot(slot)];
  let walked = opennessGen[OPEN_WALKED_BASE + slot];
  let full = (T.tick - touched) < (T.tick - walked);
  openChunk(slot, li, T.origin, full);
}
