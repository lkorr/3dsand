// ray_start.wgsl — THE RAY-START MAP (2026-09-28, raymarch perf round 2).
// Two per-frame compute entries on the shadow layout (pass_table.def
// ray_start_trace / ray_start_min, the ShadowCache table), read by
// raymarch.wgsl's fs() through rayStartAt().
//
// ---- WHAT IT BUYS ----------------------------------------------------------
// Measured on main 6c0638f (--render-budget, 1080p, RTX 3060 Ti): the primary
// ray's TRAVERSAL is 54-82% of the raymarch frame, and most of it is the
// approach — the fine march crossing air and grass-free chunks to the first
// thing it can hit, then the far march walking ~30 cascade cells and ~16 level
// chunks per pixel before its surface (noon: 20 primary + 47 far steps a
// pixel). Every one of those steps is spent again by the neighbouring pixel,
// because at 1080p a fine voxel inside the LOD handoff is >= 3.8 px wide and a
// cascade cell is >= 3.5 px wide at every level (the kFarN law): adjacent rays
// walk the same empty cells to reach the same surface.
//
// This pass marches ONE ray per 2x2 pixel block (quarter the rays) to its
// first cell that could stop it — any fine voxel that is not air (gas, liquid
// and a plant cell included), or any cascade cell traceFar could report a hit
// in (rayStartFarCandidate) — and a
// 5x5 min filter turns that into a start distance every pixel of the block may
// skip to. With the start taken from the exact per-pixel answer (the upper
// bound, emulated through the water-veil buffer on a static camera) noon went
// 8.20 -> 5.16 ms and the cascade camera 5.36 -> 2.95; the min-filtered form
// cut the per-pixel steps 20 -> 4.5 (near) and 47 -> 10.5 (far) with every
// camera's picture at the run-to-run noise floor.
//
// ---- WHY IT IS CONSERVATIVE (the whole correctness argument) ----------------
// A pixel may skip to t0 only if NOTHING its own ray could hit lies before t0.
// The samples sit on a 2-px lattice. The first cell C a pixel's ray meets
// projects to a convex region of the screen at least `c` px across (the
// inscribed sphere of the cube), with c >= 3 px guaranteed by rayStartLawOk()
// (below it: the WIDE TIER, see THE LAW further down — a staggered lattice and
// a 7x7 min that hold the same argument down to 2.5 px, the fine half capped
// per pixel). A convex region that wide, containing the pixel, holds
// a lattice point within ~3.5 px of it, and a sample through that point meets
// C or something NEARER on its own ray. The 5x5 window reaches >= 3.5 px in
// every direction, so its minimum is <= the entry t of C into the SAMPLE's
// ray; the pixel's entry into the same cube differs from that by at most the
// cube's diagonal, which the reader's margin covers (RS_NEAR_ABS / RS_FAR_REL).
// Every approximation in this file errs EARLY: the marches stop on every cell
// the fs march COULD stop in (so a refine that would have let the ray through,
// a faded plant, a gas cell the shade would not draw — all count), the cascade
// seams are taken at the dither's widest overlap, and a march that runs out of
// budget reports where it stopped, never "nothing".
//
// ---- VALIDITY ---------------------------------------------------------------
// Word 0 of the buffer is a key over this frame's camera, size and frame index
// (rayStartKey, which raymarch.wgsl carries a byte-identical copy of). The
// reader recomputes it from its own RenderParams and ignores the map on any
// mismatch — a first frame, a resize, a render pass whose EncodeShadowResolve
// did not run, a second view drawn with other params. The failure mode of
// every one of those is the old full march, never a skipped surface.
//
// Render-only derived data. Nothing is hashed; nothing is saved.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(18) var<storage, read> farVox : array<u32>;
@group(0) @binding(19) var<storage, read> farOcc : array<u32>;
@group(0) @binding(20) var<uniform> F : FarParams;
@group(0) @binding(21) var<storage, read_write> rayStart : array<u32>;

// ---- layout (raymarch.wgsl rayStartAt reads the same) ----------------------
// [0] key   [1..7] unused   then TWO planes of Wq x Hq f32 words:
// the raw first-hit t per sample (plane A, this pass's output) and the 5x5
// min of it (plane B, what fs() reads). Wq/Hq = the target's half size, rounded
// up, derived from RenderParams on both sides.
const RS_HEADER : u32 = 8u;            // raymarch.wgsl agrees
const RS_NONE : f32 = 1e30;            // "no sample saw anything"; raymarch.wgsl agrees
const RS_MIN_RADIUS : i32 = 2;         // the 5x5 window (+1 in the wide tier)

