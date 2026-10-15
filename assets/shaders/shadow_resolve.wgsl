// shadow_resolve.wgsl — the voxel-keyed shadow cache's resolve pass
// (src/sim/world.h, the kShadowCacheBuckets block; plan: shadow cache).
//
// WHAT THIS REPLACES. sunShadowAt used to cast one full trace() per lit pixel.
// At kVoxelMeters = 0.10 and 1080p/70deg a voxel face spans ~157/d pixels, so
// the same shadow answer was recomputed 40-1000x per frame. This pass computes
// it ONCE per surface patch, and the fragment shader only reads.
//
// WHY A SEPARATE PASS AND NOT AN IN-SHADER CACHE. Two reasons, and the second
// is the one that actually decided it:
//
//   1. A fragment shader cannot elect one pixel per patch to do the work, so
//      an in-shader cache degenerates into hit/miss on alternating frames.
//   2. --render-budget (RTX 3060 Ti, overlook cam, 1080p, 2026-09-01) says the
//      shadow call site costs 2.29 ms of TRAVERSAL and 3.59 ms of REGISTER
//      FOOTPRINT. Deduplicating rays while leaving an inline fallback in
//      raymarch.wgsl would chase the smaller half and forfeit the larger one.
//      The trace() call site has to be ABSENT from the compiled fragment
//      shader, which means the ray must be cast somewhere else. Here.
//
// THE RAY CAST HERE IS NOT trace(). trace() lives in raymarch.wgsl, carries 26
// Hit fields and is not reachable from a compute shader anyway. This pass casts
// traceOpaque() (common.wgsl) — a purpose-built MEDIA-BLIND DDA with no media
// accumulation, no water surface, no micro bricks and no reflection, which is
// also why it is cheaper per ray than trace(..., false) was: it carries none of
// those registers.
//
// IT USED TO LIVE HERE, as a private `shadowMarch` returning {hit, t}. W2-A
// (docs/PLAN_lin_followups.md) moved it into common.wgsl and widened the return
// to {hit, t, cell, axis, sgn, word}, because the same march answers every
// SECONDARY ray in the engine — this pass, sunShadowAt's cache-off fallback,
// traceReflection, traceRefraction and the god-ray occlusion test. TWO DDAs
// THAT MUST AGREE IS THE CLASSIC SILENT BUG and one function is the only
// structural cure; `--selftest --gate shadow-cache` still casts the
// compute-stage ray and the fragment-stage ray at sampled surface points and
// asserts they agree, which is now a statement about the two CALL SITES rather
// than about two copies of a DDA.
//
// ---- WHAT THIS ACTUALLY BOUGHT, AND THE COST NOBODY BUDGETED FOR ----------
// Measured with --render-budget (RTX 3060 Ti, overlook cam, 1080p, 2026-09-01),
// `nocache` against baseline in one process on one world:
//
//   nocache (the old inline per-pixel ray)   14.61 ms
//   baseline (this)                          12.84 ms      -1.77, ~12%
//
// Real, and it is the largest single renderer win in the file that declared
// "there is no renderer item left in this plan worth doing". But it is a third
// of what the arithmetic predicted, and the reason is worth more than the win:
//
//   * The dedup WORKS. The same frame reports 177,703 patches requested for
//     2,073,600 pixels — 8.57 per 100 px, 11.7x fewer rays — and 0 refused, so
//     the request cap is nowhere near binding.
//   * The rays got 15x MORE EXPENSIVE EACH. 1.35M rays cost 2.29 ms in the
//     fragment shader (1.7 ns each); 177k cost ~3.9 ms here (22 ns each).
//
// THE RAYS ARE INCOHERENT, AND THAT IS THIS PASS'S DOING. In the fragment
// shader adjacent pixels cast adjacent rays, so a wavefront marched one
// neighbourhood and every voxel fetch was a hit in a line some other lane had
// already pulled. Here the work list is in APPEND ORDER — pixels race to
// atomicAdd as they shade — so 64 consecutive threads march 64 unrelated parts
// of the world and share nothing. Cutting the ray count further does not help
// (subdiv 4 -> 1 is a large count reduction for 0.55 ms), which is the
// signature of a pass bound by memory locality rather than by work.
//
// So the next win here is not fewer rays, it is SORTED rays: bin the request
// list by a Morton code of the patch cell before dispatching, so a workgroup
// marches one neighbourhood again. That is a real piece of work with its own
// cost, and it is deliberately not smuggled in here — it is written down
// because the number that motivates it took one instrumented run to get and
// would take a dozen elimination runs to guess.
//
// ---- AND THEN THE NUMBER WENT AWAY (re-measured 2026-10-03) ---------------
// The sort was never built, because by the time anyone came to build it the
// 3.9 ms it was aimed at no longer existed. STAGGERED REFRESH (each valid
// patch re-cast once every SHADOW_REFRESH_PERIOD frames) and the nearest-patch
// read below SHADOW_NEAREST_PX (raymarch.wgsl) cut the request list from
// ~178k to ~90k on the overlook and the rays it holds to a quarter of that.
// Measured with --render-budget in one process, the `pre` span with this
// pass's ray zeroed (traceOpaque at 0 steps, no far continuation) against the
// shipped pass: noon 0.09 ms, seam 0.05, meadow 0.09 — the WHOLE ray cost of
// this pass, coherent or not. A Morton sort costs three dispatches and their
// barriers before it saves anything, and the most it could save is a fraction
// of 0.09 ms. Not worth a pass. If the request count ever climbs back toward
// the cap (a lower PERIOD, a finer subdiv), re-measure before building it.
// The shadow cost that IS left is the far cascade's (raymarch.wgsl
// farShadowMarch, in the fragment shader): 0.6 ms noon, ~1 ms cascade.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(5) var<storage, read_write> shadowCache : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> shadowReq : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> shadowArgs : array<u32>;
// P1 direct injection (docs/PLAN_gi.md §3; common.wgsl IRRADIANCE GRID): every
// patch this pass publishes also deposits its lit radiance into its block-face.
// `opennessGen` is the per-slot stamp shared with the openness grid, read so
// a slot the window has reused starts its blend from zero.
@group(0) @binding(8) var<storage, read_write> irradiance  : array<u32>;
@group(0) @binding(9) var<storage, read>       opennessGen : array<u32>;
// The openness byte grid itself, read so the P1 deposit can cap the shadow
// LIFT by sky visibility (shadowLiftCap, common.wgsl): without it a cave
// floor under a tall roof deposited 45% sun into the grid every frame and the
// gather lit the cave with it. The published cache value is NOT capped here;
// the fragment shader caps at its read, so the cache stays the pure ray
// answer --gate shadow-cache compares against the fragment-stage ray.
@group(0) @binding(10) var<storage, read>      openness    : array<u32>;
// The penumbra window (src/sim/world.h kShadowHistBytes) — one word per bucket,
// read-modify-written below. Bound here and NOWHERE else: the fragment shader
// reads the published byte out of shadowCache and never this.
@group(0) @binding(11) var<storage, read_write> shadowHist : array<u32>;
// (12..17 are the clouds', cloud.wgsl — same layout, different kernel.)
// The far cascade, read-only, for CASTERS OUTSIDE THE WINDOW (below).
@group(0) @binding(18) var<storage, read> farVox : array<u32>;
@group(0) @binding(19) var<storage, read> farOcc : array<u32>;
@group(0) @binding(20) var<uniform> F : FarParams;

// ===================== CASTERS OUTSIDE THE WINDOW ===========================
//
// WHAT THIS FIXES (LOD-seam package C, 2026-09-28). traceOpaque clips to the
// residency window, so a patch's ray that leaves the window unblocked read as
// SUN — and every caster outside the window (the tree two metres past the
// face, the ridge the low sun sits behind) shadowed nothing inside it. On the
// sun-facing side the ground along the face was lit where the far field one
// cell further out was in shadow, and a long evening tree shadow stopped dead
// at the face. Both are the seam the eye picks out while walking, because the
// face is fixed in the world and jumps a chunk at a time.
//
// So the ray does not stop at the face: it continues through the far cascade
// from the exit point, with the SAME cone schedule and the SAME reach the far
// field's own shadow uses (raymarch.wgsl farShadowMarch, whose header has the
// why). Both the schedule and the reach are measured from the patch, so a
// patch one voxel inside the face and a far cell one cell outside it see the
// same casters out to the same distance. It runs per jittered sample, so the
// 16-frame window below turns a far caster's edge into the same penumbra a
// near one gets.
//
// THIS IS A SECOND COPY OF A DDA, which this file's own header calls the
// classic silent bug. It is one because the far march lives in raymarch.wgsl
// against bindings common.wgsl cannot name, and moving it into common.wgsl
// costs every shader in the engine a recompile (CLAUDE.md). What holds it
// honest: raymarch.wgsl's cache-off sunShadowAt runs the continuation through
// farShadowMarch itself, and --gate shadow-cache compares that frame against
// this pass's. The copy below drops only the render-stats counters; the
// penumbra estimate is there for the FAR PATCHES (below) and skipped for these
// rays (withCov false: the cone jitter does that job here). Since 2026-10-03
// it also clips to the per-level SKY BOUND as farShadowMarch does — the copy
// had been missing it; exact (above that row there is only air), a rising
// ray simply stops walking empty chunks sooner.
//
// COST lands only on rays that actually leave the window within
// TUNE_FAR_SHADOW_REACH of their patch: a ray that exits through the top of
// the window 25 m up is past the reach before it starts and pays one compare.
const FAR_SHADOW_SEG_CELLS : f32 = 32.0;    // raymarch.wgsl, same name
const FAR_SHADOW_STEP_CAP : i32 = 160;      // raymarch.wgsl, same name
const FAR_FACE_BITS : u32 = 5u;             // world.h kFarFaceBits (check_invariants.py)
const FAR_FACE_MASK : u32 = 31u;            // world.h kFarFaceMax
const FAR_FACE_ALL  : u32 = 1u << 30u;      // world.h kFarFaceAllPending
struct FarBox { lo : vec3<i32>, hi : vec3<i32> };
// raymarch.wgsl farBox, verbatim: the VALID box of a level (the pending faces
// the sieve has not refilled yet are cut off).
fn farBox(level : u32) -> FarBox {
  let o = F.origins[level - 1u];
  let w = u32(o.w);
  var b : FarBox;
  b.lo = o.xyz * i32(CHUNK);
  if ((w & FAR_FACE_ALL) != 0u) { b.hi = b.lo; return b; }
  let lo = vec3<i32>(i32((w >> (0u * FAR_FACE_BITS)) & FAR_FACE_MASK),
                     i32((w >> (2u * FAR_FACE_BITS)) & FAR_FACE_MASK),
                     i32((w >> (4u * FAR_FACE_BITS)) & FAR_FACE_MASK));
  let hi = vec3<i32>(i32((w >> (1u * FAR_FACE_BITS)) & FAR_FACE_MASK),
                     i32((w >> (3u * FAR_FACE_BITS)) & FAR_FACE_MASK),
                     i32((w >> (5u * FAR_FACE_BITS)) & FAR_FACE_MASK));
  b.lo = (o.xyz + lo) * i32(CHUNK);
  b.hi = (o.xyz + vec3<i32>(i32(FAR_NCHUNK)) - hi) * i32(CHUNK);
  return b;
}
fn farInValid(c : vec3<i32>, b : FarBox) -> bool {
  return all(c >= b.lo) && all(c < b.hi);
}
// ---- A CLEAR RAY UNDER THE TERRAIN IS NOT SUNLIT (2026-10-13) --------------
// A shadow ray that ran farShadowReach without meeting a cell used to be SUN.
// Outdoors that is the reach doing its job (a blocker past 24 m is a ridge the
// horizon's hzSunShade answers). Underground it is a hole in the rock: in a
// chamber taller than the reach along the sun -- the Maw of teeth_caves, 30 m
// of vault -- or one whose ray leaves the cascade's valid box, every patch the
// ray could not close read full sun, and the cap downstream (shadowLiftCap)
// only knows a face's own sky view, which a far patch does not carry. So the
// point the ray stopped at is asked whether it is under the terrain's top
// (the far SURFACE MAP, binding 34; raymarch.wgsl farColumnTop, the burial
// test's reader): under = blocked at the far blocker's distance, i.e. the
// reach. Above the ground, or a column no level vouches for, keeps the old
// answer -- every outdoor ray is unchanged.
@group(0) @binding(34) var<storage, read> farMap : array<vec4<u32>>;
fn farColumnTop(level : u32, p : vec3f) -> f32 {
  let w = f32(1u << (farCellShift(level) - 1u));
  let m = vec2<i32>(floor(vec2f(p.x, p.z) / w));
  let b = farBox(level);
  let c = m >> vec2<u32>(1u);
  if (any(c < b.lo.xz) || any(c >= b.hi.xz)) { return -1e9; }
  let q = farMap[farMapCell(level, c)];
  let hx = (m.x & 1) != 0;
  let e = select(select(q.x, q.y, hx), select(q.z, q.w, hx), (m.y & 1) != 0);
  return f32(farMapTop(e) + 1);
}
// Is the end of a clear ray of length `t` (fine voxels) under the terrain? The
// level that holds that point, finest first: level 1 covers +-51 m round the
// eye, each next level twice that.
fn farUnderTerrain(ro : vec3f, rd : vec3f, t : f32) -> bool {
  let p = ro + rd * t;
  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let top = farColumnTop(level, p);
    if (top > -1e8) { return p.y < top; }
  }
  return false;
}

