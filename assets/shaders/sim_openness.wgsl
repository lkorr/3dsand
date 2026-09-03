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
// in the same frame's command stream). Returns w = 0 when the column under the
// face centre holds no blocker at all (a block with one voxel in a corner):
// then the walk has no albedo to speak of and leaves the word alone rather than
// pulling a real deposit toward zero.
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
  var c = vec3<i32>(floor(centre + n * (half - 0.5)));
  var albedo = vec3f(0.0);
  var emis = 0.0;
  var found = false;
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

// One chunk slot. `li` is both the thread index and the WORD index within the
// chunk's OPEN_WORDS_PER_CHUNK-word run.
fn openChunk(slot : u32, li : u32, origin : vec3<i32>) {
  let wc = openWorldChunk(slot, origin);
  let chunkMin = wc * i32(CHUNK);
  let stamp = opennessStamp(wc);
  // P1: every thread decides from the OLD stamp whether the irradiance words it
  // owns describe this chunk (blend into them) or a chunk that has since
  // streamed out of this slot (start from zero). Read by all before thread 0
  // stores the new one, with the barrier between, so no thread can see its own
  // workgroup's store.
  let stampOk = opennessGen[slot] == stamp;
  workgroupBarrier();
  // The stamp, from thread 0. Written unconditionally, including for a chunk
  // with no matter in it at all: "computed, and the answer is open" is a
  // different statement from "never computed", and only the stamp carries it.
  if (li == 0u) { opennessGen[slot] = stamp; }

  var packed = 0u;
  for (var k = 0u; k < 4u; k++) {
    let byteIdx = li * 4u + k;
    let block = byteIdx / OPEN_FACES;
    let face = byteIdx % OPEN_FACES;
    let idx = irrIndex(slot, block, face);

    // No blocker anywhere in this block: there is no surface here, so no reader
    // will ever look at this byte (a reader indexes the block of a cell it just
    // HIT, which by construction has one). 255 rather than 0 so that if one
    // ever does — a bilinear tap rolling off the edge of a wall — it reads as
    // open air, which is what a block with nothing in it is.
    let bx = block % SUBOCC_DIM;
    let by = (block / SUBOCC_DIM) % SUBOCC_DIM;
    let bz = block / (SUBOCC_DIM * SUBOCC_DIM);
    let sw = occupancy[subOccIndex(slot, 1u, block >> 5u)];
    var v = 255u;
    if ((sw & (1u << (block & 31u))) != 0u) {
      let blockMin = chunkMin + vec3<i32>(i32(bx), i32(by), i32(bz)) *
                                    i32(SUBOCC_BLOCK);
      let ov = openValueAt(blockMin, face);
      // Measured values are already <= OPEN_MAX; 256 ("no opinion") lands on
      // 255, which the reader drops from its filter rather than reading as
      // open sky -- the splotch fix, see opennessByteAt.
      v = min(ov, 255u);
      // ---- P1: keep the face's irradiance word honest ----
      // Marched: blend in one coarse sun sample. Could not march (a blocker
      // in front — a terrace riser, or buried rock): nothing here can measure
      // it, so its light fades at TUNE_GI_DECAY per visit, and the resolve
      // pass re-deposits it every frame while any patch of it is on screen.
      if (TUNE_GI_STRENGTH > 0.0) {
        if (ov < 256u) {
          let smp = openSunSample(blockMin, face, f32(ov) * (1.0 / OPEN_MAX));
          if (smp.w > 0.0) {
            irrDeposit(idx, smp.xyz, GI_WALK_ALPHA, stampOk, &irradiance);
          }
        } else {
          let old = select(vec3f(0.0), unpackRgb9e5(irradiance[idx]), stampOk);
          irradiance[idx] = packRgb9e5(old * (1.0 - TUNE_GI_DECAY));
        }
      }
    } else if (TUNE_GI_STRENGTH > 0.0) {
      // No surface in the block: no light leaves it. Zero, so a gather ray
      // that lands here (a block that just lost its last blocker) reads dark
      // rather than the light of whatever stood here before.
      irradiance[idx] = 0u;
    }
    packed |= v << (k * 8u);
  }
  openness[slot * OPEN_WORDS_PER_CHUNK + li] = packed;
}

// ---------------------------------------------------------------- passes ----

// dirty: the chunks written this tick, indirect over the compacted dirty list —
// the same list and the same args `occupancyDirty` rides, recorded immediately
// after it so the blockers mask this pass marches is the one that tick just
// rewrote. This is the half that makes an edit visible: dig a hole in a roof
// and the floor under it brightens on the next tick, with no invalidation
// machinery anywhere.
@compute @workgroup_size(OPEN_WORDS_PER_CHUNK)
fn dirty(@builtin(workgroup_id) wg : vec3<u32>,
         @builtin(local_invocation_index) li : u32) {
  openChunk(dirtyList[wg.x], li, T.origin);
}

// refresh: a flat TUNE_OPENNESS_CHUNKS slots per tick, round robin. The cursor
// is derived from T.tick rather than stored in a params word, which costs
// nothing and cannot go stale: tick * budget advances by exactly one budget per
// tick by construction, and a tick the pass was not recorded on simply skips
// its slice — the next pass over that slot is at most kNumChunks / budget ticks
// away either way.
//
// The dirty walk above covers every EDIT, so this exists for the two cases it
// cannot see: the cold start (a slot whose stamp is still 0) and a chunk that
// streamed into a slot some other chunk's bytes are still sitting in. Neither
// is urgent — a stale-stamped slot reads as "unknown" and shades from the old
// hemisphere lerp until this reaches it.
@compute @workgroup_size(OPEN_WORDS_PER_CHUNK)
fn refresh(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let cursor = (T.tick * u32(max(TUNE_OPENNESS_CHUNKS, 1))) % NUM_CHUNKS;
  openChunk((cursor + wg.x) % NUM_CHUNKS, li, T.origin);
}