fn rayStartDims() -> vec2<u32> {
  return vec2<u32>(u32(round(R.viewPx * R.aspect)), u32(round(R.viewPx)));
}
// raymarch.wgsl carries a byte-identical copy (it cannot import this module).
// A disagreement would only ever turn the map OFF — never make it lie.
fn rayStartKey() -> u32 {
  let d = rayStartDims();
  var h = pcg(bitcast<u32>(R.camPos.x) ^ 0x52415953u);
  h = pcg(h ^ bitcast<u32>(R.camPos.y));
  h = pcg(h ^ bitcast<u32>(R.camPos.z));
  h = pcg(h ^ bitcast<u32>(R.camFwd.x));
  h = pcg(h ^ bitcast<u32>(R.camFwd.y));
  h = pcg(h ^ bitcast<u32>(R.camFwd.z));
  h = pcg(h ^ bitcast<u32>(R.camRight.x));
  h = pcg(h ^ bitcast<u32>(R.camRight.z));
  h = pcg(h ^ bitcast<u32>(R.tanHalfFov));
  h = pcg(h ^ R.frameIdx);
  h = pcg(h ^ (d.x | (d.y << 16u)) ^ bitcast<u32>(R.origin.x) ^
          (bitcast<u32>(R.origin.z) << 7u) ^ (bitcast<u32>(R.origin.y) << 14u));
  return h | 1u;   // never 0: a zeroed buffer can never match
}

// ---- the camera ray through screen position `px` (pixels, top-left origin) --
// fs() builds its ray from the interpolated clip position; the viewport is
// y-flipped (vk_record.cpp: negative height), so clip y = 1 - 2 py / H.
fn rayStartDir(px : vec2f, dims : vec2<u32>) -> vec3f {
  let ndc = vec2f(px.x / f32(dims.x) * 2.0 - 1.0, 1.0 - px.y / f32(dims.y) * 2.0);
  return normalize(R.camFwd + R.camRight * (ndc.x * R.tanHalfFov * R.aspect) +
                   R.camUp * (ndc.y * R.tanHalfFov));
}

// ============================ THE FINE HALF =================================
// trace()'s window clip, LOD handoff and short-range ceiling, verbatim: the
// fine march must cover everything trace() could reach, and the cascade half
// must start no later than fs() hands traceFar its start (h.tExit).
struct RsSpan { tEnter : f32, tExit : f32 };
fn rayStartSpan(ro : vec3f, inv : vec3f) -> RsSpan {
  let wlo = vec3f(R.origin * i32(CHUNK));
  let tt0 = (wlo - ro) * inv;
  let tt1 = (wlo + vec3f(f32(WORLD_N)) - ro) * inv;
  let tmin = min(tt0, tt1);
  let tmax = max(tt0, tt1);
  var s : RsSpan;
  s.tEnter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
  s.tExit = min(tmax.x, min(tmax.y, tmax.z));
  if (TUNE_LOD_HANDOFF_DIST < WINDOW_HALF_EXTENT_METERS) {
    s.tExit = min(s.tExit, max(s.tEnter, TUNE_LOD_HANDOFF_DIST / VOXEL_METERS));
  }
  if ((R.flags & 4u) != 0u) {
    // shortRangeCeilM() (raymarch.wgsl), restated: bit 4 picks the near arm.
    let ceilM = select(TUNE_SHORT_RANGE_DIST, TUNE_SHORT_RANGE_NEAR_DIST,
                       (R.flags & 16u) != 0u);
    s.tExit = min(s.tExit, max(s.tEnter, ceilM / VOXEL_METERS));
  }
  return s;
}