// raymarch.wgsl farShadowBlocked: MATERIAL cells only, never the blocker flag,
// and only a ray-blocking material (media-blind: clear water is no caster).
fn farShadowBlocked(level : u32, c : vec3<i32>) -> bool {
  let bi = farVoxByteIndex(level, c);
  let slot = farCellSlot((farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu);
  return slot != 0u && isRayBlocker(materials[farPalMat(&materials, slot)]);
}
// raymarch.wgsl shadowWindowExitT.
fn shadowWindowExitT(ro : vec3f, rd : vec3f) -> f32 {
  let inv = 1.0 / rd;
  let lo = vec3f(R.origin * i32(CHUNK));
  let t0 = (lo - ro) * inv;
  let t1 = (lo + vec3f(f32(WORLD_N)) - ro) * inv;
  let tmax = max(t0, t1);
  return max(min(tmax.x, min(tmax.y, tmax.z)), 0.0);
}
// raymarch.wgsl farShadowCover, verbatim (the far patches below publish the
// fragment shader's own penumbra estimate; see FAR PATCHES).
const FAR_SHADOW_CONE_TAN : f32 = tan(TUNE_SHADOW_SUN_ANGLE * 0.017453292);
fn farShadowCover(level : u32, vc : vec3<i32>, eAx : i32, tIn : f32,
                  vMax : vec3f, rd : vec3f, stepv : vec3<i32>, s : f32) -> f32 {
  if (FAR_SHADOW_CONE_TAN <= 0.0 || eAx < 0) { return 1.0; }
  var xAx = 2;
  var tOut = vMax.z;
  if (vMax.x < vMax.y && vMax.x < vMax.z) { xAx = 0; tOut = vMax.x; }
  else if (vMax.y < vMax.z) { xAx = 1; tOut = vMax.y; }
  if (xAx == eAx) { return 1.0; }
  let nb = vc + vec3<i32>(select(0, stepv.x, xAx == 0),
                          select(0, stepv.y, xAx == 1),
                          select(0, stepv.z, xAx == 2));
  if (farInValid(nb, farBox(level)) && farShadowBlocked(level, nb)) { return 1.0; }
  let a = abs(rd);
  let ra = select(select(a.x, a.y, eAx == 1), a.z, eAx == 2);
  let rx = select(select(a.x, a.y, xAx == 1), a.z, xAx == 2);
  let margin = (tOut - tIn) * ra * rx * inverseSqrt(max(ra * ra + rx * rx, 1e-8)) * s;
  let w = 2.0 * tIn * s * FAR_SHADOW_CONE_TAN;
  return smoothstep(0.0, max(w, 1e-3), margin);
}
// raymarch.wgsl farSkyCeil (sky_top.wgsl's words at farOcc's tail).
// raymarch.wgsl PARTIAL COVERS ACCUMULATE: MUST AGREE.
const FAR_SHADOW_COVER_HITS : i32 = 4;
const FAR_SHADOW_OPEN_MIN : f32 = 0.05;
const SKY_TOP_BASE : u32 = FAR_LEVELS * FAR_NUM_CHUNKS;   // raymarch.wgsl agrees
const SKY_TOP_BIAS : i32 = 1 << 24;                       // raymarch.wgsl agrees
fn farSkyCeil(level : u32) -> f32 {
  let w = farOcc[SKY_TOP_BASE + level - 1u];
  return select(f32(i32(w) - SKY_TOP_BIAS), -1e30, w == 0u);
}

// raymarch.wgsl farShadowMarch: the blocker distance in FINE voxels from
// `roFine` (x, or -1) and, when `withCov`, the sun-disc cover farShadowCover
// estimates for it (y). The near patches' continuation passes false: their
// cone jitter does the penumbra's job.
fn farShadowT(level0 : u32, roFine : vec3f, rdIn : vec3f, tStartFine : f32) -> f32 {
  return farShadowTC(level0, roFine, rdIn, tStartFine, false).x;
}
fn farShadowTC(level0 : u32, roFine : vec3f, rdIn : vec3f, tStartFine : f32,
               withCov : bool) -> vec2f {
  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  let reachF = TUNE_FAR_SHADOW_REACH / VOXEL_METERS;
  var tF = tStartFine;
  var budget = FAR_SHADOW_STEP_CAP;
  var open = 1.0;
  var hits = 0;
  var tFirst = -1.0;
  for (var level = level0; level <= FAR_LEVELS; level++) {
    if (tF >= reachF || budget <= 0) { break; }
    let s = f32(1u << farCellShift(level));
    let tEndF = select(min(reachF, FAR_SHADOW_SEG_CELLS * s), reachF,
                       level == FAR_LEVELS);
    if (tEndF <= tF) { continue; }
    let roL = roFine / s;
    let box = farBox(level);
    let tt0 = (vec3f(box.lo) - roL) * inv;
    let tt1 = (vec3f(box.hi) - roL) * inv;
    var tExit = min(min(max(tt0.x, tt1.x), min(max(tt0.y, tt1.y), max(tt0.z, tt1.z))),
                    tEndF / s);
    // raymarch.wgsl farShadowMarch, THE SKY BOUND (exact: above the level's
    // highest occupied cell there is only air).
    if (rd.y > 0.0) {
      tExit = min(tExit, (farSkyCeil(level) - roL.y) * inv.y + 1e-3);
    }
    var tCur = tF / s;
    var cc = worldChunkOf(vec3<i32>(floor(roL + rd * tCur)));
    var cNext : vec3f;
    for (var a = 0; a < 3; a++) {
      let b = f32((cc[a] + select(0, 1, rd[a] > 0.0)) * i32(CHUNK));
      cNext[a] = (b - roL[a]) * inv[a];
    }
    // The chunk-crossing axis, for the penumbra estimate (farShadowMarch's cAx).
    var cAx = -1;
    while (budget > 0) {
      budget -= 1;
      if (!farInValid(cc * i32(CHUNK), box)) { break; }
      if (tCur >= tExit) { break; }
      let tOut = min(cNext.x, min(cNext.y, cNext.z));
      let occ = farOcc[farOccIndex(level, cc * i32(CHUNK))];
      if (occ != 0u) {
        var tIn = tCur;
        var eAx = cAx;
        var walk = true;
        let top = farOccTop(occ);
        if (top != 0u) {
          let yTop = cc.y * i32(CHUNK) + i32(top);
          let yEnter = roL.y + rd.y * (tIn + 1e-4);
          if (yEnter >= f32(yTop)) {
            if (rd.y >= 0.0) {
              walk = false;
            } else {
              let tPlane = max((f32(yTop) - roL.y) * inv.y, tIn);
              if (tPlane + 1e-4 >= tOut) { walk = false; }
              else { tIn = tPlane + 1e-4; eAx = 1; }
            }
          }
        }
        if (walk) {
          let cLo = cc * i32(CHUNK);
          var vCur = tIn;
          var vc = clamp(vec3<i32>(floor(roL + rd * (tIn + 1e-4))),
                         cLo, cLo + vec3<i32>(i32(CHUNK) - 1));
          var vMax : vec3f;
          for (var a = 0; a < 3; a++) {
            let boundary = f32(vc[a]) + select(0.0, 1.0, rd[a] > 0.0);
            vMax[a] = (boundary - roL[a]) * inv[a];
          }
          // raymarch.wgsl farShadowMarch, THE ROW SKIP INSIDE THE CHUNK: a
          // rising ray past the chunk's top row can meet nothing more in it.
          let yTopRow = select(i32(0x7FFFFFFF), cLo.y + i32(top), top != 0u);
          for (var j = 0; j < 3 * i32(CHUNK); j++) {
            if (budget <= 0) { break; }
            budget -= 1;
            if (farShadowBlocked(level, vc)) {
              if (!withCov) { return vec2f(vCur * s, 1.0); }
              // raymarch.wgsl farShadowMarch, PARTIAL COVERS ACCUMULATE.
              let cv = farShadowCover(level, vc, eAx, vCur, vMax, rd, stepv, s);
              if (hits == 0) { tFirst = vCur * s; }
              open *= 1.0 - cv;
              hits += 1;
              if (open < FAR_SHADOW_OPEN_MIN || hits >= FAR_SHADOW_COVER_HITS) {
                return vec2f(tFirst, 1.0 - open);
              }
            }
            if (vMax.x < vMax.y && vMax.x < vMax.z) {
              vc.x += stepv.x; vCur = vMax.x; vMax.x += tDelta.x; eAx = 0;
            } else if (vMax.y < vMax.z) {
              vc.y += stepv.y; vCur = vMax.y; vMax.y += tDelta.y; eAx = 1;
            } else {
              vc.z += stepv.z; vCur = vMax.z; vMax.z += tDelta.z; eAx = 2;
            }
            // raymarch.wgsl traceFar, THE CELL CURSOR STAYS IN THE VALID BOX.
            if (vCur >= tOut || vCur >= tExit || !farInValid(vc, box)) { break; }
            if (stepv.y > 0 && vc.y >= yTopRow) { break; }
          }
        }
      }
      if (cNext.x <= cNext.y && cNext.x <= cNext.z) {
        tCur = cNext.x; cc.x += stepv.x; cAx = 0;
        cNext.x = (f32((cc.x + max(stepv.x, 0)) * i32(CHUNK)) - roL.x) * inv.x;
      } else if (cNext.y <= cNext.z) {
        tCur = cNext.y; cc.y += stepv.y; cAx = 1;
        cNext.y = (f32((cc.y + max(stepv.y, 0)) * i32(CHUNK)) - roL.y) * inv.y;
      } else {
        tCur = cNext.z; cc.z += stepv.z; cAx = 2;
        cNext.z = (f32((cc.z + max(stepv.z, 0)) * i32(CHUNK)) - roL.z) * inv.z;
      }
    }
    tF = max(tF, min(tCur, tExit) * s);
  }
  return select(vec2f(-1.0, 0.0), vec2f(tFirst, 1.0 - open), hits > 0);
}

// ======================= THE SUN IS NOT A POINT ============================
//
// WHAT THIS FIXES. One ray per patch answers a yes/no question, so the cache
// could only publish "lit" or "shadowed at this blocker's lift" — a hard step
// one patch wide however far away the blocker was. Walking the sun across the
// sky then advanced that step one patch at a time, which is what a shadow
// expanding in visible jumps IS (owner report 2026-09-11), and no amount of
// subdivision fixes it: a finer grid makes the step smaller, not softer.
//
// THE RAY IS JITTERED INSIDE THE SOLAR CONE and the last SHADOW_SAMPLES
// verdicts are averaged, so a patch publishes the FRACTION of the disc it can
// see. That is a real penumbra: it costs ZERO extra rays, and its width falls
// out of the geometry — a blocker `d` away spreads the cone over 2*d*tan(angle),
// so a kerb stays crisp and a canopy 10 m up softens over most of a metre.
//
// A WINDOW, NOT AN EXPONENTIAL BLEND. An EMA over a cycling sample set never
// settles: it oscillates with the sequence's period forever, which is a
// shadow that pulses. A sliding window over a FIXED sequence is exact after
// SHADOW_SAMPLES frames and then stops moving entirely while the scene and the
// sun hold still, because the same slot is rewritten with the same bit.
// --gate shadow-cache's flicker arm asserts exactly that, which is why its
// warm-up is longer than the window.
//
// THE PATTERN IS ROTATED PER PATCH, so the 1/16 quantisation of the estimate
// dithers across neighbouring patches instead of terracing into 17 visible
// bands. That trades a contour for sub-voxel grain, and the grain is the better
// artifact here: it is STATIC (the rotation is a hash of the patch, not of the
// frame), it is finer than a voxel at subdiv 4, and the reader averages four
// patches bilinearly on top of it.
const SHADOW_SAMPLES : u32 = 16u;   // world.h kShadowSamples
// raymarch.wgsl STAGGERED REFRESH — MUST AGREE with the copy there.
const SHADOW_REFRESH_PERIOD : u32 = 4u;
fn shadowRefreshPhase(key : u32) -> u32 {
  return ((key * 0x9E3779B1u) >> 16u) % SHADOW_REFRESH_PERIOD;
}
const SHADOW_HIST_MASK : u32 = 0xFFFFu;
const SHADOW_FILL_SHIFT : u32 = 16u;
const SHADOW_FILL_MASK : u32 = 31u;
const SHADOW_LIFT_SHIFT : u32 = 21u;
// tan of the cone half-angle. A const-eval of a tuning float, exactly like the
// fluid/wind rows: the kernel that reads it is the only consumer.
const SHADOW_CONE_TAN : f32 = tan(TUNE_SHADOW_SUN_ANGLE * 0.017453292);

// ============ THE WINDOW HAS SEVENTEEN LEVELS; THE SUN HAS NONE =============
//
// WHAT THIS FIXES (owner report 2026-09-12, the follow-up to the one above).
// The cone made the shadow a gradient in SPACE. It is still a staircase in
// TIME, because SHADOW_SAMPLES binary verdicts can only estimate the disc's
// coverage at 1/16, and a patch therefore holds a value for a while and then
// jumps 6.25% of full contrast when the blocker's edge crosses one of the
// sixteen sample directions. Do the arithmetic at the tuning this ships with:
// dayNight.cycleMinutes is 6, so the sun crosses a degree of sky per SECOND
// and sweeps the whole 2 deg cone in two — sixteen levels in two seconds is a
// visible step every 7 or 8 frames, for as long as the shadow is moving. That
// is "the pixels still discretely jump in a bunch of small steps".
//
// MORE SAMPLES IS NOT THE ANSWER and there is no room for them anyway: the
// window, its fill count and the lift byte already use 29 of a word's 32 bits,
// and doubling the sample count halves the step while doubling the warm-up.
// Sixty-four levels would still step.
//
// SO THE PUBLISHED BYTE GLIDES. The window output is low-passed on its way
// into the slot, which turns each 1/16 jump into a ramp a few frames long; the
// ramps of a moving shadow run into each other and what comes out is a
// continuous slide at 1/255, which is the byte's own resolution and the end of
// the road.
//
// THIS IS NOT THE EMA THE BLOCK ABOVE REFUSES, and the distinction is the
// whole reason it is safe. That one would have averaged the RAW SAMPLES — a
// cycling sequence, so its output oscillates with the sequence's period
// forever and never settles. This one averages the WINDOW'S OUTPUT, which in a
// static scene is not a sequence at all: it is one number, bit-identical every
// frame, because the same slot is rewritten with the same bit. A low-pass fed
// a constant converges to that constant and stops. The flicker arm of
// --gate shadow-cache is what holds that claim honest.
//
// EXACT IN INTEGERS, for the same reason. The glide is computed on the stored
// BYTE, not on a float that is re-quantised at the write: a float blend that
// lands 0.4 units short of its target rounds back and forth between two
// adjacent bytes forever, which is a 1/255 flicker — small, but "0 pixels
// moved between two warmed frames" is a claim about zero. Integer steps with a
// minimum magnitude of one unit reach the target exactly and then produce a
// delta of exactly zero.
//
// A SNAP THRESHOLD keeps it from costing responsiveness. The sun creeps; a
// blocker that appears or vanishes (a block mined, a door opened) moves the
// answer by a large fraction at once, and there is no reason to smear that
// over a third of a second. Past SHADOW_GLIDE_SNAP the new answer is taken
// whole. The sun's own 1/16 steps are nowhere near it, and neither is the
// lift byte's 16-frame refresh, so both glide.
//
// THE LAG IS BOUNDED AND SMALL. Below |d*rate| = 1 the step floors at one unit
// per frame, so a steadily-moving shadow settles into a constant-velocity
// slide with a standing error of about 1/rate units — ~17 of 255 at the rate
// below, which is one window step, which is a couple of centimetres of shadow
// position. A constant-velocity slide is the smoothest thing this can be.
//
// OFF AT A ZERO CONE ANGLE, so `render.shadowSunAngle = 0` still reproduces
// the pre-2026-09-11 hard shadow exactly, value for value, and stays the
// differential oracle it was.
const SHADOW_GLIDE : f32 = 0.06;        // per frame, on the 0..255 byte
const SHADOW_GLIDE_SNAP : i32 = 64;     // >= this many units: take it whole

// A stable orthonormal pair spanning the plane normal to `n`. Frisvad's branch
// is avoided in favour of picking the axis `n` leans on least, which costs two
// compares and cannot degenerate.
fn shadowConeBasis(n : vec3f) -> mat2x3<f32> {
  let a = abs(n);
  var up = vec3f(0.0, 0.0, 1.0);
  if (a.z >= a.x && a.z >= a.y) { up = vec3f(1.0, 0.0, 0.0); }
  let t = normalize(cross(up, n));
  return mat2x3<f32>(t, cross(n, t));
}

// Sample `i` of a SHADOW_SAMPLES-point sunflower disc, rotated by `rot` turns.
// Equal-area radii (sqrt of the stratum) and the golden angle between points:
// every prefix of the sequence is already well spread, which matters because
// a patch's first frames publish a partial window.
fn shadowConeDir(L : vec3f, i : u32, rot : f32) -> vec3f {
  if (SHADOW_CONE_TAN <= 0.0) { return L; }
  let b = shadowConeBasis(L);
  let r = sqrt((f32(i) + 0.5) / f32(SHADOW_SAMPLES));
  let a = f32(i) * 2.39996323 + rot * 6.28318531;
  let off = (b[0] * cos(a) + b[1] * sin(a)) * (r * SHADOW_CONE_TAN);
  return normalize(L + off);
}

// --------------------------------------------------------------- passes ----

// prepare: turn last frame's request COUNT into a dispatch size, and reopen the
// list for this frame's fragment shader to append into.
//
// The count is moved to a SAVED slot rather than read in place, because the
// same word has to go back to zero before the render pass runs. Ordering is
// safe by construction: this pass, then resolve, then the draw — all in one
// command buffer with the pass table's barriers between them.
@compute @workgroup_size(1)
fn prepare() {
  let raw = atomicLoad(&shadowReq[0]);
  let n = min(raw, SHADOW_REQ_CAP);
  atomicStore(&shadowReq[1], n);
  // Overflow is graceful and counted, not fatal: past the cap a patch is simply
  // not registered, so it misses next frame and shades from the miss default.
  // The failure is one slightly wrong patch, not a lost voxel — the opposite of
  // the page pool, where exhaustion aborts.
  atomicStore(&shadowReq[2], raw);
  atomicStore(&shadowReq[3], select(0u, raw - SHADOW_REQ_CAP, raw > SHADOW_REQ_CAP));
  atomicStore(&shadowReq[0], 0u);
  shadowArgs[0] = (n + RESOLVE_WG - 1u) / RESOLVE_WG;
  shadowArgs[1] = 1u;
  shadowArgs[2] = 1u;
}

// ======================= FAR PATCHES (2026-10-03, raymarch-far) =============
// raymarch.wgsl FAR_SHADOW_CACHE has the why and the measurement; ON since
// 2026-10-06 (with it off, no far request is ever made and this never runs). A
// request whose packedSub carries FAR_SC_FLAG is a CASCADE patch: (level, cell
// toroidal in its level, face, sub-patch, refined). Its value is the fragment
// shader's own far-pixel formula evaluated at the patch centre — `hp` below is
// fs()'s `hp` for a pixel there (face + half a cell along the normal, lifted
// over the cell top for a refined hit), the march is farShadowMarch's
// (farShadowTC with the penumbra estimate), the law is shadowFromOpaqueHit's —
// published whole: no cone jitter, no window, no glide, no GI deposit (a far
// cell is not a window voxel). A BURIED start (the march stops in its first
// cell) is "no opinion", as for the near patches. MUST AGREE with
// raymarch.wgsl: FAR_SC_* and farScPackCell.
const FAR_SC_SUBDIV : u32 = 2u;
const FAR_SC_FLAG : u32 = 0x80000000u;
const FAR_SC_REFINED : u32 = 0x08000000u;
const FAR_SC_LEVEL_SHIFT : u32 = 28u;
const FAR_SC_AXIS_BITS : u32 = countTrailingZeros(FAR_N);

// ======================= THE FAR PATCH'S INDIRECT LIGHT (2026-10-13) ========
// WHAT THIS FIXES (ticket 2026-10-12_20-13-42, teeth_caves). The near field's
// indirect light is MEASURED: gi_gather.wgsl casts nine rays from every visible
// block-face and reads what each one lands on (an emitter's glow, a sunlit
// floor through a cave mouth, multi-bounce). The cascade's was a CLOSED FORM
// for open terrain (raymarch.wgsl farGiBounce: a lit tread in front of the
// face), which under the ground is daylight that is not there -- so the far
// field scales it out by burial, and a buried cascade hit then had NO
// indirect light at all. In a cave lit by glowcaps and crystals that drew a
// sphere at the 20.5 m handoff inside which the walls round each emitter glow
// and outside which they are black, and it moved with the camera.
//
// So a BURIED far patch measures it, in this pass, the way the near gather
// does: the same nine directions and weights (the normal 0.25, eight ring rays
// 0.09375) from the patch centre, each marched through the patch's own level
// to the first MATERIAL cell within FAR_GI_REACH_M, which sends its emission
// (irrSample's emissive term, the law every near deposit uses) and -- if it is
// NOT buried itself, i.e. ground under the open sky seen through a mouth or a
// shaft -- the sun's hard-Lambert light on the face the ray entered, unshadowed
// (the closed forms' own assumption for open ground). COVERAGE as the near
// gather credits it (giFacePlane): the hit cell's share of the cone falls off
// as 1/t^2, the rest of the cone takes what the cells beside it in the face's
// plane send -- their mean here, a cascade cell being 20-80 cm already. No
// multi-bounce (the near field's write-back has no far twin).
//
// STORED in shadowHist[bucket]: a far patch has no penumbra window (that is
// the near patches' word), so its bucket's word is free. RGB9E5 with the low
// bit forced on (0 = not buried / nothing measured: the reader keeps the
// closed form). Every buried patch that is REQUESTED gathers, and the reader
// decides how many are: raymarch.wgsl farGiCached reads one patch per cell
// face (sub-patch (0, 0)) for a fully buried hit and its own sub-patch only in
// the burial band, so a cave's walls cost one gather per cell face.
// Re-measured every FAR_GI_PERIOD resolves of the
// patch (every 64 frames, ~1 s: the sun's share moves with the day, a degree
// a second, and nothing else it sees moves at all) and on its first; written on EVERY resolve so a
// word left by the bucket's previous owner never reaches a reader (the reader
// only reads a slot this pass has published, and this pass publishes after
// it writes).
const FAR_GI_REACH_M : f32 = 12.0;
const FAR_GI_STEPS : i32 = 72;
const FAR_GI_PERIOD : u32 = 16u;
const FAR_GI_W_NORMAL : f32 = 0.25;
const FAR_GI_W_RING : f32 = 0.09375;
const FAR_GI_OMEGA_NORMAL : f32 = 0.8418;
const FAR_GI_OMEGA_RING : f32 = 0.6802;
// The burial weight of a point (raymarch.wgsl farHitBuried, verbatim: the
// ground in front of the face -- farFrontTop, the LOWEST of the four
// sub-column tops of the level cell half a sub-column out along `n` --
// FAR_BURIED_LO..HI fine voxels over the point; raymarch.wgsl farFrontTop says
// why it is not one column). -1 = no column.
fn farFrontTop(level : u32, p : vec3f, n : vec3f) -> f32 {
  let w = f32(1u << (farCellShift(level) - 1u));
  let q = p + n * (0.5 * w);
  let c = vec2<i32>(floor(vec2f(q.x, q.z) / w)) >> vec2<u32>(1u);
  let b = farBox(level);
  if (any(c < b.lo.xz) || any(c >= b.hi.xz)) { return -1e9; }
  let e = farMap[farMapCell(level, c)];
  return f32(min(min(farMapTop(e.x), farMapTop(e.y)), min(farMapTop(e.z), farMapTop(e.w))) + 1);
}
fn farBuriedW(level : u32, p : vec3f, n : vec3f) -> f32 {
  let w = f32(1u << (farCellShift(level) - 1u));
  let top = farFrontTop(level, p, n);
  if (top < -1e8) { return -1.0; }
  return smoothstep(24.0 + 2.0 * w, 48.0 + 2.0 * w, top - p.y);
}
// The material a level cell holds (0 = air or blocker-only).
fn farGiMat(level : u32, c : vec3<i32>, b : FarBox) -> u32 {
  if (!farInValid(c, b)) { return 0u; }
  let bi = farVoxByteIndex(level, c);
  let slot = farCellSlot((farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu);
  return select(farPalMat(&materials, slot), 0u, slot == 0u);
}
// What a level cell of material `m` sends back along -d through its face on
// `axis` (sign from d): emission always; the sun only when the cell is not
// buried (`litW` = 1 - its burial).
fn farGiRadiance(m : u32, axis : i32, d : vec3f, litW : f32) -> vec3f {
  let mt = materials[m];
  let albedo = (unpackColor(mt.color0) + unpackColor(mt.color1) + unpackColor(mt.color2)) *
               (1.0 / 3.0);
  let nH = axisVec(axis, -sign(axisPick(d, axis)));
  // The MEAN of a burning cell's breath, as every irradiance deposit takes it
  // (the near walk's and this pass's own near-patch deposit).
  let bt = burnTint(mt, albedo, f32(mt.emission) / 255.0, burnTintMean());
  return irrSample(bt.albedo, nH, keyLightDirP(R), keyLightColorP(R), litW, bt.emis);
}
// One gather ray through `level` from `ro` (fine voxels): its radiance
// toward the patch, coverage-weighted (see above).
fn farGiRay(level : u32, ro : vec3f, rdIn : vec3f, omega : f32, b : FarBox) -> vec3f {
  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let s = f32(1u << farCellShift(level));
  let roL = ro / s;
  let inv = 1.0 / rd;
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var c = vec3<i32>(floor(roL));
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    tMax[a] = (f32(c[a]) + select(0.0, 1.0, rd[a] > 0.0) - roL[a]) * inv[a];
  }
  let tEnd = FAR_GI_REACH_M / VOXEL_METERS / s;
  var t = 0.0;
  var axis = 1;
  for (var i = 0; i < FAR_GI_STEPS; i++) {
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      c.x += stepv.x; t = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      c.y += stepv.y; t = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      c.z += stepv.z; t = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    if (t > tEnd || !farInValid(c, b)) { return vec3f(0.0); }
    let m = farGiMat(level, c, b);
    if (m == 0u) { continue; }
    // The hit: its own radiance over its share of the cone, the plane's mean
    // over the rest. The sun term only for a cell under the open sky.
    let nIn = axisVec(axis, -sign(axisPick(rd, axis)));
    let hc = (vec3f(c) + 0.5) * s;
    let litW = 1.0 - max(farBuriedW(level, hc, nIn), 0.0);
    let own = farGiRadiance(m, axis, rd, litW);
    let tF = max(t * s, 1.0);
    let cov = min(s * s * (abs(rd.x) + abs(rd.y) + abs(rd.z)) / (tF * tF * omega), 1.0);
    var fill = vec3f(0.0);
    if (cov < 1.0) {
      let u = axisVecI((axis + 1) % 3, 1);
      let v = axisVecI((axis + 2) % 3, 1);
      var nb = vec3f(0.0);
      for (var k = 0; k < 4; k++) {
        let o = select(select(select(-v, v, k == 2), -u, k == 1), u, k == 0);
        let mn = farGiMat(level, c + o, b);
        if (mn != 0u) { nb += farGiRadiance(mn, axis, rd, litW); }
      }
      fill = nb * 0.25;
    }
    return own * cov + fill * (1.0 - cov);
  }
  return vec3f(0.0);
}
// The nine-ray gather from a far patch centre `hp` (already half a cell off
// the face along `n`), gi_gather.wgsl's directions and weights.
fn farGather(level : u32, hp : vec3f, n : vec3f, face : u32) -> vec3f {
  let b = farBox(level);
  let tg = shadowFaceTangents(face);
  let t0 = axisVec(i32(tg.x), 1.0);
  let t1 = axisVec(i32(tg.y), 1.0);
  var e = vec3f(0.0);
  // INSIDE THE RESIDENCY WINDOW THE FAR PATCH TAKES THE NEAR GATHER (2026-10-14,
  // ticket 2026-10-12_16-36-34: a crystal cave's walls 4-6x brighter past the
  // 20.5 m seam). The cascade march below sends every hit cell's radiance from
  // its MATERIAL (irrSample, emission whole); the near field's gather reads
  // what the grid holds for the faces it hits -- the deposits, multi-bounce,
  // the median background (gi_gather_rays.wgsli). Two radiance sources for
  // one wall can only agree by accident, and the seam is always inside the
  // window (render.lodHandoffDist < its half extent), so here the far patch
  // runs THE gather, from its own centre, over the real voxels: the far wall
  // takes exactly the light the near wall beside it takes. Measured at the
  // ticket camera (SEAM_DBG 4, the 10-19 m band, same pixels near and far):
  // far bounce mean 26.2 -> 8.5 against the near's 11.0 (encoded x4). Outside
  // the window no near grid exists, and the cascade march stays.
  if (inWindow(vec3<i32>(floor(hp)), R.origin)) {
    // Mask loops (2026-10-15, perf): a counted loop of constant trip is
    // unrolled by the driver, nine copies of a DDA in a kernel whose binary
    // was 205 KB. Same rays, same order.
    var mi = 0x1FFu;
    while (mi != 0u) {
      let r = firstTrailingBit(mi);
      mi &= mi - 1u;
      e += giGatherRay(hp, n, face, r);
    }
    return e;
  }
  var m9 = 0x1FFu;
  while (m9 != 0u) {
    let r = firstTrailingBit(m9);
    m9 &= m9 - 1u;
    let su = select(select(0.0, 1.0, r == 1u || r == 5u || r == 6u), -1.0,
                    r == 2u || r == 7u || r == 8u);
    let sv = select(select(0.0, 1.0, r == 3u || r == 5u || r == 7u), -1.0,
                    r == 4u || r == 6u || r == 8u);
    let d = normalize(n + t0 * su + t1 * sv);
    let w = select(FAR_GI_W_RING, FAR_GI_W_NORMAL, r == 0u);
    let om = select(FAR_GI_OMEGA_RING, FAR_GI_OMEGA_NORMAL, r == 0u);
    e += farGiRay(level, hp, d, om, b) * w;
  }
  return e;
}

// ---- THE GROUND IN FRONT OF A FAR RISER IS LIT AS THE CASCADE LIGHTS IT ----
// (2026-10-13, ticket 2026-10-09_14-23-01: sunlit and overcast ground +9..+16
// luma brighter past the 20.5 m handoff.) A far riser's bounce is a closed
// form (raymarch.wgsl farGiBounce): the ring rays that lean down land on the
// tread in front, and that tread was taken to be in FULL SUN, always. The near
// field's gather reads what the shadow resolve deposited there, which is the
// tread's real light: under a crown, in a tree's afternoon shadow, nothing.
// Attributed with `--gate lod-seam` (the same pixels shaded near and far):
// with the far ground's bounce removed altogether the ground under a canopy
// went +16.1 -> 0.0 and open ground +15.3 -> +4.9 at noon (overcast +16.3 ->
// -1.1 / +15.5 -> +2.5); the cascade's openness (the far fan read where the
// near grid is) was the other ~6 on open ground and ~1 under a canopy.
//
// So an OPEN riser patch also carries the sun its foreground gets: the same
// march and law the patch's own sun takes, from a point just over the
// heightfield top in front of it (the column the patch centre stands over,
// half a cell out). Stored in the patch's shadowHist word, tagged
// FAR_GI_FG_TAG (low bits 10: a buried gather word always has bit 0 set),
// and refreshed on the gather's period; raymarch.wgsl's far shade multiplies
// the riser's closed form by it. An underside keeps the closed form: the
// ground under an overhang is not the heightfield top over it.
const FAR_GI_FG_TAG : u32 = 2u;   // raymarch.wgsl FAR_GI_FG_TAG agrees
// farForegroundLit, the law: from (hp.x, the column top + 0.05 cell, hp.z),
// the patch march's farShadowTC + farUnderTerrain law; 1 where the surface
// map has no column (the closed form as it was). Since 2026-10-15 it is cast
// through the patch march's own call site in resolveFarPatch (ONE MARCH CALL
// SITE), which is the only place it lives.

// ---- THE FAR GROUND'S OPENNESS, BY THE NEAR WALK'S DEFINITION ----------
// (2026-10-14, ticket 2026-10-09_14-23-01.) The heightfield's openness is the
// near walk's (sim_openness openValueAt) marched over the cascade, ONCE PER
// PATCH here and read per pixel from the patch word (raymarch.wgsl
// farGiCached, bits 16..23). It was a per-pixel fan in fs until the same day:
// the seam numbers below, at +2.6 ms on the --render-budget overlook; the
// patch is already resolved, cached and refreshed every FAR_GI_PERIOD
// resolves, which is what a quantity of the static geometry wants.
//
// WHY THE DEFINITION. Measured with raymarch.wgsl SEAM_DBG 16 (the near
// definition re-measured per pixel, dbgNearOpenDef, agrees with the grid's
// byte to ~3 %): of the pixels `--gate lod-seam` classes "ground", 93 % are
// RISERS, and the riser / up-face ratio the near walk measures is 0.62 on
// open ground but 1.68 under a crown -- a riser's level rays see out UNDER
// the canopy -- so no multiple of an up fan (the far law until then:
// farOpenness, 1 tread / 0.6 riser, x the cascade's up fan) can stand for it.
// The up fan itself read the clearing 1.2x more open than the near walk (0.60
// against 0.50), the canopy over a crown's foot 2x (0.32 against 0.15). So:
//   - the face's OWN fan (a riser's normal is level), a side face's
//     down-diagonal re-aimed 63 degrees up ("a vertical face does not ask the
//     ground");
//   - real rays in fine-voxel space, the reach (render.opennessReach)
//     measured ALONG the ray (the cell-diagonal step reached 17 m on a
//     diagonal, over the crowns the near walk ends under);
//   - an unblocked ray is not sky: it climbs straight up from its endpoint;
//   - the outer stretch is a BLOCK MASK: a cell counts if any of its eight
//     children one level finer is a ray blocker. The near walk's coarse mode
//     is "any blocker in a 40 cm block"; the cascade's 80 cm cell is one
//     cover-rule sample, porous to a ray through a crown's edge (this rule
//     alone: 0.60 -> 0.49 on the clearing's up fan). Measured, NOT TO BE CUT
//     BLIND: dropping it on level rays and on the climb put ground under a
//     crown back to +7.7..+8.4 luma.
// Mean openness of the same pixels (near grid / far before / far now): open
// ground 0.30 / 0.38 / 0.35, under a crown 0.25 / 0.22 / 0.25, grass blades
// 0.23 / 0.34 / 0.27. A map-terrain test (a sample under the level-1
// heightfield top is solid) was measured and changed nothing.
// MEDIA-BLIND, as the near walk is (isRayBlocker): clear water is no roof.
// MUST AGREE: raymarch.wgsl farSkySolid (the feature fan's sample).
const FAR_OPEN_REACH_V : f32 = TUNE_OPENNESS_REACH / VOXEL_METERS;
// Weights: the up ray 1/3, each of the four diagonals 1/6 (farOpenByte counts
// them in sixths).
fn farSlotAt(level : u32, c : vec3<i32>) -> u32 {
  let bi = farVoxByteIndex(level, c);
  return farCellSlot((farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu);
}
fn farSkySolid(level : u32, c : vec3<i32>, b : FarBox) -> bool {
  if (!farInValid(c, b)) { return false; }
  let occ = farOcc[farOccIndex(level, c)];
  if (occ == 0u) { return false; }
  let top = farOccTop(occ);
  if (top != 0u && (c.y & (i32(CHUNK) - 1)) >= i32(top)) { return false; }
  let slot = farSlotAt(level, c);
  return slot != 0u && isRayBlocker(materials[farPalMat(&materials, slot)]);
}
// Is anything at or above row c.y of cell c's level chunk? (farOcc's count
// and top row; false = every cell from this row up to the chunk's top is
// air.) Outside the valid box: no.
fn farOccAbove(level : u32, c : vec3<i32>) -> bool {
  if (!farInValid(c, farBox(level))) { return false; }
  let occ = farOcc[farOccIndex(level, c)];
  if (occ == 0u) { return false; }
  let top = farOccTop(occ);
  return top == 0u || (c.y & (i32(CHUNK) - 1)) < i32(top);
}
// THE BLOCK MASK's other half: any of the eight children (level lv - 1) of
// level-lv cell c a ray blocker? The eight share one level chunk (2c is even),
// so ONE farOcc word answers "nothing here" for all of them before any byte.
fn farChildrenSolid(lv : u32, c : vec3<i32>) -> bool {
  let lc = lv - 1u;
  let c0 = c * 2;
  if (!farOccAbove(lc, c0)) { return false; }
  for (var k = 0; k < 8; k++) {
    let slot = farSlotAt(lc, c0 + vec3<i32>(k & 1, (k >> 1) & 1, (k >> 2) & 1));
    if (slot != 0u && isRayBlocker(materials[farPalMat(&materials, slot)])) { return true; }
  }
  return false;
}
// THE PREFILTER: does anything stand over the hit within the fan's reach?
// (false = the closed form is what the near grid measures, as on flat open
// ground, where it was calibrated). The two-levels-coarser chunk round the
// cell and its four side neighbours, one chunk row up; open if none of them
// holds anything over the cell's row (+1 row of terrain relief), or if the
// cell is over its level's sky ceiling. A column-only test was tried first and
// was wrong: the diagonals of a clearing's grass land in the crowns AROUND it.
fn farGroundOver(level : u32, cell : vec3<i32>) -> bool {
  if (f32(cell.y + 1) >= farSkyCeil(level)) { return false; }
  let shift = farCellShift(level);
  let l2 = min(level + 2u, FAR_LEVELS);
  let b2 = farBox(l2);
  let o2 = ((cell << vec3<u32>(shift)) + vec3<i32>(i32(1u << shift) >> 1u)) >>
           vec3<u32>(farCellShift(l2));
  let ch = i32(CHUNK);
  for (var i = 0; i < 10; i++) {
    let side = i >> 1;
    var off = vec3<i32>(0, (i & 1) * ch, 0);
    if (side == 1) { off.x = ch; } else if (side == 2) { off.x = -ch; }
    else if (side == 3) { off.z = ch; } else if (side == 4) { off.z = -ch; }
    let pr = o2 + off;
    if (!farInValid(pr, b2)) { continue; }
    let occ = farOcc[farOccIndex(l2, pr)];
    if (occ == 0u) { continue; }
    let top = farOccTop(occ);
    if (top == 0u || (pr.y >> 4u) * ch + i32(top) - 1 > o2.y + 1) { return true; }
  }
  return false;
}
// The definition from `ro` (fine voxels, just off the face) on face normal
// `ni` (an axis), 0..1: the weighted sum of its five rays (farOpenRay), in
// SIXTHS -- the up ray 2, each diagonal 1 -- which is what the float sum of
// 1/3 and 1/6 was, byte for byte after the *254 + 0.5 (all 32 combinations
// checked in float32; no sum lies within 1e-5 of a rounding edge).
// Since 2026-10-15 the five rays are cast by whichever lanes of the
// workgroup are free (THE HEAVY RAYS ARE SHARED, at resolve) and summed in
// wgFanVis; farOpenByte below is that sum's byte.
const FAR_OPEN_SIXTHS_UP : u32 = 2u;
fn farOpenByte(sixths : u32) -> u32 {
  return u32(clamp(f32(sixths) / 6.0, 0.0, 1.0) * 254.0 + 0.5);
}
// Ray `r` of the fan (0 the face normal, 1..4 the diagonals): does it see sky?
fn farOpenRay(level : u32, ro : vec3f, ni : vec3<i32>, r : i32) -> bool {
  let t1 = select(vec3<i32>(1, 0, 0), vec3<i32>(0, 0, 1), ni.x != 0);
  let t2 = vec3<i32>(ni.y * t1.z - ni.z * t1.y, ni.z * t1.x - ni.x * t1.z,
                     ni.x * t1.y - ni.y * t1.x);
  let l2 = min(level + 2u, FAR_LEVELS);
  let s0 = f32(1u << farCellShift(level));
  let s2 = f32(1u << farCellShift(l2));
  let b2 = farBox(l2);
  {
    var di = ni;
    if (r == 1) { di = ni + t1; } else if (r == 2) { di = ni - t1; }
    else if (r == 3) { di = ni + t2; } else if (r == 4) { di = ni - t2; }
    if (ni.y == 0 && di.y < 0) { di = ni + vec3<i32>(0, 2, 0); }
    let d = normalize(vec3f(di));
    var t = 0.0;
    var open = true;
    var sky = false;
    for (var k = 0; k < 48; k++) {
      let outer = t >= 16.0 - 1e-3;
      // The first outer sample is the first l2 cell past the inner stretch.
      if (outer && t < 16.0 + 1e-3) { t = (floor(16.0 / s2) + 1.0) * s2; }
      else { t += select(s0, s2, outer); }
      if (t > FAR_OPEN_REACH_V + 1e-3) { break; }
      let lv = select(level, l2, outer);
      let p = ro + d * t;
      let c = vec3<i32>(floor(p / select(s0, s2, outer)));
      if (d.y >= 0.0 && f32(c.y) >= farSkyCeil(lv)) { sky = true; break; }
      if (farSkySolid(lv, c, farBox(lv)) || (outer && lv > 1u && farChildrenSolid(lv, c))) {
        open = false;
        break;
      }
    }
    // AN UNBLOCKED RAY IS NOT SKY: straight up from the endpoint, at the
    // outer level with its block mask, to the level's sky ceiling. The
    // ceiling is the level's ONE top, so the climb skips, exactly: a row
    // where neither the cell's chunk nor its children's has anything at or
    // above it is air to the top of the children's chunk (8 cells of l2).
    if (open && !sky) {
      let e = vec3<i32>(floor((ro + d * FAR_OPEN_REACH_V) / s2));
      var cy = e.y + 1;
      for (var k = 0; k < 24; k++) {
        if (f32(cy) >= farSkyCeil(l2)) { break; }
        let c = vec3<i32>(e.x, cy, e.z);
        if (!farOccAbove(l2, c) && !(l2 > 1u && farOccAbove(l2 - 1u, c * 2))) {
          cy = ((cy >> 3u) + 1) << 3u;
          continue;
        }
        if (farSkySolid(l2, c, b2) || (l2 > 1u && farChildrenSolid(l2, c))) { open = false; break; }
        cy += 1;
      }
    }
    return open;
  }
}
// The patch word's openness bytes: (the ground's, the blade cards'), each
// 0..254, or 255 when the patch is not the GROUND (a crown or trunk cell
// clear of the heightfield -- the feature fan in fs is theirs) or nothing
// stands near it (the prefilter). `fp` is the patch centre ON the face.
//
// THE CARDS HAVE THEIR OWN. A far blade card is drawn in the GROUND cell
// (raymarch.wgsl farFeatCell), so a card pixel reads the same patch as the
// tread under it, but the pixels on a card are spread up the blade, and a
// fan from half the blade's height up sees more sky than one from the
// ground (measured with SEAM_DBG 18, the cached value against the per-pixel
// fan on the same pixels: cards 0.76x from the ground's start). An up face's
// riser-foreground byte is unused, so the cards' value rides there (bits
// 8..15).
const FAR_FEAT_VEC : u32 = FAR_LEVELS * FAR_N * FAR_N;   // raymarch.wgsl agrees
const FAR_OPEN_FADE_END_M : f32 = 110.0;                 // raymarch.wgsl FAR_SKY_FADE_END_M
const FAR_FEAT_SOLID : u32 = 0x40000000u;                // raymarch.wgsl agrees
// .z: a FEATURE face's openness (2026-10-14): a cell standing ON its
// column's top (a crown, a trunk, a wall; the heightfield's own cells and a
// card's air cell are under or at it) takes the definition from just off the
// face, with none of the ground's start rules. Measured per pixel with
// SEAM_DBG 16 against the near grid: the old cell-diagonal feature fan read
// the crowns ~0.7x (leaf 0.14 vs 0.19, bark 0.18 vs 0.21). 255 = not a
// feature cell (fs keeps its closed form).
// THE PLAN (2026-10-15, perf): which of the three fans this patch needs and
// where they start; mask 0 = none (beyond the fade, or the prefilter found
// nothing near: every byte 255). The three fans used to be three inlined
// calls of the fan here, cast by the patch's own lane.
struct FarOpenPlan { mask : u32, featOnly : bool, y : vec2f };
fn farGroundOpenPlan(level : u32, cell : vec3<i32>, fp : vec3f, n3 : vec3f,
                     refined : bool) -> FarOpenPlan {
  var none : FarOpenPlan;
  none.mask = 0u;
  none.featOnly = true;
  let s = f32(1u << farCellShift(level));
  // Past the distance fs eases the term out by (raymarch.wgsl farSkyFade:
  // FAR_SKY_FADE_END_M) nothing reads it: no fan.
  if (length(fp - R.camPos) * VOXEL_METERS > FAR_OPEN_FADE_END_M) { return none; }
  let top = farColumnTop(level, fp - n3 * (0.25 * s));
  if (top < -1e8 || !farGroundOver(level, cell)) { return none; }
  let wantF = f32(cell.y) * s >= top - 0.01;
  let featOnly = f32(cell.y) * s > top + s;
  var ro = fp + n3 * 0.5;
  let cy = f32(cell.y) * s;
  // A refined tread starts from its sub-column's top, as the pixel on it does.
  if (refined && n3.y > 0.5) { ro.y = top + 0.5; }
  // A RISER's patch is at a fixed height in its cell (a quarter / three
  // quarters up) on the cell's camera-side boundary, but the faces a pixel
  // sees are only the EXPOSED span -- from the top of the column in front up
  // to the higher of the cell's two sub-columns along the normal (a refined
  // cell's step can be at its boundary or half a cell in). A start under the
  // front column's top is inside the step below, and its level rays close on
  // the first cell (measured: open ground -7 luma at noon). So the
  // sub-patch's height in the cell is mapped onto that span, the way the
  // pixels on it are spread.
  if (abs(n3.y) < 0.5) {
    let front = farColumnTop(level, fp + n3 * (0.25 * s));
    if (front > -1e8) {
      let inner = max(top, farColumnTop(level, fp - n3 * (0.75 * s)));
      let lo = clamp(front, cy, cy + s);
      let hi = max(clamp(inner, cy, cy + s), lo);
      ro.y = mix(lo, hi, clamp((fp.y - cy) / s, 0.0, 1.0));
    }
  }
  var hg = 0u;
  if (!featOnly && n3.y > 0.5) {
    // The tallest plant of the cell's four sub-columns (farFeatCell's words:
    // height bits 8..11; a SOLID word is a stalk, not a card). Not the one
    // under the patch centre: a card pixel reads its CELL's patch for the
    // sub-patch it falls in, and measured (SEAM_DBG 19) a third of them sat
    // over a sub-column with no plant of its own, so took the ground's start.
    let q = farMap[FAR_FEAT_VEC + farMapCell(level, cell.xz)];
    for (var k = 0u; k < 4u; k++) {
      let e = select(select(q.x, q.y, k == 1u), select(q.z, q.w, k == 3u), k >= 2u);
      if ((e & FAR_FEAT_SOLID) == 0u) { hg = max(hg, (e >> 8u) & 15u); }
    }
  }
  // Fan 0 = the feature face's (f, from fp + n3/2), 1 = the ground's (g, from
  // `ro`), 2 = the cards' (c, from `ro` lifted to half the tallest plant).
  // All three start at fp + n3/2 in x and z, so a plan is the mask and two
  // heights; the fans are cast by the workgroup (farFanTask) and composed by
  // farOpenCompose.
  var p : FarOpenPlan;
  p.mask = select(0u, 1u, wantF) | select(0u, 2u, !featOnly) | select(0u, 4u, hg > 0u);
  p.featOnly = featOnly;
  p.y = vec2f(ro.y, top + 0.5 * f32(hg) + 0.5);
  return p;
}
// The patch word's three bytes from the plan's fan sums (sixths, by fan).
fn farOpenCompose(plan : FarOpenPlan, six : vec3<u32>) -> vec3<u32> {
  var b = vec3<u32>(255u);
  if ((plan.mask & 1u) != 0u) { b.z = farOpenByte(six.x); }
  if ((plan.mask & 2u) != 0u) { b.x = farOpenByte(six.y); }
  if ((plan.mask & 4u) != 0u) { b.y = farOpenByte(six.z); }
  if (plan.featOnly) { return vec3<u32>(255u, 255u, b.z); }
  // No plant: the cards read the ground's own fan.
  if ((plan.mask & 4u) == 0u) { b.y = b.x; }
  return b;
}

// Returns true when the patch's word waits on its openness fans (farFinish).
fn resolveFarPatch(key : u32, bucket : u32, packedCell : u32, packedSub : u32,
                   lane : u32) -> bool {
  var pending = false;
  let level = ((packedSub >> FAR_SC_LEVEL_SHIFT) & 7u) + 1u;
  let ab = FAR_SC_AXIS_BITS;
  let tor = vec3<i32>(vec3<u32>(packedCell, packedCell >> ab, packedCell >> (2u * ab)) &
                      vec3<u32>(FAR_N - 1u));
  let face = (packedCell >> (3u * ab)) & 7u;
  let box = farBox(level);
  // The unique representative of the toroidal cell inside the level's box
  // (farVox's own aliasing, FAR_N per axis).
  let cell = box.lo + ((tor - box.lo) & vec3<i32>(i32(FAR_N) - 1));
  var valU = 255u;
  var valid = false;
  if (level <= FAR_LEVELS && farInValid(cell, box)) {
    let s = f32(1u << farCellShift(level));
    let n3 = shadowFaceNormal(face);
    var hp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                               FAR_SC_SUBDIV) * s + n3 * (0.55 * s);
    if ((packedSub & FAR_SC_REFINED) != 0u) {
      hp.y = max(hp.y, f32(cell.y + 1) * s + 0.05 * s);
    }
    let L = keyLightDirP(R);
    // A FULLY BURIED patch has no sun to measure: every reader caps the sun
    // by the face's openness (shadowLiftCap), which burial takes to exactly 0,
    // and raymarch.wgsl does not even ask for a pixel there. Such a patch is
    // here for its indirect light (below), so it skips the march.
    var bw = farBuriedW(level, hp, n3);
    // A WET FACE IS NOT BURIED BY ITS WATER (raymarch.wgsl farWaterBed: the
    // bed under far water reads its face's patch). The surface map's top over
    // a lake or the sea is the water's, so burial took a bed 3 m down for a
    // face 3 m under rock and skipped its sun; the near field's shadow and
    // openness rays pass clear water (media-blind), and so does the march.
    let front = farGiMat(level, cell + vec3<i32>(n3), box);
    if (front != 0u && materials[front].klass == CLASS_LIQUID &&
        !isRayBlocker(materials[front])) {
      bw = 0.0;
    }
    valid = true;
    // Whether this resolve re-measures the patch's word (below), read up here
    // because the riser's foreground march (farForegroundLit) shares the
    // patch march's call site.
    let riser = abs(n3.y) < 0.5;
    let ver0 = shadowPatchVerifier(packedCell, packedSub);
    let old0 = atomicLoad(&shadowCache[bucket * 2u + 1u]);
    let mine = shadowStateValid(old0) && shadowStateVerifier(old0) == ver0;
    let due = !mine || shadowHist[bucket] == 0u ||
              ((R.frameIdx / SHADOW_REFRESH_PERIOD + (key >> 8u)) % FAR_GI_PERIOD) == 0u;
    // ONE MARCH CALL SITE (2026-10-15, perf: one inlined farShadowTC where
    // there were two, in a kernel whose binary was 205 KB): march 0 is the
    // patch's own sun from `hp`, march 1 the riser's foreground's,
    // farForegroundLit's start and law verbatim (a riser with no surface-map
    // column keeps 1, as farForegroundLit returned).
    let fgTop = farColumnTop(level, hp);
    let pFg = vec3f(hp.x, fgTop + 0.05 * s, hp.z);
    var shK = vec2f(1.0);
    var mk = select(0u, 1u, bw < 1.0) |
             select(0u, 2u, due && !(bw > 0.001) && riser && fgTop > -1e8);
    while (mk != 0u) {
      let k = firstTrailingBit(mk);
      mk &= mk - 1u;
      let p = select(hp, pFg, k == 1u);
      let r = farShadowTC(level, p, L, 0.0, true);
      var v = 1.0;
      if (r.x >= 0.0) { v = mix(1.0, shadowFromOpaqueHit(true, r.x, 0u), r.y); }
      else if (farUnderTerrain(p, L, TUNE_FAR_SHADOW_REACH / VOXEL_METERS)) {
        // A CLEAR RAY UNDER THE TERRAIN IS NOT SUNLIT (above).
        v = shadowFromOpaqueHit(true, TUNE_FAR_SHADOW_REACH / VOXEL_METERS, 0u);
      }
      if (k == 0u) { shK.x = v; } else { shK.y = v; }
    }
    var sh = 0.0;
    if (bw < 1.0) {
      sh = shK.x;
      // A BLOCKER AT CONTACT IS A SHADOW, NOT "NO OPINION" (2026-10-14,
      // ticket 2026-10-14_00-52-22). This used to publish the slot INVALID
      // when the ray met a cell within 0.01 voxels of its start, after the
      // near patch's burial rule. But a far pixel reads ONE patch, with
      // nothing to renormalise onto, and a patch with no opinion shades as
      // full sun: every far face whose start point sat in a neighbouring
      // cell -- the inside faces of a distant canopy, mostly -- was lit,
      // permanently: ~900 px of a still frame at the ticket camera, lit
      // specks over every far tree. The cache-off reference (raymarch.wgsl's farShadowMarch
      // branch) has no such rule and answers a contact shadow; so does this.
    }
    valU = u32(clamp(sh, 0.0, 1.0) * 255.0 + 0.5);
    // THE FAR PATCH'S WORD (above): a buried patch's gather; an open patch's
    // riser foreground (FAR_GI_FG_TAG) and ground openness (farGroundOpen,
    // bits 16..23). Every patch carries one now (a tread used to keep 0).
    var giW = 0u;
    if (due) {
      if (bw > 0.001) {
        giW = packRgb9e5(farGather(level, hp, n3, face)) | 1u;
      } else {
        // THE OPENNESS IS THE GEOMETRY'S, NOT THE SUN'S: measured on the
        // patch's first resolve and KEPT across its refreshes (the refresh is
        // for the riser's foreground sun, which moves with the day). Only a
        // patch with no measurement (255: beyond the fade, or nothing near
        // when it was asked) asks again, which is a distance test and the
        // prefilter's ten farOcc words. An edit under a live patch keeps the
        // old value until the slot is reclaimed (a far-field edit is a
        // fardown refresh of what is already 10+ m away; the near grid owns
        // the edited ground inside the handoff).
        let oldW = shadowHist[bucket];
        var ob = vec3<u32>((oldW >> 16u) & 0xFFu, (oldW >> 8u) & 0xFFu, oldW >> 24u);
        let fgB = u32(clamp(shK.y, 0.0, 1.0) * 255.0 + 0.5);
        if (!mine || (oldW & 3u) != FAR_GI_FG_TAG || (ob.x == 255u && ob.z == 255u)) {
          let fp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                                     FAR_SC_SUBDIV) * s;
          let plan = farGroundOpenPlan(level, cell, fp, n3, (packedSub & FAR_SC_REFINED) != 0u);
          if (plan.mask != 0u) {
            // The fans are the workgroup's (THE HEAVY RAYS ARE SHARED); the
            // word is composed by farFinish once they are in. The publish
            // below does not wait for it: nothing reads this pass's
            // shadowHist words before the dispatch ends.
            wgFarPlan[lane] = vec4<u32>(plan.mask | select(0u, 8u, plan.featOnly),
                                        bitcast<u32>(plan.y.x), bitcast<u32>(plan.y.y),
                                        fgB | select(0u, 256u, riser));
            pending = true;
          }
          ob = farOpenCompose(plan, vec3<u32>(0u));
        }
        // bits 8..15: a riser's foreground sun; an up face's cards' openness.
        let fg = select(ob.y, fgB, riser);
        giW = (ob.z << 24u) | (ob.x << 16u) | (fg << 8u) | FAR_GI_FG_TAG;
      }
    } else {
      giW = shadowHist[bucket];
    }
    if (!pending) { shadowHist[bucket] = giW; }
  }
  let ver = shadowPatchVerifier(packedCell, packedSub);
  if (atomicLoad(&shadowCache[bucket * 2u]) != key) { return pending; }
  let old = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  if (shadowStateVerifier(old) != ver) { return pending; }
  atomicStore(&shadowCache[bucket * 2u + 1u],
              shadowPackState(valU, R.frameIdx & 15u, shadowStateRequested(old),
                              valid, ver));
  return pending;
}

