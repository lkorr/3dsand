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

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(5) var<storage, read_write> shadowCache : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> shadowReq : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> shadowArgs : array<u32>;

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
  shadowArgs[0] = (n + 63u) / 64u;
  shadowArgs[1] = 1u;
  shadowArgs[2] = 1u;
}

// resolve: one media-blind shadow ray per requested patch.
@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = atomicLoad(&shadowReq[1]);
  if (gid.x >= n) { return; }

  let base = SHADOW_REQ_HEADER + gid.x * SHADOW_REQ_WORDS;
  let key = atomicLoad(&shadowReq[base]);
  let bucket = atomicLoad(&shadowReq[base + 1u]);
  let packedCell = atomicLoad(&shadowReq[base + 2u]);
  let packedSub = atomicLoad(&shadowReq[base + 3u]);

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

  // 1e30 is `coarseFromT`: accepted and ignored until W2-B teaches the march
  // to terminate on the 4^3 blocker mask past a distance. This pass will be the
  // first caller to pass a real one. The two pointers are how a function in
  // common.wgsl reaches bindings declared after it (see traceOpaque).
  let s = traceOpaque(hp + n3 * TUNE_SHADOW_BIAS, keyLightDirP(R),
                      TUNE_SHADOW_STEPS, 1e30, &occupancy, &materials);
  // The softening law is sunShadowAt's, verbatim, and must stay that way: the
  // penumbra is taken from how far the ray travelled before being blocked, so a
  // contact shadow stays crisp and a distant blocker's shadow lifts.
  var v = 1.0;
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
  let buried = s.hit && s.t < 0.05;
  if (s.hit) {
    let dM = s.t * VOXEL_METERS;
    v = clamp(smoothstep(TUNE_SHADOW_SOFT_NEAR, TUNE_SHADOW_SOFT_FAR, dM) *
              TUNE_SHADOW_LIFT, 0.0, 1.0);
  }

  // Publish, guarded on the slot still being OURS — key AND verifier. A slot
  // can only change hands once it has gone stale (raymarch.wgsl shadowSlotRead
  // never steals a live one), but a duplicate claim or a set that turned over
  // in the frame since this request was queued would otherwise stamp our value
  // under another patch's identity — precisely the "wrong shadow" the blend's
  // zero weight exists to avoid. Losing the write instead costs this patch one
  // frame with no opinion. `requested` and the verifier ride through unchanged.
  let ver = shadowPatchVerifier(packedCell, packedSub);
  if (atomicLoad(&shadowCache[bucket * 2u]) != key) { return; }
  let old = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  if (shadowStateVerifier(old) != ver) { return; }
  atomicStore(&shadowCache[bucket * 2u + 1u],
              shadowPackState(u32(v * 255.0 + 0.5), R.frameIdx & 15u,
                              shadowStateRequested(old), !buried, ver));
}