// The first fine cell holding ANY material along [tEnter, tExit], or -1. The
// skips are trace()'s own for a media-aware ray: an empty chunk (total count
// 0, or an AIR sentinel) and an empty 4^3 block of the TOTAL class. A
// non-air sentinel chunk is a hit at its entry — every cell in it is that
// material. Out of budget reports where it stopped: nothing was found before
// it, which is all the reader needs.
fn rayStartFine(ro : vec3f, rd : vec3f, inv : vec3f, sp : RsSpan) -> f32 {
  if (sp.tExit <= sp.tEnter) { return -1.0; }
  let wloI = R.origin * i32(CHUNK);
  let wloHi = wloI + vec3<i32>(i32(WORLD_N));
  var tCur = sp.tEnter + 1e-4;
  var cell = clamp(vec3<i32>(floor(ro + rd * tCur)), wloI, wloHi - vec3<i32>(1));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    tMax[a] = (f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0) - ro[a]) * inv[a];
  }
  var cchC = vec3<i32>(0x7FFFFFFF);
  var cchOcc = 0u;
  var cchPt = 0u;
  var cchM0 = 0u;
  var cchM1 = 0u;
  for (var i = 0; i < TUNE_PRIMARY_STEPS; i++) {
    if (any(cell < wloI) || any(cell >= wloHi) || tCur >= sp.tExit) { return -1.0; }
    let cc = cell >> vec3<u32>(CHUNK_SHIFT);
    if (any(cc != cchC)) {
      cchC = cc;
      let chIdx = chunkIndexW(cell);
      cchOcc = occupancy[chIdx];
      cchPt = pageEntryOf(chIdx);
      let mb = subOccIndex(chIdx, 0u, 0u);   // class 0 = TOTAL, as trace()
      cchM0 = occupancy[mb];
      cchM1 = occupancy[mb + 1u];
    }
    var skip = occTotal(cchOcc) == 0u;
    var blk = i32(CHUNK);
    if ((cchPt & PT_SENTINEL_BIT) != 0u) {
      if ((cchPt & PT_MAT_MASK) == MAT_AIR) { skip = true; }
      else { return tCur; }
    }
    if (!skip) {
      let sbit = subOccBitLocal(vec3<u32>(cell & vec3<i32>(CHUNK_MASK)));
      let mw = select(cchM1, cchM0, sbit < 32u);
      if ((mw & (1u << (sbit & 31u))) == 0u) {
        skip = true;
        blk = i32(SUBOCC_BLOCK);
      } else if (voxMat(voxWordAtEntry(cchPt, cell)) != MAT_AIR) {
        return tCur;
      }
    }
    if (skip) {
      // trace()'s box jump, including the forced crossing on the exit axis.
      let lo = cell & vec3<i32>(~(blk - 1));
      let e0 = (vec3f(lo) - ro) * inv;
      let e1 = (vec3f(lo) + f32(blk) - ro) * inv;
      let ex = max(e0, e1);
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      tCur = tOut + 1e-4;
      if (tCur >= sp.tExit) { return -1.0; }
      var nc = vec3<i32>(floor(ro + rd * tCur));
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(lo.x - 1, lo.x + blk, rd.x > 0.0);
      } else if (ex.y <= ex.z) {
        nc.y = select(lo.y - 1, lo.y + blk, rd.y > 0.0);
      } else {
        nc.z = select(lo.z - 1, lo.z + blk, rd.z > 0.0);
      }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        tMax[a] = (f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0) - ro[a]) * inv[a];
      }
      continue;
    }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z;
    }
  }
  return tCur;
}

// ============================ THE CASCADE HALF ===============================
// traceFar's level walk, reduced to "the first non-zero byte": any palette slot
// or the conservative blocker flag, at every level (the refine can only let a
// ray through a cell it entered, so the cell's entry is early enough). The
// seams take farDither's WIDEST pull-back (0.5), so every pixel's own levels
// start no earlier than these do. The sky clip and the level spheres are
// traceFar's (exact: what they cut off is air or another level's).
struct FarBox { lo : vec3<i32>, hi : vec3<i32> };
const FAR_FACE_BITS : u32 = 5u;          // world.h kFarFaceBits (check_invariants.py)
const FAR_FACE_MASK : u32 = 31u;         // world.h kFarFaceMax
const FAR_FACE_ALL  : u32 = 1u << 30u;   // world.h kFarFaceAllPending
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
const SKY_TOP_BASE : u32 = FAR_LEVELS * FAR_NUM_CHUNKS;   // sky_top.wgsl agrees
const SKY_TOP_BIAS : i32 = 1 << 24;                       // sky_top.wgsl agrees
fn farSkyCeil(level : u32) -> f32 {
  let w = farOcc[SKY_TOP_BASE + level - 1u];
  return select(f32(i32(w) - SKY_TOP_BIAS), -1e30, w == 0u);
}
const FAR_SPHERE_MARGIN_CHUNKS : i32 = 2;   // raymarch.wgsl, same name
const FAR_DITHER_MAX : f32 = 0.5;           // raymarch.wgsl farDither's ceiling