// The far patch's word once its fans are in: the plan and the foreground byte
// resolveFarPatch left in wgFarPlan, the fan sums in wgFanVis.
fn farFinish(lane : u32, bucket : u32) {
  let w = wgFarPlan[lane];
  var plan : FarOpenPlan;
  plan.mask = w.x & 7u;
  plan.featOnly = (w.x & 8u) != 0u;
  let six = vec3<u32>(atomicLoad(&wgFanVis[lane * 3u]), atomicLoad(&wgFanVis[lane * 3u + 1u]),
                      atomicLoad(&wgFanVis[lane * 3u + 2u]));
  let ob = farOpenCompose(plan, six);
  let riser = (w.w & 256u) != 0u;
  let fg = select(ob.y, w.w & 255u, riser);
  shadowHist[bucket] = (ob.z << 24u) | (ob.x << 16u) | (fg << 8u) | FAR_GI_FG_TAG;
}

// The order a FRESH window casts its samples in (resolve, A FRESH PATCH CASTS
// ITS WHOLE WINDOW): the undeflected centre, then two rim samples of the
// sunflower ~190 degrees apart (15 and 11: golden angle x 4), then the rest.
fn shadowFreshOrder(k : u32) -> u32 {
  if (k == 0u) { return 0u; }
  if (k == 1u) { return 15u; }
  if (k == 2u) { return 11u; }
  let j = k - 2u;   // 1..13 -> the samples 1..10, 12, 13, 14
  return select(j, j + 1u, j >= 11u);
}

