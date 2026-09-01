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
// THE RAY CAST HERE IS NOT trace(). trace() lives in raymarch.wgsl and is not
// reachable from a compute shader; moving it into common.wgsl would prepend
// 800 lines to all 17 shaders. shadowMarch below is a purpose-built MEDIA-BLIND
// DDA instead — no media accumulation, no water surface, no micro bricks, no
// reflection — which is also why it is cheaper per ray than trace(..., false)
// was: it carries none of those registers.
//
// TWO DDAs THAT MUST AGREE IS THE CLASSIC SILENT BUG, so it is not left to a
// comment: `--selftest --gate shadow-cache` casts both at sampled surface
// points and asserts they return the same answer.
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

// ---------------------------------------------------------------- march ----

struct ShadowHit {
  hit : bool,
  t   : f32,   // fine-voxel units, distance to the blocker
}

// The media-blind march. Structurally the same DDA as trace()'s — same window
// clip, same per-chunk lookup cache, same chunk-skip jump geometry — with every
// branch a media-aware ray needs deleted.
//
// WHAT COUNTS AS A HIT, and why it is exactly isRayBlocker(): walking trace()'s
// per-cell chain for wantMedia = false, a gas or translucent liquid takes the
// participating-media branch and accumulates nothing; a micro cell falls
// through explicitly (raymarch.wgsl, "shadow / reflection ray meets a micro
// cell"); everything else hits — INCLUDING translucent solids, because ice
// casts a solid shadow and the sim's seesSky already believes that. Solid,
// powder, opaque liquid, no micro: that is isRayBlocker's definition verbatim,
// which is the same equivalence the sub-chunk occupancy block in common.wgsl
// relies on.
fn shadowMarch(ro : vec3f, rdIn : vec3f, maxSteps : i32) -> ShadowHit {
  var out : ShadowHit;
  out.hit = false;
  out.t = 0.0;

  // Axis-aligned rays would divide by zero and produce inf tMax. Same nudge
  // trace() applies, and it must stay identical or the two disagree on rays
  // exactly along an axis — which is the common case here, since the sun is
  // often near-vertical.
  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // Clip to the residency window AABB, in world coords.
  //
  // NO LOD HANDOFF CLAMP HERE, deliberately. trace() shortens tExit to
  // TUNE_LOD_HANDOFF_DIST for media-AWARE rays only, and the comment there
  // says why a shadow ray must be excluded: one that gave up at 18 m would
  // report "lit" for a receiver whose blocker is at 20 m, which unshadows
  // terrain rather than coarsening it.
  let nf = f32(WORLD_N);
  let wloI = R.origin * i32(CHUNK);
  let wlo = vec3f(wloI);
  let tt0 = (wlo - ro) * inv;
  let tt1 = (wlo + vec3f(nf) - ro) * inv;
  let tmin = min(tt0, tt1);
  let tmax = max(tt0, tt1);
  let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
  let tExit = min(tmax.x, min(tmax.y, tmax.z));
  if (tExit <= tEnter) { return out; }

  var t = tEnter + 1e-4;
  var p = ro + rd * t;
  var cell = clamp(vec3<i32>(floor(p)), wloI, wloI + vec3<i32>(i32(WORLD_N) - 1));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = (boundary - ro[a]) * inv[a];
  }
  var tCur = t;

  let wloHi = wloI + vec3<i32>(i32(WORLD_N));
  // Per-chunk lookup cache, same as trace()'s: occupancy and the page entry are
  // invariant while the ray stays in a chunk, which is 16-48 steps. Compared on
  // the CHUNK COORD rather than the linear index so the miss test is three
  // shifts and a vec3 compare instead of chunkIndexW's multiplies.
  var cchOcc = 0u;
  var cchPt = 0u;
  var cchC = vec3<i32>(0x7FFFFFFF);

  for (var i = 0; i < 4096; i++) {
    if (i >= maxSteps) { break; }
    if (any(cell < wloI) || any(cell >= wloHi)) { break; }

    let cc = cell >> vec3<u32>(CHUNK_SHIFT);
    if (any(cc != cchC)) {
      cchC = cc;
      let chIdx = chunkIndexW(cell);
      cchOcc = occupancy[chIdx];
      cchPt = pageEntryOf(chIdx);
    }

    // Chunk skip on the BLOCKER count, never the total: a chunk holding only
    // smoke or grass must be as cheap as air for this ray class.
    var chunkSkip = (occBlockers(cchOcc) == 0u);
    if ((cchPt & PT_SENTINEL_BIT) != 0u) {
      let sMat = cchPt & PT_MAT_MASK;
      if (sMat == MAT_AIR) {
        chunkSkip = true;
      } else if (isRayBlocker(materials[sMat])) {
        // A uniform body of blocker material: this cell IS that material, so
        // the answer is here. No isTranslucentSolid exclusion, unlike the
        // primary ray's version of this branch in raymarch.wgsl — that
        // exclusion exists so a PRIMARY ray keeps marching through glass to
        // accumulate its Beer-Lambert path, and a shadow ray must stop.
        out.hit = true;
        out.t = tCur;
        return out;
      } else if ((materials[sMat].flags & MATF_MICRO) != 0u) {
        chunkSkip = true;   // a whole chunk of grass casts no shadow
      }
    }

    if (chunkSkip) {
      // Jump to the chunk's exit face. Masking off the low bits is
      // floor-to-corner for negative world coords too.
      let blkLo = cell & vec3<i32>(~(i32(CHUNK) - 1));
      let lo = vec3f(blkLo);
      let hi = lo + f32(CHUNK);
      let e0 = (lo - ro) * inv;
      let e1 = (hi - ro) * inv;
      let ex = max(e0, e1);
      // max against tCur: a cell floored onto a shared face belongs to a box
      // the ray is already exiting, so the raw exit t can be <= tCur and the
      // march would stall in place.
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      t = tOut + 1e-4;
      if (t >= tExit) { break; }
      p = ro + rd * t;
      var nc = vec3<i32>(floor(p));
      // Force the crossing on the exit axis: float noise at a shared face can
      // floor() back into the box just left, which reads as a shadow leak
      // along chunk boundaries.
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(blkLo.x - 1, blkLo.x + i32(CHUNK), rd.x > 0.0);
      } else if (ex.y <= ex.z) {
        nc.y = select(blkLo.y - 1, blkLo.y + i32(CHUNK), rd.y > 0.0);
      } else {
        nc.z = select(blkLo.z - 1, blkLo.z + i32(CHUNK), rd.z > 0.0);
      }
      if (any(nc < wloI) || any(nc >= wloHi)) { break; }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - ro[a]) * inv[a];
      }
      tCur = t;
      continue;
    }

    let w = voxWordAtEntry(cchPt, cell);
    let mat = voxMat(w);
    if (mat != MAT_AIR && isRayBlocker(materials[mat])) {
      out.hit = true;
      out.t = tCur;
      return out;
    }

    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z;
    }
  }
  return out;
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

  let s = shadowMarch(hp + n3 * TUNE_SHADOW_BIAS, keyLightDirP(R),
                      TUNE_SHADOW_STEPS);
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