fn rayStartFarByte(level : u32, c : vec3<i32>) -> u32 {
  let bi = farVoxByteIndex(level, c);
  return (farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu;
}
// Every cell traceFar can report a hit IN, and no other: a material cell, or
// a flagged air cell that is either a refine candidate (material directly
// under it, at a level the refine runs at — the sub-columns can rise into it)
// or at a level where the flag itself is a hit (render.farBlockerHitLevel). NOT the bare flag: it also sits
// on the cover-height rows over the ground (worldgen farBlockerBand), and
// stopping there left the far march 30 of its 47 steps a pixel (measured).
fn rayStartFarCandidate(level : u32, c : vec3<i32>) -> bool {
  let b = rayStartFarByte(level, c);
  if (farCellSlot(b) != 0u) { return true; }
  if (b == 0u) { return false; }
  return i32(level) <= TUNE_FAR_BLOCKER_HIT_LEVEL ||
         (i32(level) <= TUNE_FAR_REFINE_LEVEL &&
          farCellSlot(rayStartFarByte(level, c - vec3<i32>(0, 1, 0))) != 0u);
}

fn rayStartFar(ro : vec3f, rd : vec3f, inv : vec3f, tStart : f32) -> f32 {
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  let tCeil = select(1e30,
                     select(TUNE_SHORT_RANGE_DIST, TUNE_SHORT_RANGE_NEAR_DIST,
                            (R.flags & 16u) != 0u) / VOXEL_METERS,
                     (R.flags & 4u) != 0u);
  var tPrev = max(tStart - FAR_DITHER_MAX * f32(1u << farCellShift(1u)), 0.0);
  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let s = f32(1u << farCellShift(level));
    let box = farBox(level);
    let roL = ro / s;
    let tt0 = (vec3f(box.lo) - roL) * inv;
    let tt1 = (vec3f(box.hi) - roL) * inv;
    let tmin = min(tt0, tt1);
    let tmax = max(tt0, tt1);
    var tEnter = max(max(tmin.x, tmin.y), max(tmin.z, tPrev / s));
    var tExit = min(min(tmax.x, min(tmax.y, tmax.z)), tCeil / s);
    if (level < FAR_LEVELS) {
      tExit = min(tExit, f32((i32(FAR_NCHUNK) / 2 - FAR_SPHERE_MARGIN_CHUNKS) *
                             i32(CHUNK)));
    }
    // farSkyClip, inlined.
    let ceil = farSkyCeil(level);
    let tPlane = (ceil - roL.y) * inv.y;
    if (rd.y > 0.0) { tExit = min(tExit, tPlane + 1e-3); }
    else if (roL.y >= ceil) { tEnter = max(tEnter, tPlane - 1e-3); }
    if (tExit <= tEnter) { continue; }

    var tCur = tEnter + 1e-4;
    var cc = worldChunkOf(clamp(vec3<i32>(floor(roL + rd * tCur)),
                                box.lo, box.hi - vec3<i32>(1)));
    var cNext : vec3f;
    for (var a = 0; a < 3; a++) {
      cNext[a] = (f32((cc[a] + select(0, 1, rd[a] > 0.0)) * i32(CHUNK)) - roL[a]) *
                 inv[a];
    }
    var tStop = tExit;
    var budget = TUNE_FAR_STEPS;
    while (true) {
      if (budget <= 0) { return tCur * s; }   // nothing before here: say so
      budget -= 1;
      if (!farInValid(cc * i32(CHUNK), box) || tCur >= tExit) { break; }
      let tOut = min(cNext.x, min(cNext.y, cNext.z));
      let occ = farOcc[farOccIndex(level, cc * i32(CHUNK))];
      if (occ != 0u) {
        var tIn = tCur;
        var walk = true;
        let top = farOccTop(occ);
        if (top != 0u) {
          let yTop = cc.y * i32(CHUNK) + i32(top);
          if (roL.y + rd.y * (tIn + 1e-4) >= f32(yTop)) {
            if (rd.y >= 0.0) {
              walk = false;
            } else {
              let tp = max((f32(yTop) - roL.y) * inv.y, tIn);
              if (tp + 1e-4 >= tOut) { walk = false; } else { tIn = tp + 1e-4; }
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
            vMax[a] = (f32(vc[a]) + select(0.0, 1.0, rd[a] > 0.0) - roL[a]) * inv[a];
          }
          for (var j = 0; j < 3 * i32(CHUNK); j++) {
            if (budget <= 0) { return vCur * s; }
            budget -= 1;
            if (rayStartFarCandidate(level, vc)) { return vCur * s; }
            if (vMax.x < vMax.y && vMax.x < vMax.z) {
              vc.x += stepv.x; vCur = vMax.x; vMax.x += tDelta.x;
            } else if (vMax.y < vMax.z) {
              vc.y += stepv.y; vCur = vMax.y; vMax.y += tDelta.y;
            } else {
              vc.z += stepv.z; vCur = vMax.z; vMax.z += tDelta.z;
            }
            if (vCur >= tOut || vCur >= tExit) { break; }
          }
        }
      }
      if (cNext.x <= cNext.y && cNext.x <= cNext.z) {
        tCur = cNext.x; cc.x += stepv.x;
        cNext.x = (f32((cc.x + max(stepv.x, 0)) * i32(CHUNK)) - roL.x) * inv.x;
      } else if (cNext.y <= cNext.z) {
        tCur = cNext.y; cc.y += stepv.y;
        cNext.y = (f32((cc.y + max(stepv.y, 0)) * i32(CHUNK)) - roL.y) * inv.y;
      } else {
        tCur = cNext.z; cc.z += stepv.z;
        cNext.z = (f32((cc.z + max(stepv.z, 0)) * i32(CHUNK)) - roL.z) * inv.z;
      }
    }
    tPrev = max(tPrev, tStop * s - FAR_DITHER_MAX * 2.0 * s);
  }
  return RS_NONE;
}

// ---- THE LAW, AND THE WIDE TIER BELOW IT (2026-10-03, raymarch-core) -------
// raymarch.wgsl's rayStartAt trusts this map only while every cell either
// march can meet is wide enough on screen for the lattice argument above.
// k = R.viewPx / (2 tan(fovY / 2)) is px per fine voxel at t = 1.
//
// THE SHIPPED TIER (RS_LAW_PX = 3): k >= 3 * 224 = 672 — a 2-px square lattice,
// one ray per 2x2 block through its corner, a 5x5 min. Exactly what landed
// 2026-09-28, untouched. It holds at 1920x1080 and the shipped fovY 1.35 by
// 0.4% (k = 675): the game's OWN 1600x900 window (k = 562) has never had it,
// and neither has a 1080p frame once the sprint FOV widens (thirdPerson.
// speedFov 0.06 -> fovY 1.41, k = 633). Until this change the prepass did not
// even ask: below the law it marched every sample for a reader that threw the
// map away (noon at 1600x900: 0.56 ms of `pre` for nothing).
//
// THE WIDE TIER, for every frame below that law. Two changes make the argument
// hold down to 2.5 px:
//   * A STAGGERED lattice: odd sample rows shift one pixel right, so the
//     samples are (2i + 1 + (j & 1), 2j + 1). Its covering radius is 1.25 px
//     (the circumradius of the (0,0),(2,0),(1,2) triangle) against the square
//     lattice's sqrt(2) = 1.41, so a cell whose inscribed disc is >= 2.5 px
//     across holds a sample (the square lattice needs 2.83).
//   * A 7x7 min instead of 5x5. The pixel lies in the cell's projection, which
//     contains the convex hull of the pixel and the inscribed disc (radius
//     c/2, within sqrt(3) c / 2 of the pixel); at distance d from the pixel
//     along the line to the disc centre that hull holds a disc of radius
//     (c/2) d / |PO|, which reaches the covering radius by d = 2.17 px — so a
//     sample lies within 2.17 + 1.25 = 3.42 px of the pixel for EVERY c >= 2.5,
//     however big the cell. The 7x7 window reaches >= 4 px on every side of
//     every pixel of the block with the stagger (5x5 reaches only 2 on one side
//     of an odd row), so the bound holds with room.
// And the two halves are trusted separately:
//   * THE CASCADE HALF needs its cells >= 2.5 px everywhere: k >= 2.5 * 224 =
//     560 (rayStartFarOk). Below that this pass does not march it, writes
//     RS_NONE for "nothing fine", and the reader does not read rs1.
//   * THE FINE HALF needs no global law at all: every fine cell a pixel skips
//     lies nearer than its start, so capping the start at k / 2.5 (fs(),
//     RS_NEAR_CAP) makes every skipped cell >= 2.5 px wide — the argument
//     holds per pixel, at any size. A frame where even that cap is short
//     (k / 2.5 < RS_NEAR_MIN_VOX) builds no map.
// Measured, one process, wide tier vs no map (RTX 3060 Ti): at 1080p and the
// sprint FOV 1.41, noon 7.67 -> 6.84 ms, cascade 5.80 -> 5.06, meadow 5.87 ->
// 5.44, seam 5.30 -> 4.77, seamveg 8.14 -> 7.42; at 1600x900 and fovY 1.35,
// noon 5.64 -> 5.18, cascade 4.28 -> 4.01, meadow 4.31 -> 4.09, seam 3.82 ->
// 3.46, seamveg 6.15 -> 5.83 — the prepass included. The wide tier is NOT used
// where the shipped law holds: there it measured +0.13..0.26 ms against the
// 5x5 (a min over 49 samples starts every ray a little earlier).
//
// The reader (raymarch.wgsl rayStartAt) carries the same constants and the
// same expressions. Where this pass builds no map, thread 0 writes a key of 0,
// which no frame's key equals (rayStartKey forces the low bit), so even a
// reader that disagreed would fail the key check and march from the camera.
const RS_LAW_PX : f32 = 3.0;                // raymarch.wgsl agrees
const RS_LAW_WIDE_PX : f32 = 2.5;           // raymarch.wgsl agrees
const RS_NEAR_MIN_VOX : f32 = 32.0;         // raymarch.wgsl agrees
const RS_NEAR_MAX_VOX : f32 = select(f32(WORLD_N) * 0.8661,
                                     TUNE_LOD_HANDOFF_DIST / VOXEL_METERS,
                                     TUNE_LOD_HANDOFF_DIST < WINDOW_HALF_EXTENT_METERS);
const RS_FAR_MAX_CELLS : f32 =
    f32((i32(FAR_NCHUNK) / 2 - FAR_SPHERE_MARGIN_CHUNKS) * i32(CHUNK));
fn rayStartK() -> f32 { return R.viewPx * 0.5 / R.tanHalfFov; }
// The shipped tier: both halves, square lattice, 5x5.
fn rayStartLawOk() -> bool {
  return rayStartK() >= RS_LAW_PX * max(RS_NEAR_MAX_VOX, RS_FAR_MAX_CELLS);
}
// The cascade half, in either tier.
fn rayStartFarOk() -> bool {
  return rayStartLawOk() || rayStartK() >= RS_LAW_WIDE_PX * RS_FAR_MAX_CELLS;
}
// Any map at all.
fn rayStartAny() -> bool {
  return rayStartK() >= RS_LAW_WIDE_PX * RS_NEAR_MIN_VOX;
}

// ---- pass 1: one ray per 2x2 block, through the block's shared corner ------
// A FIXED grid (the buffer's capacity, pass_table.def); threads outside this
// frame's target return on the bounds test, and thread 0 writes the key.
@compute @workgroup_size(8, 8)
fn rayStartTrace(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = rayStartDims();
  let q = (dims + vec2<u32>(1u)) / 2u;
  let need = RS_HEADER + 2u * q.x * q.y;
  if (need > arrayLength(&rayStart)) { return; }
  // No map at all: no march and NO KEY, so the reader's key check fails too.
  if (!rayStartAny()) {
    if (all(gid.xy == vec2<u32>(0u))) { rayStart[0] = 0u; }
    return;
  }
  let farOk = rayStartFarOk();
  if (all(gid.xy == vec2<u32>(0u))) { rayStart[0] = rayStartKey(); }
  if (gid.x >= q.x || gid.y >= q.y) { return; }
  // The wide tier's staggered lattice (see THE LAW above).
  let stag = select(f32(gid.y & 1u), 0.0, rayStartLawOk());
  var rd = rayStartDir(vec2f(gid.xy * 2u) + vec2f(1.0 + stag, 1.0), dims);
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let sp = rayStartSpan(R.camPos, inv);
  var t = rayStartFine(R.camPos, rd, inv, sp);
  if (t < 0.0 && !farOk) {
    // The cascade half is not trusted at this size: "nothing fine".
    t = RS_NONE;
  } else if (t < 0.0) {
    // Nothing fine: the cascade from where fs() will start it (h.tExit is the
    // span's exit, or 0 for a ray that never enters the window).
    t = rayStartFar(R.camPos, rd, inv, select(0.0, sp.tExit, sp.tExit > sp.tEnter));
  }
  rayStart[RS_HEADER + gid.y * q.x + gid.x] = bitcast<u32>(t);
}

// ---- pass 2: the 5x5 (7x7) minimum (the reach the argument needs) ----------
// SEPARABLE, THROUGH WORKGROUP MEMORY (2026-10-03, raymarch-far). The window
// min used to read its 25 (49) samples straight from the storage buffer, per
// output: ~13M loads a 1080p frame. A workgroup now stages its 8x8 outputs'
// tile plus a RS_MIN_PAD apron once (196 loads for 64 outputs), takes the row
// min, then the column min. Same samples, same min: bit-identical output.
// Measured (--render-budget, one process, the old loop as an in-process arm,
// RTX 3060 Ti 1080p): the `pre` span 1.23 -> 1.19 ms noon, 0.93 -> 0.90
// cascade, 0.84 -> 0.81 seam.
const RS_MIN_PAD : i32 = RS_MIN_RADIUS + 1;               // the wide tier's radius
const RS_MIN_TILE : u32 = 8u + 2u * u32(RS_MIN_PAD);      // 14
var<workgroup> rsTile : array<f32, RS_MIN_TILE * RS_MIN_TILE>;
var<workgroup> rsRowMin : array<f32, RS_MIN_TILE * 8u>;
@compute @workgroup_size(8, 8)
fn rayStartMin(@builtin(global_invocation_id) gid : vec3<u32>,
               @builtin(local_invocation_id) lid : vec3<u32>,
               @builtin(local_invocation_index) li : u32,
               @builtin(workgroup_id) wid : vec3<u32>) {
  let dims = rayStartDims();
  let q = (dims + vec2<u32>(1u)) / 2u;
  if (RS_HEADER + 2u * q.x * q.y > arrayLength(&rayStart)) { return; }
  if (!rayStartAny()) { return; }
  // 5x5 in the shipped tier, 7x7 in the wide one (see THE LAW).
  let rad = select(RS_MIN_RADIUS + 1, RS_MIN_RADIUS, rayStartLawOk());
  // Stage the tile. Clamped at the frame edge: every output whose window the
  // clamp could reach is within `rad` of the edge and writes 0 below anyway.
  let org = vec2<i32>(wid.xy * 8u) - vec2<i32>(RS_MIN_PAD);
  let qMax = vec2<i32>(q) - vec2<i32>(1);
  for (var i = li; i < RS_MIN_TILE * RS_MIN_TILE; i += 64u) {
    let p = clamp(org + vec2<i32>(i32(i % RS_MIN_TILE), i32(i / RS_MIN_TILE)),
                  vec2<i32>(0), qMax);
    rsTile[i] = bitcast<f32>(rayStart[RS_HEADER + u32(p.y) * q.x + u32(p.x)]);
  }
  workgroupBarrier();
  // Row pass: every tile row, the 8 output columns.
  for (var i = li; i < RS_MIN_TILE * 8u; i += 64u) {
    let r = i / 8u;
    let c = i32(i % 8u) + RS_MIN_PAD;
    var m = RS_NONE;
    for (var dx = -rad; dx <= rad; dx++) {
      m = min(m, rsTile[r * RS_MIN_TILE + u32(c + dx)]);
    }
    rsRowMin[i] = m;
  }
  workgroupBarrier();
  if (gid.x >= q.x || gid.y >= q.y) { return; }
  // THE SCREEN EDGE: a cell the frame border cuts may show a sliver narrower
  // than the lattice with no sample of its own on screen, so a window the
  // border clips proves nothing. 0 = "march from the camera", the old path,
  // for a frame RS_MIN_RADIUS samples (~4 px) wide.
  let gi = vec2<i32>(gid.xy);
  if (any(gi < vec2<i32>(rad)) || any(gi >= vec2<i32>(q) - vec2<i32>(rad))) {
    rayStart[RS_HEADER + q.x * q.y + gid.y * q.x + gid.x] = 0u;
    return;
  }
  var m = RS_NONE;
  for (var dy = -rad; dy <= rad; dy++) {
    m = min(m, rsRowMin[u32(i32(lid.y) + RS_MIN_PAD + dy) * 8u + lid.x]);
  }
  rayStart[RS_HEADER + q.x * q.y + gid.y * q.x + gid.x] = bitcast<u32>(m);
}