// ======================= THE HEAVY RAYS ARE SHARED (2026-10-15, perf) =======
// WHAT THIS FIXES. A request used to be one lane's whole job, and a warp runs
// for its slowest lane. Most requests are one ray (a valid near patch's
// window sample) or one cheap march (a valid far patch), but two kinds are
// long: a FRESH near window in a penumbra casts all sixteen samples (A FRESH
// PATCH CASTS ITS WHOLE WINDOW), and a FRESH far patch casts up to three
// openness fans of five rays each (THE FAR GROUND'S OPENNESS). They are rare
// -- on the 12-10-09 ticket flight ~110 sixteen-ray windows and ~3,600 fresh
// far patches a frame among ~41,000 requests -- but nearly every warp held
// one, so nearly every warp ran as long as it. Measured on that flight
// (attribution arms, one exe, exclusive): capping the sixteen at the three
// probes took 0.61 ms off a 1.85 ms pass, skipping the fans 0.44 ms.
//
// So the RAYS are the unit of work, not the request. The workgroup is one
// warp (32 lanes). Each lane reads its request and does the request's own
// bookkeeping, but the rays it needs go into the group's task list
// (stageTask), and every lane takes tasks from the list in turn -- a
// sixteen-ray window becomes one ray on each of sixteen lanes. A window's
// later steps depend on its first rays (the probes decide whether the other
// thirteen are needed), so the group runs in ROUNDS: cast the round's tasks,
// barrier, each owner reads its results and either finishes (nearFinish /
// farFinish) or queues its next rays. Two rounds cover the longest chain
// (centre + probes -> the thirteen). A task's results go back through workgroup
// memory: the sample bits (wgSun), the centre ray's lift and burial
// (wgRay0), a fan's ray count in sixths (wgFanVis).
//
// THE ANSWERS ARE THE SAME. Every ray is the ray the owner would have cast
// (same origin, direction, march; the origin is recomputed from the request
// by the same expressions), every bit lands where the owner's loop put it,
// and a fan's byte is the same byte (farOpenByte). Only who casts it moved.
// A task list overflowing COOP_CAP keeps the excess with its owner, which
// casts it itself in the same loop.
const RESOLVE_WG : u32 = 32u;
const COOP_CAP : u32 = 128u;
var<workgroup> wgTaskN : array<atomic<u32>, 2>;
var<workgroup> wgTaskA : array<u32, COOP_CAP>;
var<workgroup> wgTaskB : array<u32, COOP_CAP>;
var<workgroup> wgSun : array<atomic<u32>, RESOLVE_WG>;
var<workgroup> wgRay0 : array<u32, RESOLVE_WG>;
var<workgroup> wgFanVis : array<atomic<u32>, 96>;          // RESOLVE_WG x 3 fans
var<workgroup> wgFarPlan : array<vec4<u32>, RESOLVE_WG>;   // resolveFarPatch / farFinish

// A task word: the owner lane (bits 0..4), then for a near ray the sample
// (5..8), TASK_CENTRE (the undeflected ray, which owns the lift) and
// TASK_BURIEDRAY (the ray the burial test reads); for a fan ray TASK_FAR, the
// fan (5..6) and the ray (7..9).
const TASK_FAR : u32 = 0x80000000u;
const TASK_CENTRE : u32 = 0x200u;
const TASK_BURIEDRAY : u32 = 0x400u;
// A lane's stage: what its queued rays are, i.e. what it reads next.
const ST_DONE : u32 = 0u;
const ST_SINGLE : u32 = 1u;     // a valid window: one sample (aux: sample | centre << 4)
const ST_FRESH : u32 = 2u;      // a fresh window: the centre and the two rim probes
const ST_FULL : u32 = 3u;       // a window in a penumbra: the other thirteen
const ST_FAR : u32 = 4u;        // a far patch's openness fans (aux: the fan mask)
const SC_PROBE_BITS : u32 = 0x8801u;   // samples 0, 11, 15 (shadowFreshOrder 0..2)

fn stageTaskCount(stage : u32, aux : u32) -> u32 {
  switch (stage) {
    case ST_SINGLE: { return 1u; }
    case ST_FRESH: { return 3u; }
    case ST_FULL: { return 13u; }
    case ST_FAR: { return 5u * countOneBits(aux & 7u); }
    default: { return 0u; }
  }
}
// The j-th ray of a lane's stage.
fn stageTask(stage : u32, lane : u32, aux : u32, j : u32) -> u32 {
  switch (stage) {
    case ST_SINGLE: {
      return lane | ((aux & 15u) << 5u) | select(0u, TASK_CENTRE, (aux & 16u) != 0u) |
             TASK_BURIEDRAY;
    }
    case ST_FRESH: {
      return lane | (shadowFreshOrder(j) << 5u) |
             select(0u, TASK_CENTRE | TASK_BURIEDRAY, j == 0u);
    }
    case ST_FULL: { return lane | (shadowFreshOrder(j + 3u) << 5u); }
    default: {
      // ST_FAR: fan = the (j / 5)-th set bit of the mask, ray = j % 5.
      var m = aux & 7u;
      for (var q = 0u; q < j / 5u; q++) { m &= m - 1u; }
      return TASK_FAR | lane | (firstTrailingBit(m) << 5u) | ((j % 5u) << 7u);
    }
  }
}
// Queue a lane's stage on `list`; returns how many went in (the rest are the
// owner's to cast itself, from that index on).
fn pushTasks(list : u32, stage : u32, lane : u32, aux : u32) -> u32 {
  let n = stageTaskCount(stage, aux);
  let b = atomicAdd(&wgTaskN[list], n);
  var j = 0u;
  while (j < n && b + j < COOP_CAP) {
    let t = stageTask(stage, lane, aux, j);
    if (list == 0u) { wgTaskA[b + j] = t; } else { wgTaskB[b + j] = t; }
    j += 1u;
  }
  return j;
}

// A lane's request state across the rounds.
struct ReqSt {
  stage : u32,
  aux : u32,
  hist : u32,    // the window: sample bits 0..15, the lift byte at SHADOW_LIFT_SHIFT
  fill : u32,
  flags : u32,   // ST_F_*
  old0 : u32,    // the slot's state word before this resolve (the glide reads it)
  bucket : u32,
};
const ST_F_FRESH : u32 = 1u;
const ST_F_WASFULL : u32 = 2u;
const ST_F_BURIED : u32 = 4u;

fn reqWord(r : u32, w : u32) -> u32 {
  return atomicLoad(&shadowReq[SHADOW_REQ_HEADER + r * SHADOW_REQ_WORDS + w]);
}

// resolve: the requested patches, one request per lane, their rays shared.
@compute @workgroup_size(32)
fn resolve(@builtin(local_invocation_index) li : u32,
           @builtin(workgroup_id) wid3 : vec3<u32>) {
  let wid = wid3.x;
  let n = atomicLoad(&shadowReq[1]);
  if (li < 2u) { atomicStore(&wgTaskN[li], 0u); }
  atomicStore(&wgSun[li], 0u);
  wgRay0[li] = 0u;
  atomicStore(&wgFanVis[li * 3u], 0u);
  atomicStore(&wgFanVis[li * 3u + 1u], 0u);
  atomicStore(&wgFanVis[li * 3u + 2u], 0u);
  workgroupBarrier();
  let r0 = wid * RESOLVE_WG + li;
  var st : ReqSt;
  st.stage = ST_DONE;
  if (r0 < n) {
    let key = reqWord(r0, 0u);
    let bucket = reqWord(r0, 1u);
    let packedCell = reqWord(r0, 2u);
    let packedSub = reqWord(r0, 3u);
    if ((packedSub & FAR_SC_FLAG) != 0u) {
      if (resolveFarPatch(key, bucket, packedCell, packedSub, li)) {
        st.stage = ST_FAR;
        st.aux = wgFarPlan[li].x & 7u;
        st.bucket = bucket;
      }
    } else {
      st = nearBegin(key, bucket, packedCell, packedSub);
    }
  }
  var own0 = 0u;
  var ownN = 0u;
  if (st.stage != ST_DONE) {
    own0 = pushTasks(0u, st.stage, li, st.aux);
    ownN = stageTaskCount(st.stage, st.aux);
  }
  for (var round = 0u; round < 2u; round++) {
    workgroupBarrier();
    let list = round & 1u;
    let cnt = min(atomicLoad(&wgTaskN[list]), COOP_CAP);
    var i = li;
    var j = own0;
    loop {
      var t = 0u;
      if (i < cnt) {
        t = select(wgTaskB[i], wgTaskA[i], list == 0u);
        i += RESOLVE_WG;
      } else if (j < ownN) {
        t = stageTask(st.stage, li, st.aux, j);
        j += 1u;
      } else {
        break;
      }
      runTask(t, wid);
    }
    workgroupBarrier();
    // This list is refilled two rounds on; the next round's is the other one.
    if (li == 0u) { atomicStore(&wgTaskN[list], 0u); }
    own0 = 0u;
    ownN = 0u;
    if (st.stage != ST_DONE) {
      st = resolveStep(st, li, r0);
      if (st.stage != ST_DONE) {
        own0 = pushTasks(1u - list, st.stage, li, st.aux);
        ownN = stageTaskCount(st.stage, st.aux);
      }
    }
  }
}

fn runTask(t : u32, wid : u32) {
  let o = t & 31u;
  let r = wid * RESOLVE_WG + o;
  let packedCell = reqWord(r, 2u);
  let packedSub = reqWord(r, 3u);
  if ((t & TASK_FAR) != 0u) { farFanTask(t, o, packedCell, packedSub); }
  else { nearRayTask(t, o, packedCell, packedSub); }
}

// One ray of a far patch's openness fan (farGroundOpenPlan's starts).
fn farFanTask(t : u32, o : u32, packedCell : u32, packedSub : u32) {
  let level = ((packedSub >> FAR_SC_LEVEL_SHIFT) & 7u) + 1u;
  let ab = FAR_SC_AXIS_BITS;
  let tor = vec3<i32>(vec3<u32>(packedCell, packedCell >> ab, packedCell >> (2u * ab)) &
                      vec3<u32>(FAR_N - 1u));
  let face = (packedCell >> (3u * ab)) & 7u;
  let box = farBox(level);
  let cell = box.lo + ((tor - box.lo) & vec3<i32>(i32(FAR_N) - 1));
  let s = f32(1u << farCellShift(level));
  let n3 = shadowFaceNormal(face);
  let fp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                             FAR_SC_SUBDIV) * s;
  let k = (t >> 5u) & 3u;
  let r = i32((t >> 7u) & 7u);
  var p = fp + n3 * 0.5;
  let w = wgFarPlan[o];
  if (k == 1u) { p.y = bitcast<f32>(w.y); } else if (k == 2u) { p.y = bitcast<f32>(w.z); }
  if (farOpenRay(level, p, vec3<i32>(round(n3)), r)) {
    atomicAdd(&wgFanVis[o * 3u + k], select(1u, FAR_OPEN_SIXTHS_UP, r == 0));
  }
}

// A near patch's window state before its rays: what nearBegin reads from the
// request and the slot, and which rays its first round needs.
fn nearBegin(key : u32, bucket : u32, packedCell : u32, packedSub : u32) -> ReqSt {
  // Reconstruct the patch's world-space centre and normal. The request stores
  // the cell TOROIDAL (9 bits per axis at WORLD_N 512), so it means the same
  // patch whether or not the window moved between the frame that queued it
  // and this one — shadowPackCell in common.wgsl says why that matters — and
  // it is unwrapped into the window that is current NOW, which is the window
  // the fragment shader reading the answer will be looking through.
  let cell = shadowUnwrapCell(packedCell, R.origin * i32(CHUNK));
  let face = (packedCell >> (WORLD_SHIFT * 3u)) & 7u;
  let hp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                             R.shadowSubdiv);
  let n3 = shadowFaceNormal(face);

  // ---- which sample of the window is this frame's, and is the slot ours ----
  // The slot's state word is read BEFORE the ray, for one reason: its VALID bit
  // is the only signal that says whether the window in shadowHist belongs to
  // this patch. A slot is claimed by zeroing its state word (raymarch.wgsl
  // shadowSlotRead), and nothing but this pass ever sets valid, so "not valid"
  // is exactly "claimed since the last publish" — a patch that has just taken
  // the slot over, whose predecessor's 16 samples are about some other surface
  // entirely. Carrying them over would paint one patch's penumbra onto another.
  let ver = shadowPatchVerifier(packedCell, packedSub);
  let old0 = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  let fresh = !shadowStateValid(old0) || shadowStateVerifier(old0) != ver;
  let hist = select(shadowHist[bucket], 0u, fresh);
  var fill = (hist >> SHADOW_FILL_SHIFT) & SHADOW_FILL_MASK;
  // Read BEFORE the increment below: the glide may only run once the window is
  // a full window. While it is filling, the divisor itself is changing, so the
  // output moves for a reason that has nothing to do with the scene and
  // smoothing it would only delay the first honest answer — and would leave
  // the value still creeping after the warm-up --gate shadow-cache pays for.
  let wasFull = fill >= SHADOW_SAMPLES;
  // Which window sample: the patch is resolved once every
  // SHADOW_REFRESH_PERIOD frames on its own phase (raymarch.wgsl, STAGGERED
  // REFRESH), so its sample index advances by one per RESOLVE, not per frame —
  // otherwise it would only ever land on 16 / PERIOD of the cone's samples and
  // most patches would never take the centre ray. A fresh patch resolving off
  // its phase lands on some index; `centre` covers it below.
  let slotIdx = ((R.frameIdx + shadowRefreshPhase(key)) / SHADOW_REFRESH_PERIOD)
                % SHADOW_SAMPLES;

  // THE CENTRE RAY IS NOT JITTERED, and which frames take it is load-bearing.
  // The LIFT — how far a shadowed patch is lifted toward TUNE_SHADOW_LIFT by
  // its blocker's distance — is a smooth, slowly-varying quantity, and there
  // are no spare bits to run a second 16-sample window for it. So it is
  // refreshed from ONE deterministic ray: the undeflected one, on the frame
  // this patch's window wraps, plus immediately on a fresh slot so that a newly
  // visible patch never spends a frame at the reset value of 0 (which is
  // contact black). A single fixed ray means the byte settles and STAYS, which
  // is what keeps a static scene's published value bit-stable frame to frame.
  let centre = fresh || slotIdx == 0u;

  // ---- A FRESH PATCH CASTS ITS WHOLE WINDOW (2026-10-13) ------------------
  // (ticket 2026-10-13_09-57-09: "very thin 1-pixel circles and lines ... they
  // only appear while I'm moving forward".) A patch whose window starts empty
  // used to publish its centre ray alone -- 0 or 1 -- and gain one cone sample
  // per resolve, i.e. one per SHADOW_REFRESH_PERIOD frames: 64 frames, a
  // second, before its penumbra was its penumbra. Standing still nothing is
  // fresh. Walking, whole ROWS of patches are: the reader pools sub-pixel
  // patches by powers of two (raymarch.wgsl SUB-PIXEL PATCHES ARE POOLED), so
  // where the ground crosses a pool step as it comes toward the eye it needs
  // the finer patches nobody had read, and each such boundary -- a curve of
  // constant projected patch size round the camera -- dragged a band of
  // half-estimated, high-contrast patches across every soft shadow. That was
  // the moving circle under the house. So the FIRST resolve of a window
  // publishes a FULL window; every later resolve refreshes one sample as
  // before, the same direction into the same bit, so a static scene still
  // settles bit-exactly. It is cheap where it can be: the centre ray and two
  // rim samples ~190 degrees apart round the disc (sunflower 15 and 11) go
  // first, and when all three agree the patch is outside every penumbra --
  // the common case: open ground, a wall in full shadow -- and the other
  // thirteen bits take their verdict (each is re-cast for real on its own
  // turn, so a thin blocker the three missed costs a sample's worth of error
  // for under a second). Only a patch the three disagree on, i.e. one IN a
  // penumbra, casts all sixteen. Measured at the ticket camera walking
  // (1324x1128): shadow_resolve mean 0.25 ms a frame before, 0.55 with all
  // sixteen on every fresh patch, see DESIGN.md 9.s for the shipped number.
  // The rays are the workgroup's (THE HEAVY RAYS ARE SHARED): a valid window
  // queues its one sample, a fresh one its centre and probes, and resolveStep
  // takes it from there.
  var st : ReqSt;
  st.hist = hist;
  st.fill = fill;
  st.flags = select(0u, ST_F_FRESH, fresh) | select(0u, ST_F_WASFULL, wasFull);
  st.old0 = old0;
  st.bucket = bucket;
  if (fresh && SHADOW_CONE_TAN > 0.0) {
    st.stage = ST_FRESH;
  } else {
    st.stage = ST_SINGLE;
    st.aux = slotIdx | select(0u, 16u, centre);
  }
  return st;
}

// One near window sample, cast by any lane for its owner `o`: the ray the
// owner's loop cast (same origin, same direction), its bit into wgSun, and --
// from the centre / burial ray -- the lift and the burial into wgRay0.
fn nearRayTask(t : u32, o : u32, packedCell : u32, packedSub : u32) {
  let cell = shadowUnwrapCell(packedCell, R.origin * i32(CHUNK));
  let face = (packedCell >> (WORLD_SHIFT * 3u)) & 7u;
  let hp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                             R.shadowSubdiv);
  let n3 = shadowFaceNormal(face);
  // shadowCoarseFromT() is `coarseFromT` (W2-B): past that distance the march
  // terminates on the 4^3 blockers mask instead of the voxel. It MUST be the
  // same expression sunShadowAt passes — the gate casts this ray and that one
  // at the same surface point and asserts they agree — which is why it is a
  // function in common.wgsl and not a knob read twice. The two pointers are
  // how a function in common.wgsl reaches bindings declared after it (see
  // traceOpaque).
  let ro = hp + n3 * TUNE_SHADOW_BIAS;
  let rot = f32(shadowPatchKey(packedCell, packedSub) & 0xFFFFu) * (1.0 / 65536.0);
  let L = keyLightDirP(R);
  // The sample this ray is (the task word: stageTask), and whether it is the
  // undeflected centre ray.
  let idx = (t >> 5u) & 15u;
  let isCentre = (t & TASK_CENTRE) != 0u;
  let dir = select(shadowConeDir(L, idx, rot), L, isCentre);
  let s = traceOpaque(ro, dir, TUNE_SHADOW_STEPS, shadowCoarseFromT(),
                      &occupancy, &materials);
  // A BURIED PATCH HAS NO OPINION. The ray starts TUNE_SHADOW_BIAS off the
  // face; a hit within a twentieth of a voxel of that means the cell in front
  // of the patch is itself solid — the patch is not a surface anyone can see,
  // it is the top face of the block UNDER a terrace step, or the side face
  // of one INSIDE the hill. No pixel is ever on such a patch, but the reader's
  // bilinear taps roll into the neighbour cell's same face past a face edge
  // (raymarch.wgsl shadowCached), and at every step of a terraced hillside
  // that neighbour is buried. Resolving it to a contact-black value painted a
  // one-pixel dark line along every voxel edge of the terrain, sun or no sun
  // — --gate shadow-cache's walk arm counted them as ~15-21k "phantom" pixels
  // a frame, every one on a step edge. Marking the slot INVALID instead makes
  // the tap drop out of the blend (weight 0) and the pixel shade from its own
  // face's patches, which is the ordinary patch-quantised edge and nothing
  // more. The slot stays claimed and live, so it is re-requested each frame
  // (one ray that terminates in its first cell) and never reclaimed by a
  // foreign patch mid-frame.
  //
  // A HIT ON THE PATCH'S OWN VOXEL IS NOT BURIAL, IT IS SELF-SHADOW (2026-10-13,
  // ticket 2026-10-13_09-55-24: "at this specific time of day, shadows have a
  // bunch of noise on them"). The near shade asks the cache for every face the
  // WRAPPED diffuse lights (render.diffuseWrap 1: every face but the exact
  // anti-sun one), so most patches on the dark side of a house are faces the
  // sun is BEHIND. Their ray turns into the face and meets the patch's own
  // voxel at t = shadowBias / -dot(n, dir): 0.05 or more while dot(n, L) is
  // above -0.4, a valid contact shadow; under it, this test called the patch
  // buried, and a pixel whose taps are all buried shades as FULL SUN. At the
  // hour a wall's dot(n, L) sat at -0.4 the cone's sixteen samples (+-1 deg)
  // fell either side of the line, so the wall flickered between its shadow
  // and full sun patch by patch on a 64-frame cycle (the window), and further
  // into the evening the whole wall was sunlit from behind. The cell in front
  // of the patch is what burial means; the patch's own cell is a blocker at
  // contact distance, which is exactly what sunShadowAt (the cache-off
  // reference) answers for the same ray.
  let selfHit = s.hit && all(s.cell == cell);
  // The ray the burial test reads: a valid window's one sample, a fresh
  // window's centre (TASK_BURIEDRAY).
  let buried = (t & TASK_BURIEDRAY) != 0u && s.hit && s.t < 0.05 && !selfHit;

  // ---- fold this frame's verdict into the window ----
  // `lit` is the sample's bit. RAN OUT OF STEPS counts as OCCLUDED, not as
  // daylight: leaving the window is sky, but exhausting the budget underground
  // is a blocker somewhere past it, and "no hit = lit" put full sun on the
  // floor of any cave deeper than the budget reaches along the sun (2026-09-02,
  // with block termination off: 384 fine steps = 38 m). It takes the FAR
  // blocker's lift below, which the openness cap at the reader turns into
  // nothing inside a cave and into the usual soft lift outdoors. Same rule in
  // sunShadowAt (raymarch.wgsl); the gate compares them.
  let ranOut = !s.hit && s.steps > u32(TUNE_SHADOW_STEPS);
  // ---- past the window face (CASTERS OUTSIDE THE WINDOW, above) ----
  // The near ray left the window clear; the cascade decides the rest. `bHit`
  // and `bT` are the verdict and blocker distance everything below reads --
  // the buried test above stays on the near ray alone, since a far blocker is
  // by construction never within a twentieth of a voxel of the patch.
  var bHit = s.hit;
  var bT = s.t;
  if (!s.hit && !ranOut) {
    let tw = shadowWindowExitT(ro, dir);
    let reachF = TUNE_FAR_SHADOW_REACH / VOXEL_METERS;
    if (tw < reachF) {
      let ft = farShadowT(1u, ro, dir, tw);
      if (ft >= 0.0) { bHit = true; bT = ft; }
    }
    // A CLEAR RAY UNDER THE TERRAIN IS NOT SUNLIT (above): asked where the
    // ray stopped being marched -- the reach, or the window's face for a ray
    // that left it past the reach.
    if (!bHit && farUnderTerrain(ro, dir, max(tw, reachF))) {
      bHit = true;
      bT = max(tw, reachF);
    }
  }
  let sunSeen = !bHit && !ranOut;
  // The sample's bit, for its owner (resolveStep folds it into the window).
  if (sunSeen) { atomicOr(&wgSun[o], 1u << idx); }

  // The lift this ray saw, if it is the one that owns the byte. The law is
  // sunShadowAt's, verbatim, and must stay that way: the depth of a shadow is
  // taken from how far the ray travelled before being blocked, so a contact
  // shadow stays dark and a distant blocker's shadow lifts.
  var w = select(0u, 1u, buried);
  if (isCentre) {
    var lv = 0.0;
    if (bHit) {
      let dM = bT * VOXEL_METERS;
      lv = clamp(smoothstep(TUNE_SHADOW_SOFT_NEAR, TUNE_SHADOW_SOFT_FAR, dM) *
                 TUNE_SHADOW_LIFT, 0.0, 1.0);
    } else if (ranOut) {
      lv = TUNE_SHADOW_LIFT;
    } else {
      // The centre ray is clear, so nothing is blocking the middle of the
      // disc and only its rim can be clipped. Such a blocker is by
      // construction a grazing one; lifting its sliver of shadow all the way
      // to the far value would brighten the outer half of every penumbra.
      // Contact-dark is the conservative end and the one that keeps an edge
      // reading as an edge.
      lv = 0.0;
    }
    w |= (u32(lv * 255.0 + 0.5) << 8u) | 0x10000u;
  }
  // Burial and the lift for the owner: only the ray that owns them writes.
  if ((t & (TASK_BURIEDRAY | TASK_CENTRE)) != 0u) { wgRay0[o] = w; }
}

// An owner's turn after a round: fold the round's rays into its window and
// either finish or queue the next ones. The order is the one the owner's own
// loop had (A FRESH PATCH CASTS ITS WHOLE WINDOW): centre + probes; three that
// agree fill the window with their verdict; otherwise the other thirteen.
fn resolveStep(stIn : ReqSt, lane : u32, r : u32) -> ReqSt {
  var st = stIn;
  if (st.stage == ST_FAR) {
    farFinish(lane, st.bucket);
    st.stage = ST_DONE;
    return st;
  }
  let sun = atomicLoad(&wgSun[lane]);
  let ray0 = wgRay0[lane];
  var done = false;
  switch (st.stage) {
    case ST_SINGLE: {
      let bit = 1u << (st.aux & 15u);
      st.hist = (st.hist & ~bit) | (sun & bit);
      st.fill = min(st.fill + 1u, SHADOW_SAMPLES);
      done = true;
    }
    case ST_FRESH: {
      st.hist = (st.hist & ~SC_PROBE_BITS) | (sun & SC_PROBE_BITS);
      st.fill = min(st.fill + 3u, SHADOW_SAMPLES);
      let seen = countOneBits(sun & SC_PROBE_BITS);
      if (seen == 0u || seen == 3u) {
        st.hist = select(st.hist & ~SHADOW_HIST_MASK, st.hist | SHADOW_HIST_MASK, seen == 3u);
        st.fill = SHADOW_SAMPLES;
        done = true;
      } else {
        st.stage = ST_FULL;
      }
    }
    default: {
      // ST_FULL: the thirteen.
      let m13 = SHADOW_HIST_MASK & ~SC_PROBE_BITS;
      st.hist = (st.hist & ~m13) | (sun & m13);
      st.fill = min(st.fill + 13u, SHADOW_SAMPLES);
      done = true;
    }
  }
  // The centre / burial ray of this request was in the first round, which every
  // stage above has seen by now; ray0 holds its burial and, from a centre, the
  // lift.
  if (done) {
    if ((ray0 & 0x10000u) != 0u) {
      st.hist = (st.hist & ~(0xFFu << SHADOW_LIFT_SHIFT)) | (((ray0 >> 8u) & 0xFFu) << SHADOW_LIFT_SHIFT);
    }
    if ((ray0 & 1u) != 0u) { st.flags |= ST_F_BURIED; }
    nearFinish(st, r);
    st.stage = ST_DONE;
  }
  return st;
}

// The window's published value, its deposit and the publish (the owner's).
fn nearFinish(st : ReqSt, r : u32) {
  let key = reqWord(r, 0u);
  let bucket = st.bucket;
  let packedCell = reqWord(r, 2u);
  let packedSub = reqWord(r, 3u);
  let cell = shadowUnwrapCell(packedCell, R.origin * i32(CHUNK));
  let face = (packedCell >> (WORLD_SHIFT * 3u)) & 7u;
  let n3 = shadowFaceNormal(face);
  let ver = shadowPatchVerifier(packedCell, packedSub);
  let fresh = (st.flags & ST_F_FRESH) != 0u;
  let wasFull = (st.flags & ST_F_WASFULL) != 0u;
  let buried = (st.flags & ST_F_BURIED) != 0u;
  let old0 = st.old0;
  let fill = st.fill;
  let liftB = (st.hist >> SHADOW_LIFT_SHIFT) & 0xFFu;
  var hist = st.hist;
  hist = (hist & SHADOW_HIST_MASK) | (fill << SHADOW_FILL_SHIFT) |
         (liftB << SHADOW_LIFT_SHIFT);

  // ---- the published value ----
  // `open` is the fraction of the window that saw the sun. The rest of the disc
  // is blocked, and what survives there is the lift — so the two compose as
  // `open + (1 - open) * lift`, which degenerates EXACTLY to the pre-cone
  // behaviour at a zero cone angle: every sample agrees, `open` is 0 or 1, and
  // the value is the lift or full sun. Unfilled slots read as 0 bits, which is
  // why the divisor is `fill` and not SHADOW_SAMPLES: a patch resolved for the
  // first time publishes its one centre ray's answer, same as it always did.
  let openF = f32(countOneBits(hist & SHADOW_HIST_MASK)) / f32(max(fill, 1u));
  let liftF = f32(liftB) * (1.0 / 255.0);
  let aim = clamp(openF + (1.0 - openF) * liftF, 0.0, 1.0);
  // ---- the glide (see THE WINDOW HAS SEVENTEEN LEVELS, above) ----
  // Integer units of the stored byte throughout, so the fixed point is exact.
  var valU = i32(aim * 255.0 + 0.5);   // `aim` not `target`: reserved word
  // SHADOW_GLIDE = 0 is the differential oracle: it takes the window's answer
  // whole, which is the pre-2026-09-12 behaviour, and `--gate shadow-cache`'s
  // creep arm goes from 0.8% of moves jumping to 99.2% — and from 43,928
  // slot-frames moving at all to 7,146, because moving a LITTLE EVERY FRAME
  // rather than a lot occasionally is precisely what the fix is. One WGSL
  // const, no rebuild: that is the whole A/B.
  if (SHADOW_GLIDE > 0.0 && SHADOW_CONE_TAN > 0.0 && wasFull && !fresh) {
    let prevU = i32(old0 & 0xFFu);
    let d = valU - prevU;
    if (d != 0 && abs(d) < SHADOW_GLIDE_SNAP) {
      var s = i32(round(f32(d) * SHADOW_GLIDE));
      // At least one unit toward the target — a rate that rounds to zero would
      // park the byte a quantum short and never arrive.
      if (s == 0) { s = select(-1, 1, d > 0); }
      if (abs(s) > abs(d)) { s = d; }   // never overshoot
      valU = prevU + s;
    }
  }
  let v = f32(valU) * (1.0 / 255.0);
  // Written UNGUARDED, unlike the publish below. If the slot has changed hands
  // since the request was queued, the value write is dropped but this one is
  // not — and that is harmless by construction: the new owner's verifier will
  // not match `old0` next frame, so it reads `fresh` and starts the window from
  // zero. Guarding it would cost a second atomic load to learn nothing.
  shadowHist[bucket] = hist;

  // ---- P1 direct injection (docs/PLAN_gi.md §3) ----
  // The patch is a lit (or shadowed) piece of a real surface and this pass is
  // the one place that knows its cell, its face and its shadow term at once, so
  // it deposits albedo × sun × lambert × lit into the block-face word here —
  // one voxel-word read on top of the ray. Not guarded by the cache publish
  // below: a slot that changed hands is a cache-identity problem and the light
  // is still real. Buried patches (the cell in front is solid) deposit nothing
  // — no surface anyone sees, and a bilinear reader would never ask. Racy
  // against the other patches of the same face by design; see irrDeposit.
  if (TUNE_GI_STRENGTH > 0.0 && !buried) {
    let pw = voxWordAt(cell);
    let pm = materials[voxMat(pw)];
    // Burning foliage deposits the MEAN of its breath, which is what
    // sim_openness.wgsl's walk deposits into the SAME word -- the two writers
    // of one value have to agree, and this pass is the loud one (every frame,
    // against once per sweep). Depositing the raw palette instead put a
    // burning crown's leaf-green into the grid at full emission and lit
    // everything under the tree green (owner report 2026-09-03).
    let bt = burnTint(pm, paletteColor(pm, voxState(pw), &materials),
                      f32(pm.emission) / 255.0, burnTintMean());
    let ob = opennessByteAt(cell, face, &openness, &opennessGen);
    let lit = shadowLiftCap(v, select(-1.0, f32(ob) * (1.0 / OPEN_MAX), ob >= 0));
    let sample = irrSample(bt.albedo, n3, keyLightDirP(R), keyLightColorP(R),
                           lit, bt.emis);
    let stampOk = opennessGen[chunkIndexW(cell)] == opennessStamp(worldChunkOf(cell));
    // x PERIOD: a patch deposits once per SHADOW_REFRESH_PERIOD frames now, so
    // the per-deposit rate is scaled to keep the grid's convergence per FRAME
    // what it was when every patch deposited every frame.
    irrDeposit(irrIndexOfCell(cell, face), sample,
               min(GI_RESOLVE_ALPHA * f32(SHADOW_REFRESH_PERIOD), 1.0), stampOk,
               &irradiance);
  }

  // Publish, guarded on the slot still being OURS — key AND verifier. A slot
  // can only change hands once it has gone stale (raymarch.wgsl shadowSlotRead
  // never steals a live one), but a duplicate claim or a set that turned over
  // in the frame since this request was queued would otherwise stamp our value
  // under another patch's identity — precisely the "wrong shadow" the blend's
  // zero weight exists to avoid. Losing the write instead costs this patch one
  // frame with no opinion. `requested` and the verifier ride through unchanged.
  // `ver` is computed at the top of the pass — the window above needs it to
  // tell "my slot, carry the samples" from "somebody else's, start over".
  if (atomicLoad(&shadowCache[bucket * 2u]) != key) { return; }
  let old = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  if (shadowStateVerifier(old) != ver) { return; }
  // `valU` and not `u32(v * 255.0 + 0.5)`: the byte IS the state the glide
  // above iterates on, and a round trip through a float is the one place a
  // fixed point could pick up a unit of drift.
  atomicStore(&shadowCache[bucket * 2u + 1u],
              shadowPackState(u32(valU), R.frameIdx & 15u,
                              shadowStateRequested(old), !buried, ver));
}
