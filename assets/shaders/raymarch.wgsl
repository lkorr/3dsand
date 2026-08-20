// raymarch.wgsl — fullscreen two-level DDA over the voxel grid.
// Chunk-level skip via per-chunk occupancy counts, voxel DDA inside non-empty
// chunks. The renderer reads the same device-local buffers the sim writes —
// zero upload cost (DESIGN.md §9). Rendering is allowed to use floats; only
// sim state must stay integer.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
// far-field cascades (render-only LOD — DESIGN.md §9)
@group(0) @binding(4) var<storage, read> farVox : array<u32>;
@group(0) @binding(5) var<storage, read> farOcc : array<u32>;
@group(0) @binding(6) var<uniform> F : FarParams;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) uv : vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out : VSOut;
  out.pos = vec4f(p[vi], 0.0, 1.0);
  out.uv = p[vi];
  return out;
}

fn unpackColor(c : u32) -> vec3f {
  return vec3f(f32(c & 0xFFu), f32((c >> 8u) & 0xFFu), f32((c >> 16u) & 0xFFu)) / 255.0;
}

fn paletteColor(m : Material, state : u32) -> vec3f {
  switch (state % 3u) {
    case 0u: { return unpackColor(m.color0); }
    case 1u: { return unpackColor(m.color1); }
    default: { return unpackColor(m.color2); }
  }
}

fn skyColor(rd : vec3f) -> vec3f {
  let t = clamp(rd.y * 1.4 + 0.25, 0.0, 1.0);
  var c = mix(vec3f(0.72, 0.80, 0.90), vec3f(0.25, 0.47, 0.85), t);
  let s = max(dot(rd, R.sunDir), 0.0);
  c += vec3f(1.0, 0.9, 0.7) * (pow(s, 800.0) * 3.0 + pow(s, 8.0) * 0.12);
  return c;
}

// Per-meter absorption scale applied to material opacity — trace() (media
// early-out) and fs() (final tint) must agree on it.
const MEDIA_ABSORB : f32 = 6.4;
// Optical depth past which the background is invisible (exp(-6) ~ 0.25%):
// stop marching instead of walking the rest of a smoke plume voxel-by-voxel.
const MEDIA_TAU_MAX : f32 = 6.0;

struct Hit {
  hit      : bool,
  saturated: bool,    // media absorbed the ray before any surface hit
  t        : f32,
  tExit    : f32,     // where the ray leaves the window box (0 if it misses):
                      // the far-field march starts here, never inside the
                      // window, so coarse data can't occlude fine data

  cell     : vec3<i32>,
  axis     : i32,     // axis stepped into the hit cell
  sgn      : f32,     // ray direction sign on that axis
  word     : u32,
  mediaTau : f32,     // optical depth: per-cell opacity x fullness x length
  mediaTint: vec3f,   // tau-weighted media color (divide by mediaTau to shade)
  mediaMat : u32,     // first media material crossed (liquid surface term)
  mediaSurf: f32,     // fullness (0..1) of the first media cell — surface term
  fireGlow : f32,     // flicker- and transmittance-weighted emissive path
  fireMat  : u32,     // first emissive media material (palette for the ramp)

  // ---- water surface (see shadeWater) ----
  // A translucent liquid is BOTH a surface and a volume. The media fields above
  // are the volume half; these are the surface half: where the ray first
  // crossed into the liquid, and which cell it entered, so fs() can build a
  // normal there and shade a real air/water interface instead of only tinting.
  liqT     : f32,     // t of the first liquid entry (0 if none)
  liqCell  : vec3<i32>,
  liqAxis  : i32,     // face the ray entered the liquid through
  liqSgn   : f32,
  liqPath  : f32,     // total distance travelled INSIDE liquid, fine voxels —
                      // drives per-channel Beer-Lambert depth absorption
};

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, R.origin); }

fn chunkOcc(cell : vec3<i32>) -> u32 {
  return occupancy[chunkIndexW(cell)];
}

fn trace(ro : vec3f, rdIn : vec3f, maxSteps : i32, wantMedia : bool) -> Hit {
  var out : Hit;
  out.hit = false;
  out.saturated = false;
  out.tExit = 0.0;
  out.mediaTau = 0.0;
  out.mediaTint = vec3f(0.0);
  out.mediaMat = 0u;
  out.mediaSurf = 0.0;
  out.fireGlow = 0.0;
  out.fireMat = 0u;
  out.liqT = 0.0;
  out.liqCell = vec3<i32>(0);
  out.liqAxis = 1;
  out.liqSgn = -1.0;
  out.liqPath = 0.0;

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // clip to the residency window AABB (world coords)
  let nf = f32(WORLD_N);
  let wlo = vec3f(R.origin * i32(CHUNK));
  let tt0 = (wlo - ro) * inv;
  let tt1 = (wlo + vec3f(nf) - ro) * inv;
  let tmin = min(tt0, tt1);
  let tmax = max(tt0, tt1);
  let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
  let tExit = min(tmax.x, min(tmax.y, tmax.z));
  if (tExit <= tEnter) { return out; }
  out.tExit = tExit;

  var t = tEnter + 1e-4;
  var p = ro + rd * t;
  let wloI = R.origin * i32(CHUNK);
  var cell = clamp(vec3<i32>(floor(p)), wloI, wloI + vec3<i32>(i32(WORLD_N) - 1));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = (boundary - ro[a]) * inv[a];
  }

  // which axis did we enter through (for first-cell normal)
  var axis = 0;
  if (tmin.y > tmin.x && tmin.y > tmin.z) { axis = 1; }
  else if (tmin.z > tmin.x && tmin.z > tmin.y) { axis = 2; }

  var tCur = t;
  // Optical depth from GASES only — drives the saturation early-out, which is
  // a smoke optimization and must not cut a ray short inside clear water.
  var gasTau = 0.0;

  for (var i = 0; i < 4096; i++) {
    if (i >= maxSteps) { break; }
    if (!inBounds(cell)) { break; }

    // Chunk skip. Media-blind rays (shadows) skip on the BLOCKER count, so a
    // chunk holding only smoke/steam is as cheap as air; media-aware rays
    // need the per-cell march whenever anything at all is present.
    let occ = chunkOcc(cell);
    let occN = select(occBlockers(occ), occTotal(occ), wantMedia);
    if (occN == 0u) {
      // empty chunk: jump straight to its exit face
      let ch = worldChunkOf(cell);
      let lo = vec3f(ch * i32(CHUNK));
      let hi = lo + f32(CHUNK);
      let e0 = (lo - ro) * inv;
      let e1 = (hi - ro) * inv;
      let ex = max(e0, e1);
      // The jump target must never sit behind the ray: a cell floor()ed onto a
      // shared face belongs to a chunk the ray is already exiting, so the raw
      // exit t can be <= tCur and the march would stall in place.
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      t = tOut + 1e-4;
      if (t >= tExit) { break; }
      p = ro + rd * t;
      var nc = vec3<i32>(floor(p));
      // Force the crossing on the exit axis: float noise at a shared face can
      // floor() back into the chunk just exited, which reads as a see-through
      // seam along chunk boundaries.
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(ch.x * i32(CHUNK) - 1, (ch.x + 1) * i32(CHUNK), rd.x > 0.0);
      } else if (ex.y <= ex.z) {
        nc.y = select(ch.y * i32(CHUNK) - 1, (ch.y + 1) * i32(CHUNK), rd.y > 0.0);
      } else {
        nc.z = select(ch.z * i32(CHUNK) - 1, (ch.z + 1) * i32(CHUNK), rd.z > 0.0);
      }
      if (!inBounds(nc)) { break; }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - ro[a]) * inv[a];
      }
      tCur = t;
      continue;
    }

    let w = voxels[cellIndexW(cell)];
    let mat = voxMat(w);
    var weight = 0.0;   // this cell's media contribution per unit length
    var cellOp = 0.0;   // this cell's opacity (per-cell, not first-material)
    var cellTint = vec3f(0.0);
    var cellFire = 0.0; // this cell's flicker-weighted emission
    var cellLiq = 0.0;  // fullness if this cell is a translucent liquid
    if (mat != MAT_AIR) {
      let k = materials[mat].klass;
      // gases and translucent liquids are participating media; OPAQUE liquids
      // (lava, molten glass) read as surfaces
      if (k == CLASS_GAS ||
          (k == CLASS_LIQUID && (materials[mat].flags & MATF_OPAQUE) == 0u)) {
        if (wantMedia) {
          // liquids weight by fullness so a 1/8 film tints far less than a
          // full cell; gases count whole
          weight = select(1.0, f32(voxState(w) + 1u) / 8.0, k == CLASS_LIQUID);
          cellOp = f32(materials[mat].opacity) / 255.0;
          cellTint = (unpackColor(materials[mat].color0) +
                      unpackColor(materials[mat].color1)) * 0.5;
          if (out.mediaMat == 0u) {
            out.mediaMat = mat;
            out.mediaSurf = weight;
          }
          // First liquid crossing: remember the interface so fs() can shade a
          // real surface there. Recorded per-CLASS (not per-material) because
          // only liquids get the Fresnel/refraction treatment — a gas has no
          // interface to reflect off.
          if (k == CLASS_LIQUID) {
            cellLiq = weight;
            if (out.liqT == 0.0) {
              out.liqT = tCur;
              out.liqCell = cell;
              out.liqAxis = axis;
              out.liqSgn = sign(rd[axis]);
            }
          }
          // emissive media (fire): per-cell spatio-temporal flicker so each
          // flame voxel pulses on its own phase instead of the whole plume
          // beating in sync (render-only floats, same trick as ember surfaces)
          if (materials[mat].emission > 0u) {
            let fh = pcg(bitcast<u32>(cell.x * 7 + cell.y * 131 + cell.z * 2917));
            let fl = 0.70 + 0.55 * sin(R.time * 13.0 + f32(fh & 0x3FFu) * 0.00614);
            cellFire = (f32(materials[mat].emission) / 255.0) * fl;
            if (out.fireMat == 0u) { out.fireMat = mat; }
          }
        }
        // fall through and keep marching
      } else {
        out.hit = true;
        out.t = tCur;
        out.cell = cell;
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.word = w;
        return out;
      }
    }

    // DDA step; accumulate the segment the ray spent inside this cell
    let tPrev = tCur;
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    let segRaw = tCur - tPrev;
    out.liqPath += segRaw * cellLiq;   // depth travelled in liquid (Beer-Lambert)
    let seg = segRaw * weight;
    if (seg > 0.0 && cellOp > 0.0) {
      // fire is dimmed by the media already crossed in front of it, so flames
      // deep inside their own smoke fade out instead of x-raying the plume
      if (cellFire > 0.0) {
        let trans = exp(-out.mediaTau * VOXEL_METERS * MEDIA_ABSORB);
        out.fireGlow += seg * cellFire * trans;
      }
      let dTau = seg * cellOp;
      out.mediaTau += dTau;
      out.mediaTint += cellTint * dTau;
      if (cellLiq == 0.0) { gasTau += dTau; }
    }
    // Media early-out: fs() will mix the background in at exp(-tau); once
    // that is ~0 the rest of the march (often hundreds of per-voxel steps
    // through a smoke plume) cannot change the pixel.
    //
    // GASES ONLY. This is a smoke optimization, and applying it to liquids is
    // what kept lake beds invisible: water's authored opacity (90/255) against
    // the legacy MEDIA_ABSORB saturates after ~2.7 m of path, so any lake
    // deeper than waist height — or any shallow one viewed at a grazing angle,
    // which is most of them — terminated the ray in mid-water and reported no
    // hit. shadeWater() attenuates with real per-channel Beer-Lambert instead,
    // under which 1.5 m of water still transmits ~53% green / ~74% blue, so
    // the bed is genuinely visible and the march has to reach it.
    // Liquids get their own far looser cap below.
    if (gasTau * VOXEL_METERS * MEDIA_ABSORB > MEDIA_TAU_MAX) {
      out.saturated = true;
      out.t = tCur;
      break;
    }
    // Liquid depth cap: past this much water even blue is gone (exp(-0.2*24)
    // ~ 0.8%), so the bed cannot affect the pixel and the march can stop.
    // Purely a perf bound on very deep water, ~24 m of path.
    if (out.liqPath * VOXEL_METERS > 24.0) {
      out.saturated = true;
      out.t = tCur;
      break;
    }
    if (tCur >= tExit) { break; }
  }
  return out;
}

// ---- far-field cascade march (DESIGN.md §9) ----
// Continues a ray that left the residency window without hitting anything.
// Each cascade level is marched in ITS OWN cell units (the same DDA as the
// fine march, occupancy-skipped per level chunk); `t` values convert back to
// fine-voxel units so depth and fog reuse the existing math. Levels are
// nested boxes: starting level k at max(its entry, level k-1's exit) makes
// the t-ordering skip every region covered by finer data automatically.
//
// ---- level-transition dither (plan phase 3A) ----
// Every handoff above is a HARD distance: at one exact t the representation
// jumps from 2^k-voxel cells to 2^(k+1)-voxel cells, and because the two
// resolutions disagree about where the surface is by up to half a coarse
// cell, that constant-t surface draws as a visible arc across hillsides —
// the same artifact as an unblended terrain-LOD ring.
//
// Fix: pull each handoff distance NEARER by a per-pixel random amount of up to
// half an OUTER cell at that seam. Neighboring pixels then cross the seam at
// slightly different distances, so the ring dissolves into a stipple that
// reads as texture instead of as a line. This is stratified sampling of the
// seam, not a blend: each pixel still picks exactly one level, so there is no
// extra marching cost.
//
// THE OFFSET IS ONE-SIDED (nearer only), and that is not a style choice. The
// levels tile t-space exactly: level k+1 starts where level k's box ends.
// Pushing a handoff FARTHER opens a gap that no level marches, and rays through
// it fall past the terrain into whatever is behind — measured as ~3.2k pixels
// of hole-speckle punched through tree edges when this was first written
// two-sided. Pulling it NEARER only makes the coarser level re-cover a sliver
// the finer level already marched and found empty, which is exactly the
// intended "this pixel takes the coarse surface a bit early".
//
// Two rules the hash must obey:
//   * NO TIME INPUT. A time-varying hash makes the stipple crawl, which is
//     far more objectionable than the seam it replaces; the pattern must be
//     frozen to the pixel so it reads as static dither.
//   * KEYED ON PIXEL, NOT ON WORLD POSITION. The seam is a screen-space
//     artifact of the camera-centered cascade boxes, so screen space is where
//     it must be broken up; a world-space key would leave the pattern
//     stationary in the world and re-align into arcs as the boxes recenter.
// Render-only float math on render-only data — determinism is untouched
// (CLAUDE.md rule 1 scopes to sim state).
//
// Returns an offset in [0, 0.5) cells, uniform-ish per pixel, to SUBTRACT.
fn farDither(px : vec2f) -> f32 {
  let h = pcg(u32(px.x) * 1973u + u32(px.y) * 9277u + 0x9E3779B9u);
  // 16 bits of mantissa is plenty: the seam only needs enough distinct
  // offsets that no two adjacent pixels line up, not a smooth distribution.
  return f32(h & 0xFFFFu) * (0.5 / 65536.0);
}

struct FarHit {
  hit   : bool,
  t     : f32,         // fine-voxel units
  axis  : i32,
  sgn   : f32,
  mat   : u32,
  cell  : vec3<i32>,   // level cells (palette jitter, AO neighbors)
  level : u32,         // which cascade level the hit lives in (shadow march)
};

fn farMatAt(level : u32, c : vec3<i32>) -> u32 {
  let bi = farVoxByteIndex(level, c);
  return (farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu;
}

fn traceFar(ro : vec3f, rdIn : vec3f, tStart : f32, px : vec2f) -> FarHit {
  var out : FarHit;
  out.hit = false;

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // One draw per pixel, reused at every seam scaled by that seam's cell size:
  // the offsets stay correlated across levels for one pixel (a pixel that
  // takes the coarse side early keeps taking it), which stipples cleanly
  // instead of re-randomizing into per-level speckle.
  let dith = farDither(px);

  // Window -> level 1 seam. tStart is where the ray left the residency window;
  // the fine march already found no hit out to there, so starting level 1 up to
  // half a level-1 cell earlier only lets it re-cover the last sliver of
  // already-marched empty fine space. It exists to break the hard line where
  // the two representations of the same terrain disagree, nothing more.
  var tPrev = max(tStart - dith * f32(1u << farCellShift(1u)), 0.0);

  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let s = f32(1u << farCellShift(level));   // fine voxels per level cell
    let org = F.origins[level - 1u].xyz;
    // everything below is in LEVEL-CELL coords: pos/s, t/s (same rd)
    let roL = ro / s;
    let lo = vec3f(org * i32(CHUNK));
    let tt0 = (lo - roL) * inv;
    let tt1 = (lo + f32(FAR_N) - roL) * inv;
    let tmin = min(tt0, tt1);
    let tmax = max(tt0, tt1);
    let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, tPrev / s));
    let tExit = min(tmax.x, min(tmax.y, tmax.z));
    if (tExit <= tEnter) { continue; }   // box missed (or fully behind tPrev)

    var t = tEnter + 1e-4;
    var p = roL + rd * t;
    let loI = org * i32(CHUNK);
    var cell = clamp(vec3<i32>(floor(p)), loI, loI + vec3<i32>(i32(FAR_N) - 1));
    let stepv = vec3<i32>(sign(rd));
    let tDelta = abs(inv);
    var tMax : vec3f;
    for (var a = 0; a < 3; a++) {
      let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
      tMax[a] = (boundary - roL[a]) * inv[a];
    }
    var axis = 0;
    if (tmin.y > tmin.x && tmin.y > tmin.z) { axis = 1; }
    else if (tmin.z > tmin.x && tmin.z > tmin.y) { axis = 2; }
    var tCur = t;

    for (var i = 0; i < 384; i++) {
      if (!farInBox(cell, org)) { break; }
      if (farOcc[farOccIndex(level, cell)] == 0u) {
        // empty level chunk: jump to its exit face (same seam-safe jump as
        // the fine march — force the crossing on the exit axis)
        let ch = worldChunkOf(cell);
        let clo = vec3f(ch * i32(CHUNK));
        let e0 = (clo - roL) * inv;
        let e1 = (clo + f32(CHUNK) - roL) * inv;
        let ex = max(e0, e1);
        let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
        t = tOut + 1e-4;
        if (t >= tExit) { break; }
        p = roL + rd * t;
        var nc = vec3<i32>(floor(p));
        if (ex.x <= ex.y && ex.x <= ex.z) {
          nc.x = select(ch.x * i32(CHUNK) - 1, (ch.x + 1) * i32(CHUNK), rd.x > 0.0);
        } else if (ex.y <= ex.z) {
          nc.y = select(ch.y * i32(CHUNK) - 1, (ch.y + 1) * i32(CHUNK), rd.y > 0.0);
        } else {
          nc.z = select(ch.z * i32(CHUNK) - 1, (ch.z + 1) * i32(CHUNK), rd.z > 0.0);
        }
        if (!farInBox(nc, org)) { break; }
        cell = nc;
        for (var a = 0; a < 3; a++) {
          let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
          tMax[a] = (boundary - roL[a]) * inv[a];
        }
        tCur = t;
        continue;
      }

      let mat = farMatAt(level, cell);
      if (mat != 0u) {
        out.hit = true;
        out.t = tCur * s;   // back to fine-voxel units
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.mat = mat;
        out.cell = cell;
        out.level = level;
        return out;
      }

      if (tMax.x < tMax.y && tMax.x < tMax.z) {
        cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
      } else if (tMax.y < tMax.z) {
        cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
      } else {
        cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
      }
      if (tCur >= tExit) { break; }
    }
    // Level k -> k+1 seam. The outer level's cells are 2s fine voxels, so half
    // a coarse cell is s: pull this handoff up to s nearer and the ring where
    // level k's box ends dissolves. Still max()'d against the running tPrev so
    // the start can never precede an even earlier level's coverage.
    tPrev = max(tPrev, tExit * s - dith * 2.0 * s);
  }
  return out;
}

// ---- far-field sun shadows (phase 4: distance look) ----
// One coarse DDA toward the sun through the SAME cascade level the hit lives
// in, occupancy-skipped and hard-capped. Lighting mismatch is what makes LOD
// terrain read as "LOD terrain": the near field casts real shadow rays, so an
// unshadowed far field renders every hillside and every spot under a canopy at
// identical brightness and the horizon flattens into wallpaper. One level only
// — a sun ray leaves the hit level's box within a few dozen cells, and
// cross-level shadow reach buys nothing visible through fog at that range.
// Render-only float math on render-only data (CLAUDE.md rule 1 scopes to sim).
fn farShadowed(level : u32, roFine : vec3f) -> bool {
  var rd = R.sunDir;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let s = f32(1u << farCellShift(level));
  let roL = roFine / s;
  let org = F.origins[level - 1u].xyz;
  let lo = vec3f(org * i32(CHUNK));
  let tt0 = (lo - roL) * inv;
  let tt1 = (lo + f32(FAR_N) - roL) * inv;
  let tExit = min(max(tt0.x, tt1.x), min(max(tt0.y, tt1.y), max(tt0.z, tt1.z)));

  var cell = vec3<i32>(floor(roL));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = (boundary - roL[a]) * inv[a];
  }
  var tCur = 0.0;
  for (var i = 0; i < 128; i++) {
    if (!farInBox(cell, org)) { return false; }
    if (farOcc[farOccIndex(level, cell)] == 0u) {
      // empty level chunk: jump to its exit face (seam-safe, as in traceFar)
      let ch = worldChunkOf(cell);
      let clo = vec3f(ch * i32(CHUNK));
      let e0 = (clo - roL) * inv;
      let e1 = (clo + f32(CHUNK) - roL) * inv;
      let ex = max(e0, e1);
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      let t = tOut + 1e-4;
      if (t >= tExit) { return false; }
      let p = roL + rd * t;
      var nc = vec3<i32>(floor(p));
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(ch.x * i32(CHUNK) - 1, (ch.x + 1) * i32(CHUNK), rd.x > 0.0);
      } else if (ex.y <= ex.z) {
        nc.y = select(ch.y * i32(CHUNK) - 1, (ch.y + 1) * i32(CHUNK), rd.y > 0.0);
      } else {
        nc.z = select(ch.z * i32(CHUNK) - 1, (ch.z + 1) * i32(CHUNK), rd.z > 0.0);
      }
      if (!farInBox(nc, org)) { return false; }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - roL[a]) * inv[a];
      }
      tCur = t;
      continue;
    }
    if (farMatAt(level, cell) != 0u) { return true; }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z;
    }
    if (tCur >= tExit) { return false; }
  }
  return false;
}

// Aerial perspective: distance fog that converges EXACTLY to the sky color in
// that ray's direction. The old `skyColor * 0.9` target left every distant
// surface hanging slightly darker than the sky it should dissolve into, which
// read as a gray veil over the whole horizon instead of atmosphere.
fn applyAerial(color : vec3f, rd : vec3f, tFine : f32) -> vec3f {
  let f = 1.0 - exp(-tFine * VOXEL_METERS * R.fogDensity);
  return mix(color, skyColor(rd), f);
}

// ============================================================================
// OPAQUE SURFACE LOOK (DESIGN.md §9)
// ============================================================================
// Everything below shades a solid/powder voxel face. It replaces what used to
// be four lines — palette pick, a hardcoded per-axis constant, a binary shadow
// ray, and a flat 0.38 ambient — which is why terrain read as matte plastic:
// every up-facing voxel in the frame returned the exact same brightness, so
// there was no ambient occlusion anywhere, no sky/bounce color split, and the
// only spatial variation in the whole image was per-voxel palette confetti.
//
// All render-only float math on render-only data — the sim never sees any of
// it and the world hash never covers it (CLAUDE.md rule 1 scopes to sim state).

// ---- hemisphere ambient ----
// Ambient was a scalar, and a scalar ambient is the single strongest "untextured
// prototype" cue there is: it lights the underside of an overhang exactly as
// brightly, and in exactly the same hue, as a face pointing at open sky.
//
// Real outdoor ambient has two very different sources, and splitting them costs
// one mix(): the SKY (cool, from above) and BOUNCE off the ground (warm, from
// below, since sunlight that missed the surface hit the dirt first). Terrain
// shaded this way gets its form back for free — north faces go blue-shifted,
// undersides go earth-toned, and the eye reads that split as shape.
const AMB_SKY    : vec3f = vec3f(0.40, 0.48, 0.62);   // zenith, cool
const AMB_GROUND : vec3f = vec3f(0.25, 0.22, 0.17);   // bounce, warm
fn ambientAt(n : vec3f) -> vec3f {
  // n.y = -1 -> full bounce, n.y = +1 -> full sky
  return mix(AMB_GROUND, AMB_SKY, n.y * 0.5 + 0.5);
}

// ---- diffuse response ----
// Plain max(dot(n,l),0) is wrong for terrain built out of axis-aligned voxel
// faces, and it is the specific reason a grassy hillside rendered as harsh
// horizontal banding. A voxel slope is a STAIRCASE: every 1-voxel rise puts a
// vertical face next to a horizontal one. With a hard Lambert term the top face
// gets dot ~= 0.66 and the away-facing riser gets exactly 0, so the two
// alternate at ~1.8x brightness down the whole hill. The eye reads that
// alternation as noise, not as slope, because a real grass slope has no such
// discontinuity — the two facets differ by a few percent, not by 80%.
//
// Wrapped diffuse fixes it at the source: remap dot from [-1,1] so the falloff
// continues smoothly past the terminator instead of clamping to zero. This is
// the standard cheap stand-in for the light a rough/scattering surface picks up
// at grazing angles, and it keeps risers lit enough to sit next to their tops.
fn wrapDiffuse(ndl : f32, wrap : f32) -> f32 {
  return clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
}

// ---- voxel-scale grain ----
// The palette variants are picked by the state nibble, which worldgen fills
// with `rnd % 3` — white noise. Three colors at ~8% lightness spread, assigned
// independently per voxel, is precisely the recipe for the green confetti that
// covered every grass field: maximum spatial frequency at maximum contrast.
//
// Fix without touching sim state (the nibble is hashed into the world hash, so
// worldgen cannot change): keep the palette pick, but modulate it with a
// SMOOTH, correlated value-noise field so neighbouring voxels agree. That turns
// per-voxel static into patches that read as material variation. Two octaves at
// different world scales: a broad one for large-scale mottling, a fine one that
// still varies per voxel but at a fraction of the amplitude.
fn vnHash(c : vec3<i32>) -> f32 {
  return f32(pcg(u32(c.x * 374761393 + c.y * 668265263 + c.z * 1274126177)) &
             0xFFFFu) * (1.0 / 65535.0);
}
// Trilinear value noise over a lattice of `scale` voxels.
fn valueNoise(p : vec3f, scale : f32) -> f32 {
  let q = p / scale;
  let i = vec3<i32>(floor(q));
  var f = fract(q);
  f = f * f * (3.0 - 2.0 * f);   // smoothstep fade — no lattice creases
  let c000 = vnHash(i + vec3<i32>(0,0,0));
  let c100 = vnHash(i + vec3<i32>(1,0,0));
  let c010 = vnHash(i + vec3<i32>(0,1,0));
  let c110 = vnHash(i + vec3<i32>(1,1,0));
  let c001 = vnHash(i + vec3<i32>(0,0,1));
  let c101 = vnHash(i + vec3<i32>(1,0,1));
  let c011 = vnHash(i + vec3<i32>(0,1,1));
  let c111 = vnHash(i + vec3<i32>(1,1,1));
  let x00 = mix(c000, c100, f.x);
  let x10 = mix(c010, c110, f.x);
  let x01 = mix(c001, c101, f.x);
  let x11 = mix(c011, c111, f.x);
  return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

// Multiplicative brightness grain in roughly [1-amp, 1+amp].
fn surfaceGrain(cell : vec3<i32>, amp : f32) -> f32 {
  let p = vec3f(cell);
  // ~11 voxels (0.7 m) for the broad patches, ~2.5 voxels for the fine break-up.
  let broad = valueNoise(p, 11.0);
  let fine  = valueNoise(p, 2.5);
  let n = broad * 0.68 + fine * 0.32;
  return 1.0 + (n - 0.5) * 2.0 * amp;
}

// ---- voxel ambient occlusion ----
// The classic Minecraft-style per-vertex AO, evaluated per PIXEL because a
// raymarcher has no vertices: for the face we hit, sample the two tangent
// neighbours and the diagonal on the side the hit point leans toward, and
// darken by how many are solid. This is what puts a dark seam in every inside
// corner and under every overhang, and its absence is why the wooden frame
// looked like flat cardboard cutouts.
//
// Cost is 3 voxel fetches (plus 1 for the face-above term) on the primary hit
// only — reflections and shadow rays skip it entirely.
fn aoSolidAt(c : vec3<i32>) -> f32 {
  if (!inBounds(c)) { return 0.0; }   // unloaded space must not cast AO
  let w = voxels[cellIndexW(c)];
  let m = voxMat(w);
  if (m == MAT_AIR) { return 0.0; }
  // Only ray blockers occlude: smoke and shallow water must not stamp hard
  // AO shadows onto the terrain they touch.
  return select(0.0, 1.0, isRayBlocker(materials[m]));
}

// `uv` is the hit position's fractional offset within the face, in [0,1]^2
// along the two tangent axes (a1, a2).
fn voxelAO(cell : vec3<i32>, n : vec3<i32>, a1 : i32, a2 : i32, uv : vec2f) -> f32 {
  // The neighbour cell in front of the face — AO samples live in that plane, so
  // an occluder is something sitting beside the face, not inside the solid.
  let base = cell + n;
  // Pick the quadrant the hit leans into: this is what makes the darkening ramp
  // smoothly across the face instead of switching at the midpoint.
  let s1 = select(-1, 1, uv.x > 0.5);
  let s2 = select(-1, 1, uv.y > 0.5);
  var d1 = vec3<i32>(0); d1[a1] = s1;
  var d2 = vec3<i32>(0); d2[a2] = s2;

  let side1 = aoSolidAt(base + d1);
  let side2 = aoSolidAt(base + d2);
  let corner = aoSolidAt(base + d1 + d2);

  // Standard vertex-AO rule: two touching sides fully enclose the corner, so
  // the diagonal cannot lighten it.
  var occ = side1 + side2;
  if (side1 > 0.0 && side2 > 0.0) { occ = 3.0; } else { occ += corner; }

  // How strongly this pixel belongs to the chosen quadrant: at the face centre
  // the AO fades out, at the corner it is full. Without this the AO tiles as
  // four flat quadrants per voxel and reads as a checkerboard.
  let w1 = abs(uv.x - 0.5) * 2.0;
  let w2 = abs(uv.y - 0.5) * 2.0;
  let reach = clamp(max(w1, w2), 0.0, 1.0);

  // 0.55 at a fully enclosed corner — deep enough to read, shallow enough that
  // interiors don't crush to black.
  let ao = 1.0 - (occ / 3.0) * 0.45 * reach;
  return clamp(ao, 0.0, 1.0);
}

// ---- soft sun shadows ----
// The old shadow term was binary (`if (s.hit) { lambert = 0.0; }`), which gives
// razor-sharp shadow edges everywhere — the look of a point light in vacuum.
// The real sun subtends ~0.5 degrees, so shadow edges soften with the distance
// between blocker and receiver; that gradient is a strong depth cue and its
// absence makes shadows read as painted-on decals.
//
// ---- WHY THIS IS NOT A JITTERED CONE ----
// The obvious cheap trick — jitter the ray direction per pixel inside a cone
// and take one sample, letting neighbouring pixels integrate the penumbra — was
// tried here and is WRONG for voxel terrain. One binary sample per pixel cannot
// resolve a penumbra; it only dithers between fully lit and fully shadowed. On
// near-flat ground under a grazing sun, adjacent pixels then randomly hit or
// miss the next terrace step, and the result is per-pixel salt-and-pepper over
// every hillside. Measured: the jitter DOUBLED high-frequency luminance energy
// on foreground grass (mean |dL| between horizontally adjacent pixels went
// 2.33 -> 4.74) — that stipple was the "noise on the floor", not the palette
// and not the AO. Without temporal accumulation or many samples there is
// nothing to average it back out, so the noise is the final image.
//
// Instead: ONE deterministic ray along the exact sun direction, and take the
// softness from the geometry it already reports. A shadow edge's penumbra width
// grows with the distance between blocker and receiver, so `s.t` (how far the
// ray travelled before being blocked) is exactly the quantity a soft shadow
// needs — a contact shadow right at the surface stays crisp, and a shadow cast
// from far away goes soft. Same one-ray cost, no noise, and it is a closer
// model of the real effect than a cone of one sample ever was.
fn sunShadow(hp : vec3f, n : vec3f, px : vec2f) -> f32 {
  let s = trace(hp + n * 0.02, R.sunDir, 384, false);
  if (!s.hit) { return 1.0; }
  // Distance from receiver to blocker, in metres. Near blockers (a voxel
  // resting on the ground) keep a hard, dark contact shadow; distant ones (a
  // tree canopy over a meadow) soften and lift, which is what stops every
  // terrace step from stamping a hard black band onto the hillside.
  let dM = s.t * VOXEL_METERS;
  // A shadowed point keeps NO direct sun — the hemisphere ambient term is what
  // fills it in, and that is already occluded by AO. Letting direct sun leak
  // into shadow instead washes the whole scene out and erases the cast shadow
  // under overhangs. The softening is in the EDGE, not in the depth: a distant
  // blocker only partially covers the solar disc, so its shadow lifts toward
  // ~0.45 of full sun, while a contact shadow stays at 0.
  return clamp(smoothstep(0.6, 9.0, dM) * 0.45, 0.0, 1.0);
}

// ============================================================================
// WATER SURFACE (DESIGN.md §9)
// ============================================================================
// A translucent liquid is not fog. Before this pass water was shaded purely as
// participating media — a flat per-meter tint — which is why a lake read as a
// blue disc painted onto the terrain: no interface, so no reflection, no
// glint, no refraction, and no depth cue at all (the bed 24 voxels down was
// invisible). Real water gets its whole look from the AIR/WATER INTERFACE plus
// what happens to the light that makes it through:
//
//   1. a normal — smoothed from the fullness field, not the voxel face
//   2. ripples  — animated normal perturbation
//   3. Fresnel  — reflect vs refract, angle-dependent (this is the big one)
//   4. reflection — a real traced secondary ray, sky as fallback
//   5. refraction — the transmitted ray, bent, so the bed distorts
//   6. absorption — per-channel Beer-Lambert over the underwater path
//   7. glint    — a sharp specular lobe on the rippled normal
//
// All of it is render-only float math on render-only data. The sim never sees
// any of this and the world hash never covers it, so determinism rule #1 is
// untouched (it scopes to sim state — CLAUDE.md).

// Index of refraction, air -> water. Drives both the Schlick F0 below and the
// refract() bend.
const WATER_IOR : f32 = 1.333;
// Schlick F0 for that IOR: ((1-n)/(1+n))^2 = 0.0204. Water reflects only ~2%
// head-on but ~100% at grazing — that spread IS the look, and it's exactly
// what a constant tint cannot reproduce.
const WATER_F0 : f32 = 0.0204;

// Per-channel absorption, per METRE of path, for clear water. Red is absorbed
// roughly an order of magnitude faster than blue — that is why shallow water
// reads cyan-green and deep water reads deep blue, and it's the single
// strongest depth cue available. A scalar tint (what this shader used to do)
// is flat by construction no matter how it's tuned.
const WATER_ABSORB : vec3f = vec3f(1.85, 0.42, 0.20);
// Scattering back toward the eye — the color deep water TENDS toward rather
// than going black. Without it, depth just crushes to black and reads as a pit.
const WATER_SCATTER : vec3f = vec3f(0.045, 0.16, 0.20);

// ---- ripples ----
// Sum of directional waves evaluated in world XZ. Cheap gradient-of-a-height
// -field: each wave contributes its analytic slope, so there is no texture
// fetch and no normal map. Frequencies are deliberately non-harmonic (and the
// directions non-parallel) so the pattern never visibly tiles or beats.
//
// SCALE MATTERS: one voxel is VOXEL_METERS, so wave lengths are written in
// METRES and converted, exactly like the tree dimensions in worldgen. Writing
// them as bare voxel counts gives you either mirror-flat water or static.
struct Ripple { dir : vec2f, len : f32, amp : f32, speed : f32 };
const RIPPLE_BANDS : i32 = 5;

// `footM` is the width in METRES that one pixel covers on the surface here.
// Each wave is faded out once the footprint approaches its wavelength (i.e.
// once it can no longer be sampled), which is per-band mip selection done
// analytically. Pass 0 to disable damping.
fn rippleSlope(pWorldM : vec2f, t : f32, footM : f32) -> vec2f {
  var slope = vec2f(0.0);
  // len in metres, amp in metres of height. Four octaves is enough to read as
  // water; the two short ones carry the glint sparkle, the two long ones give
  // the surface a sense of swell so it isn't uniformly busy.
  // Amplitudes are deliberately SMALL relative to wavelength (steepness
  // amp*k stays ~0.03-0.04). Real calm water is very nearly flat: push the
  // steepness up and the perturbed normals start pointing at the shore
  // instead of at the sky, which collapses the Fresnel reflection into a
  // uniform dark green and reads as pond scum rather than water.
  // Steepness (amp*k) per band runs ~0.055 down to ~0.03. This is the setting
  // the look is most sensitive to and it is a narrow window:
  //   * too low  -> a flat sheet; the reflection is uniform and the sun
  //                 highlight fuses into one blown-out white slab
  //   * too high -> normals swing far enough to point at the shore, which
  //                 reads as choppy corrugated metal and crawls when animated
  // The 0.22 m band exists purely to break the sun highlight into sparkle;
  // it carries almost no relief of its own.
  var waves = array<Ripple, RIPPLE_BANDS>(
    Ripple(normalize(vec2f( 1.0,  0.35)), 2.60, 0.0230, 0.55),
    Ripple(normalize(vec2f(-0.42, 1.0 )), 1.70, 0.0135, 0.73),
    Ripple(normalize(vec2f( 0.78, -0.75)), 0.85, 0.0060, 1.15),
    Ripple(normalize(vec2f(-0.85, -0.5)), 0.48, 0.0030, 1.60),
    Ripple(normalize(vec2f( 0.30, -0.95)), 0.22, 0.0011, 2.30));
  for (var i = 0; i < RIPPLE_BANDS; i++) {
    let w = waves[i];
    let k = 6.28318 / w.len;                 // angular wavenumber
    let phase = dot(pWorldM, w.dir) * k + t * w.speed * k;
    // Per-band fade: full amplitude while the footprint is comfortably under
    // half a wavelength (Nyquist), gone by the time it exceeds it.
    var band = 1.0;
    if (footM > 0.0) { band = 1.0 - smoothstep(w.len * 0.28, w.len * 0.85, footM); }
    // d/dp of (amp * sin(phase)) = amp * k * cos(phase) * dir
    slope += w.dir * (w.amp * k * cos(phase)) * band;
  }
  return slope;
}

// ---- surface normal ----
// The voxel face normal is axis-aligned and would make a lake look like tiled
// glass. Instead take the GRADIENT OF THE FULLNESS FIELD: the liquid state
// nibble is fill level in eighths (DESIGN.md §4), so the liquid column height
// varies cell to cell and its gradient is the true macro slope of the surface
// — this is the standard scalar-field-gradient normal from the smooth-voxel
// literature, applied to data the sim already maintains for free.
//
// This is what makes a settling / flowing / wavy body of water read as a
// surface with shape rather than as a staircase, and it costs 4 taps.
fn liquidFullnessAt(c : vec3<i32>, mat : u32) -> f32 {
  if (!inBounds(c)) { return 0.0; }
  let w = voxels[cellIndexW(c)];
  if (voxMat(w) != mat) { return 0.0; }
  return f32(voxState(w) + 1u) / 8.0;
}

// Surface height (in voxels, relative to the cell floor) of the liquid column
// at XZ: full cells below stack, and the topmost partial cell adds its
// fullness. Sampling the column rather than one cell is what lets the gradient
// see a slope of more than one cell.
fn liquidColumn(c : vec3<i32>, mat : u32) -> f32 {
  // Walk up from the sample cell while cells stay full; the first non-full
  // cell contributes its fraction and ends the column.
  var h = 0.0;
  for (var i = 0; i < 3; i++) {
    let f = liquidFullnessAt(c + vec3<i32>(0, i, 0), mat);
    h += f;
    if (f < 0.999) { break; }
  }
  return h;
}

fn waterNormal(cell : vec3<i32>, mat : u32, axis : i32, sgn : f32,
               hitP : vec3f, upFacing : bool) -> vec3f {
  // Side/bottom faces of a liquid volume keep their flat voxel normal: the
  // fullness gradient describes the TOP surface, and applying it to a wall
  // would tilt it into the terrain.
  if (!upFacing) {
    var n = vec3f(0.0);
    n[axis] = -sgn;
    return n;
  }

  // Central differences of the column height across X and Z. dh/dx in voxels
  // per voxel is already a slope, so the normal is (-dh/dx, 1, -dh/dz).
  let hx0 = liquidColumn(cell + vec3<i32>(-1, 0, 0), mat);
  let hx1 = liquidColumn(cell + vec3<i32>( 1, 0, 0), mat);
  let hz0 = liquidColumn(cell + vec3<i32>(0, 0, -1), mat);
  let hz1 = liquidColumn(cell + vec3<i32>(0, 0,  1), mat);
  var slope = vec2f((hx0 - hx1) * 0.5, (hz0 - hz1) * 0.5);

  // Ripples on top of the macro slope, in world metres.
  let pm = vec2f(hitP.x, hitP.z) * VOXEL_METERS;
  // PER-WAVE DISTANCE DAMPING (the analytic stand-in for normal-map mipping).
  // One pixel far across a lake covers many wavelengths, so the ripples inside
  // it should average toward flat; without damping, distant water aliases into
  // crawling speckle — full-amplitude slope at a collapsed sampling rate,
  // exactly the undersampling a mip chain exists to fix.
  //
  // The footprint estimate must be the SCREEN-SPACE one, not raw distance.
  // Raw distance is a radial function about the camera, and multiplying the
  // ripple field by it stamps that radial function onto the water: the waves
  // visibly bend into concentric rings centred on the viewer, which moves with
  // the camera and is far worse than the aliasing it fixes. Grazing angle is
  // the other half of it — a surface seen edge-on has a footprint stretched
  // along the view direction no matter how near it is.
  let toEye = R.camPos - hitP;
  let distM = length(toEye) * VOXEL_METERS;
  // |cos| between the view ray and the surface normal-ish up axis: small at
  // grazing incidence, where the footprint blows up.
  let graze = max(abs(normalize(toEye).y), 0.06);
  // Footprint width in metres ~ distance * pixel angular size / graze.
  // R.viewPx is the render height in pixels, so tanHalfFov*2/viewPx is the
  // vertical angle one pixel subtends — the same quantity a mip LOD is chosen
  // from. Hardcoding a resolution here would make the water's apparent
  // choppiness change with window size.
  let footM = distM * (R.tanHalfFov * 2.0 / max(R.viewPx, 1.0)) / graze;
  // Damping is applied PER WAVE against its own wavelength inside
  // rippleSlope() — a 2.6 m swell stays visible long after 0.4 m chop has
  // averaged out, which is what gives distance a sense of scale. `gain` here
  // is just the footprint handed down.
  let gain = footM;
  // Ripple slope is metres-of-height per metre — same units as `slope` above
  // (voxels per voxel), so they add directly.
  slope += rippleSlope(pm, R.time, gain);

  return normalize(vec3f(-slope.x, 1.0, -slope.y));
}

// Sky-only reflection fallback plus a horizon-grounded haze, used when a
// reflected ray finds nothing. A raw skyColor() lookup below the horizon
// returns the ground-ward gradient, which reads as a bright band; clamping the
// reflected direction's downward component keeps distant reflections plausible.
fn reflectionSky(rd : vec3f) -> vec3f {
  var d = rd;
  if (d.y < 0.02) { d.y = 0.02; }
  return skyColor(normalize(d));
}

// ---- traced reflection ----
// The engine already has a DDA, so the reflection can be a REAL ray rather
// than a screen-space approximation: no missing-information artifacts at the
// screen edge, and objects behind the camera reflect correctly (Teardown's
// screen-space reflections explicitly can't do this). Budget is small — a
// reflected ray off water is nearly always either sky or the near shore, and
// this runs per water pixel.
fn traceReflection(p : vec3f, n : vec3f, rd : vec3f) -> vec3f {
  let rr = reflect(rd, n);
  // A reflected ray pointing DOWN came from a normal that the ripples tilted
  // past the view ray; it has nothing valid to reflect. Take the sky, but
  // BLEND across the horizon rather than switching at exactly rr.y == 0.
  //
  // A hard cutoff here is visible: at a grazing view the reflected rays sit
  // within a few degrees of horizontal, so the ripple field pushes adjacent
  // pixels back and forth across the threshold and the surface breaks into
  // per-pixel speckle instead of reading as one continuous sheet.
  let horizon = smoothstep(-0.06, 0.02, rr.y);
  if (horizon <= 0.0) { return reflectionSky(rr); }
  // Step budget, not distance: this is one secondary ray per water pixel and a
  // lake can fill the screen, so this number is a direct frame-time multiplier
  // on water-heavy views. Rays that run out return sky, which is what a long
  // unobstructed reflection was going to be anyway — the budget only truncates
  // reflections of things far across the water, where the aerial perspective
  // below has already faded them most of the way to sky.
  //
  // 96 is chosen against the geometry, not by taste: a reflected ray off
  // near-flat water leaves at a shallow angle and spends most of its steps
  // crossing empty air above the surface, where the chunk-skip advances it a
  // whole 16-cell chunk per iteration. 96 steps therefore reaches well past
  // the far shore of any pond-sized body while capping the pathological case —
  // a reflection grazing INTO dense canopy, where every step is a real voxel
  // step and the skip never fires. Media is off (`wantMedia = false`) so
  // reflected rays skip on the blocker count and smoke costs them nothing.
  let h = trace(p + n * 0.05, rr, 96, false);
  if (!h.hit) { return reflectionSky(rr); }

  // Shade the reflected hit with the same terms as a primary hit, minus the
  // secondary shadow ray (a shadow test on a reflection is invisible at this
  // budget and doubles the cost).
  let m = materials[voxMat(h.word)];
  var albedo = paletteColor(m, voxState(h.word));
  if (m.klass == CLASS_LIQUID) { albedo = unpackColor(m.color0); }
  var rn = vec3f(0.0);
  rn[h.axis] = -h.sgn;
  var face = 1.0;
  if (h.axis == 0) { face = 0.96; }
  else if (h.axis == 2) { face = 0.92; }
  if (m.klass != CLASS_LIQUID) { albedo *= surfaceGrain(h.cell, 0.065); }
  let lam = wrapDiffuse(dot(rn, R.sunDir), 0.55);
  // Same lighting model as a primary hit (minus AO and the shadow ray, which
  // are not resolvable in a reflection at this budget).
  var c = albedo * face * (ambientAt(rn) + vec3f(1.0, 0.95, 0.86) * lam * 0.70);
  let emis = f32(m.emission) / 255.0;
  if (emis > 0.0) { c += albedo * emis * 1.7; }
  // Reflected geometry is seen across the water plus its own distance, so it
  // takes aerial perspective too — without this, a reflected far hillside is
  // sharper than the real one and the reflection reads as a decal.
  return mix(reflectionSky(rr), applyAerial(c, rr, h.t), horizon);
}

// ---- the full water shade ----
// `sceneBehind` is whatever the primary march already resolved BEHIND the
// water (lake bed, terrain, or sky) — this function decides how much of it
// survives the trip back up through the water, and what covers the rest.
//
// Returns the final color for a pixel whose primary ray crossed a water
// surface. `underwater` flips the treatment: from below there is no sky to
// reflect and the absorption applies to the whole view, not just the depth.
fn shadeWater(hitP : vec3f, rd : vec3f, mat : u32, cell : vec3<i32>,
              axis : i32, sgn : f32, pathVox : f32, surfFull : f32,
              sceneBehind : vec3f, tSurf : f32, underwater : bool) -> vec3f {
  let m = materials[mat];
  // Up-facing means the ray entered through the TOP of the liquid — the only
  // face that gets the fullness-gradient normal and the sky reflection.
  let upFacing = (axis == 1 && sgn < 0.0);
  var n = waterNormal(cell, mat, axis, sgn, hitP, upFacing);
  if (underwater) { n = -n; }   // seen from below, the interface faces down

  let v = -rd;                                  // toward the eye
  let cosI = clamp(dot(n, v), 0.0, 1.0);

  // ---- Fresnel (Schlick) ----
  // The single most important term. At 2% head-on and ~100% at grazing, this
  // is what makes water look wet: you see THROUGH it at your feet and see the
  // SKY in it at the far shore, across one continuous surface.
  var fres = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - cosI, 5.0);
  // Non-water liquids (oil, acid, blood) are dielectrics too but far more
  // absorbing; they get the same interface with a muted reflection so they
  // read as their own substance rather than all becoming "water".
  let isWater = (m.tagMask != 0u) && (f32(m.opacity) / 255.0 < 0.45);
  if (!isWater) { fres *= 0.55; }
  // A partially-filled surface cell is a thin film / spray, not a mirror:
  // fade the specular interface out with fullness so a 1/8 puddle skin doesn't
  // reflect the sky as hard as a lake does.
  fres *= mix(0.35, 1.0, surfFull);
  if (!upFacing) { fres *= 0.5; }   // side walls: glancing, but not mirrors

  // ---- transmitted light: per-channel Beer-Lambert ----
  // Depth in METRES, so the look is independent of voxel size. Absorption is
  // applied per channel, so the bed goes green-cyan then blue-black with depth
  // instead of uniformly darkening. Path length is doubled for the down-and-
  // back trip only when we can see a bed; for an unbounded view the march
  // already accumulated the true path.
  let depthM = max(pathVox, 0.0) * VOXEL_METERS;
  var absorbK = WATER_ABSORB;
  var scatter = WATER_SCATTER;
  if (!isWater) {
    // Other liquids: derive the absorption from their authored opacity and
    // palette so oil stays black-brown and acid stays acid-green, without
    // hardcoding material IDs (CLAUDE.md conventions).
    let base = (unpackColor(m.color0) + unpackColor(m.color1)) * 0.5;
    let k = (f32(m.opacity) / 255.0) * 9.0;
    // absorb the COMPLEMENT of the material color: a green liquid must absorb
    // red and blue, which is what leaves it looking green at depth
    absorbK = (vec3f(1.0) - base) * k + vec3f(0.05);
    scatter = base * 0.22;
  }
  let trans = exp(-absorbK * depthM);

  // ---- caustics ----
  // Sunlight refracting through the wave surface focuses into the bright
  // shifting web everyone recognises on a lake bed, and its absence is a
  // strong "this is fake" cue even when the absorption is right.
  //
  // Proper caustics need photon transport; the standard real-time cheat is
  // that the caustic intensity tracks the CONVERGENCE of the refracted rays,
  // and for a small-slope surface that convergence is the curvature of the
  // wave height field. Sampling the ripple slope at two nearby points and
  // taking the difference gives that curvature for a couple of extra ALU ops
  // and no new data.
  //
  // Projected along the SUN direction, not straight down, so the pattern
  // shifts across the bed with the sun's angle instead of being pinned under
  // the waves that cast it.
  var lit = sceneBehind;
  if (!underwater && depthM > 0.02) {
    // where on the surface the sunlight entering this bed point came from
    let bedP = hitP + rd * (pathVox);
    let drift = R.sunDir.xz / max(R.sunDir.y, 0.25) * (depthM / VOXEL_METERS);
    let cp = (vec2f(bedP.x, bedP.z) - drift) * VOXEL_METERS;
    // Difference over a WIDE baseline and damp out the short bands. Caustic
    // webs come from the long swell — a crest metres across is what has the
    // focal length to reach the bed. Differencing at the scale of the 22 cm
    // chop instead samples curvature that focuses far below any real bed and
    // renders as a fine dotted grid of per-pixel noise, not a caustic.
    // Passing a footprint of 0.5 m mutes every band under ~0.6 m, leaving the
    // 2.6 m and 1.7 m swell to drive the pattern.
    let e = 0.22;    // metres — finite-difference baseline for curvature
    let cf = 0.5;    // metres — band damping footprint
    let s0 = rippleSlope(cp, R.time, cf);
    let sx = rippleSlope(cp + vec2f(e, 0.0), R.time, cf);
    let sz = rippleSlope(cp + vec2f(0.0, e), R.time, cf);
    // divergence of the slope field = Laplacian of the height field. Negative
    // curvature (a wave crest acting as a converging lens) is the bright case.
    let curv = ((sx.x - s0.x) + (sz.y - s0.y)) / e;
    // Focusing strength grows with depth (longer lever arm from surface to
    // bed) then saturates — deep water's caustics wash out as the light
    // scatters, and unbounded growth would blow the bed out to white.
    let focus = clamp(depthM * 1.5, 0.0, 1.4);
    let caustic = max(-curv, 0.0) * focus;
    // MULTIPLICATIVE on the bed, not additive. Caustics are a redistribution
    // of the sunlight already landing on the bed, so they scale what is there:
    // bright sand goes brighter, dark stone stays dark. Adding a constant
    // instead lights up the water itself, which reads as glowing blobs
    // floating in the volume rather than light playing over a surface.
    lit *= 1.0 + min(caustic * 1.5, 0.85);
  }

  // What comes back up: the bed (plus its caustics), filtered by the water
  // column, plus the column's own in-scattered light (which is what keeps
  // deep water blue rather than black).
  var refracted = lit * trans + scatter * (vec3f(1.0) - trans);

  // ---- reflection ----
  var reflection : vec3f;
  if (underwater) {
    // From below, the sky is compressed into Snell's window and everything
    // outside it is total internal reflection of the murk. Approximating that
    // with the in-scatter color is both cheap and closer than a sky lookup.
    reflection = scatter * 1.6;
  } else if (upFacing) {
    // Only spend a secondary ray where the reflection can actually be SEEN.
    // Fresnel runs from 2% head-on to ~100% at grazing, so a top-down water
    // pixel is ~98% refracted and a traced reflection changes it by less than
    // the dither — while costing exactly as much as a grazing pixel where the
    // reflection is the whole image. Below the cutoff, take the sky: at that
    // weight the difference between a real reflection and a sky sample is not
    // resolvable, and this is what stops a screenful of water from firing a
    // full-budget ray per pixel for no visible return.
    if (fres > 0.06) {
      reflection = traceReflection(hitP, n, rd);
    } else {
      reflection = reflectionSky(reflect(rd, n));
    }
  } else {
    reflection = reflectionSky(reflect(rd, n));
  }

  var color = mix(refracted, reflection, fres);

  // ---- sun glint ----
  // Sharp Blinn-Phong lobe on the RIPPLED normal. This is what turns a
  // correct-but-dull surface into something that reads as water in motion:
  // the ripple slopes scatter the highlight into moving sparkle rather than
  // one blob. Gated on the sun being above the horizon relative to the normal.
  if (upFacing && !underwater) {
    let hv = normalize(R.sunDir + v);
    // Roughen the lobe with distance to match the ripple damping in
    // waterNormal(): those flattened far normals would otherwise all agree and
    // collapse the sparkle into one hard mirror disc. Widening the lobe as the
    // ripples fade spreads that energy back out into a glitter path, which is
    // what a real sun track on water looks like.
    let distM = length(hitP - R.camPos) * VOXEL_METERS;
    // Keep the lobe TIGHT with distance rather than widening it. Widening it
    // was wrong: as the ripple damping flattens the far normals they all agree,
    // and a broad lobe over agreeing normals integrates into one blown-out
    // white slab across the sun track instead of a glitter path. A narrow lobe
    // on flat water gives a small bright highlight, which is correct.
    let power = mix(180.0, 900.0, clamp(distM / 40.0, 0.0, 1.0));
    let spec = pow(max(dot(n, hv), 0.0), power);
    // Modulated by Fresnel so the glint follows the same angular law as the
    // reflection instead of floating on top of it. Capped: the highlight is a
    // bloom cue, not a light source, and letting it run to 2.6x saturates a
    // whole band of the lake to flat white and destroys the ripple detail
    // underneath it.
    color += vec3f(1.0, 0.97, 0.88) * min(spec, 1.0) * 0.85 * (0.25 + fres);
  }

  // ---- shoreline foam ----
  // Where the water column is thin, it is meeting the bed — a shore or a
  // sandbar. Real shorelines break there. A touch of brightening keyed on
  // shallowness, broken up by the ripple field so it isn't a clean contour
  // ring, sells the boundary between water and land far better than the hard
  // color step it replaces.
  if (upFacing && !underwater) {
    let shallow = 1.0 - smoothstep(0.0, 0.42, depthM);
    if (shallow > 0.0) {
      let pm = vec2f(hitP.x, hitP.z) * VOXEL_METERS;
      // reuse the ripple field as the foam mask so foam moves with the waves
      // (undamped: foam is a shoreline feature, always near the camera, and
      // damping it would dissolve the far shore's foam line)
      let s = rippleSlope(pm, R.time, 0.0);
      let mask = smoothstep(0.010, 0.055, length(s));
      color = mix(color, vec3f(0.92, 0.95, 0.97), shallow * mask * 0.55);
    }
  }

  return color;
}

// ============================================================================
// MOLTEN SURFACES (DESIGN.md §9)
// ============================================================================
// Lava is the OPPOSITE problem to water, and reusing the water treatment would
// get it wrong in every particular. Water's look comes from what it REFLECTS
// and TRANSMITS — Fresnel, refraction, depth absorption. Lava is opaque
// (MATF_OPAQUE, so it resolves as a surface hit and never enters the media
// path at all) and its look comes almost entirely from what it EMITS. There is
// no reflection worth tracing, nothing behind it to refract, and no depth to
// absorb through.
//
// Before this pass, lava was: flat palette albedo, one per-cell random
// flicker, added uniformly at emission 215/255 * 1.7 = 1.43x. Every channel
// saturated, so a lava pool rendered as a featureless WHITE slab — brighter
// than the sky and with less structure than the grass around it. Turning the
// intensity down alone would only have produced a flat ORANGE slab: the
// problem is the absence of spatial structure, not the exposure.
//
// What actually makes molten rock read as molten rock:
//   1. a CRUST — real flows are dark basaltic plates with glowing cracks
//      between them, not uniform orange. This is the whole look.
//   2. a blackbody ramp — black -> red -> orange -> yellow -> white, driven
//      by temperature rather than by palette index
//   3. flow — the crust drifts and the cracks shear open and closed
//   4. tonemapping — so "hotter" reads as a colour shift, not as clipping
//   5. light spill onto the surroundings, so the pool lights its own rim
//
// Render-only float math on render-only data: the sim never sees any of this
// (CLAUDE.md rule 1 scopes to sim state).

// Blackbody-ish ramp for incandescent rock, parameterised on normalised
// temperature 0..1. Not a Planck-law fit: it is anchored on the AUTHORED
// palette so a modder retinting lava in materials.json still gets a coherent
// heat ramp, which the data-driven-materials convention requires. c2/c0/c1 are
// the cool/mid/hot palette entries by convention for emissive materials.
fn moltenRamp(m : Material, temp : f32) -> vec3f {
  let t = clamp(temp, 0.0, 1.0);
  // Below the incandescence threshold the rock is genuinely dark — this is the
  // crust, and it must be allowed to go nearly black or there are no plates,
  // only a bright field with darker bits.
  let dark = vec3f(0.055, 0.040, 0.038);
  let cool = unpackColor(m.color1);   // deep red
  let mid  = unpackColor(m.color0);   // orange
  let hot  = unpackColor(m.color2);   // yellow-orange
  // The top of the ramp stays SATURATED. It is tempting to drive it to white
  // here, but the per-channel tonemap in fs() already desaturates bright
  // values on its own — pushing the ramp toward white as well compounds the
  // two and the crack cores come out bleached to grey-white, losing the hue
  // that made them read as molten. Let the hottest authored colour be the
  // hottest colour, and let the tonemap decide how white it looks.
  let white = mix(hot, vec3f(1.0, 0.86, 0.55), 0.35);
  // Band boundaries are pushed LATE on purpose. Most of a real flow's visible
  // area is crust and cooling red rock; the orange-and-above range belongs to
  // the crack cores alone, which are a small fraction of the surface. Spread
  // the bands evenly instead and the average pixel lands in the orange/yellow
  // part of the ramp, which — compounded by the tonemap's per-channel
  // desaturation on the way up — renders the whole pool as glowing gold
  // honeycomb rather than molten rock.
  var c = mix(dark, cool, smoothstep(0.00, 0.38, t));
  c = mix(c, mid,   smoothstep(0.48, 0.78, t));
  c = mix(c, hot,   smoothstep(0.80, 0.94, t));
  c = mix(c, white, smoothstep(0.95, 1.00, t));
  return c;
}

// ---- the crust field ----
// A value in 0..1 across the surface: 0 = cooled plate, 1 = molten crack.
//
// Built from layered value noise in WORLD metres (so plates are a fixed
// physical size regardless of voxel scale), ADVECTED so the whole crust drifts
// like a skin being dragged along. The crack network is the ridge transform
// (1 - |2n-1|), which is what turns smooth blobs into the thin branching
// filaments that read as fractures between plates; smooth noise alone gives
// soft mottling that reads as rust.
fn hashNoise2(p : vec2f) -> f32 {
  let i = floor(p);
  let f = fract(p);
  // Quintic smoothstep — C2 continuous, so the derivative used by the glow
  // gradient below doesn't show the lattice.
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let a = f32(pcg(u32(i32(i.x) * 374761393 + i32(i.y) * 668265263)) & 0xFFFFu) / 65535.0;
  let b = f32(pcg(u32(i32(i.x + 1.0) * 374761393 + i32(i.y) * 668265263)) & 0xFFFFu) / 65535.0;
  let c = f32(pcg(u32(i32(i.x) * 374761393 + i32(i.y + 1.0) * 668265263)) & 0xFFFFu) / 65535.0;
  let d = f32(pcg(u32(i32(i.x + 1.0) * 374761393 + i32(i.y + 1.0) * 668265263)) & 0xFFFFu) / 65535.0;
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Ridged multi-octave noise -> crack network. Returns 0..1, high in cracks.
fn crustCracks(pm : vec2f, t : f32) -> f32 {
  // Advection: each octave drifts at its own rate and direction, so the crust
  // shears rather than sliding rigidly. Slow — lava is viscous, and anything
  // fast enough to notice per-frame reads as boiling water, not rock.
  var v = 0.0;
  var amp = 1.0;
  var norm = 0.0;
  // Base frequency in cycles per metre. Plates want to be roughly fist- to
  // head-sized; at 0.55 they came out metres across and the pool read as
  // polished marble rather than crust.
  var p = pm * 2.4;
  var drift = vec2f(0.031, -0.019);
  for (var i = 0; i < 4; i++) {
    let n = hashNoise2(p + drift * t);
    // ridge transform: peaks become creases
    let ridge = 1.0 - abs(n * 2.0 - 1.0);
    v += ridge * ridge * amp;
    norm += amp;
    amp *= 0.5;
    p = p * 2.07 + vec2f(11.3, 7.7);   // non-integer lacunarity: no re-tiling
    drift = vec2f(-drift.y, drift.x) * 1.35;
  }
  return v / max(norm, 1e-5);
}

// ---- the full molten shade ----
// Returns the emitted colour of a molten surface cell, in linear HDR (values
// well above 1 are expected and are handled by the tonemap in fs()).
fn shadeMolten(m : Material, cell : vec3<i32>, hitP : vec3f, n : vec3f,
               rd : vec3f) -> vec3f {
  // Plates live in the horizontal plane for a pool surface; for a wall of
  // lava, project onto whichever plane the face points out of, so a vertical
  // flow gets vertical structure instead of a smeared top-down pattern.
  var pm : vec2f;
  if (abs(n.y) > 0.5) { pm = vec2f(hitP.x, hitP.z); }
  else if (abs(n.x) > 0.5) { pm = vec2f(hitP.z, hitP.y); }
  else { pm = vec2f(hitP.x, hitP.y); }
  pm *= VOXEL_METERS;

  let cracks = crustCracks(pm, R.time);

  // Map the crack field to temperature with a SHARP knee. The knee is what
  // separates plate from crack: a soft ramp gives a uniformly warm surface
  // with no plate boundaries, which is the failure mode this whole function
  // exists to avoid.
  // The knee sits HIGH so that most of the surface falls below it and stays
  // crust. Lowering it floods the pool with melt and the plates stop reading
  // as plates — the dark area between cracks is what gives the glow something
  // to be brighter *than*.
  var temp = smoothstep(0.52, 0.93, cracks);

  // Per-cell variation so two adjacent plates are not the same temperature —
  // some crust is freshly congealed and still glowing, some is old and dark.
  // Keyed on the CELL (stable as the camera moves), coarsened to ~4-voxel
  // patches so it reads as plate-scale variation and not per-voxel noise.
  let pc = cell >> vec3<u32>(2u);
  let ph = pcg(u32(pc.x * 7 + pc.y * 131 + pc.z * 2917));
  temp += (f32(ph & 0xFFu) / 255.0 - 0.5) * 0.22;

  // Slow per-cell pulse: convection turning fresh melt over. Distinct phase
  // per patch so the pool does not beat in unison (same reasoning as the fire
  // flicker, which this deliberately does NOT reuse — fire flickers fast and
  // randomly, lava breathes).
  temp += 0.06 * sin(R.time * 0.9 + f32(ph & 0x3FFu) * 0.0061);

  // Top faces are the coolest (they radiate to the sky and skin over first);
  // the sides of a flow are freshly exposed melt and run hotter. This is a
  // real effect and it also usefully breaks up the silhouette of a pool.
  if (n.y > 0.5) { temp -= 0.10; }
  else { temp += 0.12; }

  temp = clamp(temp, 0.0, 1.0);

  var c = moltenRamp(m, temp);

  // Emission scales STEEPLY with temperature (Stefan-Boltzmann is T^4; the
  // exponent here is tuned rather than physical, but the point is the same —
  // the cracks must out-radiate the plates by a large factor, or the surface
  // averages back out into the flat slab this replaces).
  let emis = f32(m.emission) / 255.0;
  // Peak intensity is bounded on purpose. The steep exponent is what makes
  // cracks out-radiate plates, but run it to 3.4x and the crack cores land
  // deep in the tonemap's shoulder, where per-channel compression bleaches
  // them to white and the hue is lost exactly where the surface is most
  // interesting. ~1.9x peak keeps the cores inside the range where the
  // shoulder still discriminates colour.
  let power = temp * temp * (0.35 + 2.2 * temp);
  c *= 0.16 + power * emis * 1.9;

  // Fresnel-ish rim: a glancing view of any surface catches more of its
  // emission, and on a pool this draws a hot lip around the far edge that
  // makes it read as a volume of liquid rather than a painted disc.
  let grazing = 1.0 - abs(dot(n, -rd));
  c += moltenRamp(m, min(temp + 0.25, 1.0)) * pow(grazing, 3.0) * emis * 0.55;

  return c;
}

struct FSOut {
  @location(0) color : vec4f,
  @builtin(frag_depth) depth : f32,
};

@fragment
fn fs(in : VSOut) -> FSOut {
  let ndc = in.uv;
  let rd = normalize(R.camFwd
                   + R.camRight * (ndc.x * R.tanHalfFov * R.aspect)
                   + R.camUp    * (ndc.y * R.tanHalfFov));

  let h = trace(R.camPos, rd, 4096, true);

  // Rays that leave the window without a surface hit (and weren't absorbed by
  // media) continue into the far-field cascades from the window's exit point.
  var far : FarHit;
  far.hit = false;
  if (!h.hit && !h.saturated) {
    // in.pos.xy is the fragment's pixel coordinate — the dither key (see
    // farDither: screen-space, time-free, stable per pixel)
    far = traceFar(R.camPos, rd, h.tExit, in.pos.xy);
  }

  // reversed-Z depth so raster geometry (particles/debris) composites in.
  // A saturated media march writes depth at its stop point: the smoke is
  // opaque there, and raster geometry behind it must not draw through.
  var depth = 0.0;  // sky = far
  if (h.hit || h.saturated) {
    let viewZ = h.t * dot(rd, R.camFwd);
    depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  } else if (far.hit) {
    let viewZ = far.t * dot(rd, R.camFwd);
    depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  }

  var color : vec3f;
  if (!h.hit) {
    if (far.hit) {
      // far-field shading (phase 4): same palette, face term, sun tint, and
      // ambient floor as the near field, plus cascade-marched sun shadows and
      // a one-sample AO — lighting mismatch at the window seam is what makes
      // LOD terrain read as a different world.
      let m = materials[far.mat];
      // palette jitter at a FIXED world frequency (~0.5 m patches) instead of
      // per level cell: coarse cells otherwise flatten into single-color slabs
      // and the texture contrast visibly drops at every LOD handoff.
      let jc = (far.cell << vec3<u32>(farCellShift(far.level))) >> vec3<u32>(3u);
      let jit = pcg(u32(jc.x * 7 + jc.y * 131 + jc.z * 2917));
      var albedo = paletteColor(m, jit);
      var n = vec3f(0.0);
      n[far.axis] = -far.sgn;
      if (m.klass == CLASS_LIQUID) {
        albedo = unpackColor(m.color0);
        // distant water: a touch of sky reflection on up-facing surfaces so
        // lakes read as water instead of flat blue paint
        if (n.y > 0.5) { albedo = mix(albedo, skyColor(reflect(rd, n)), 0.35); }
      }
      // Match the near field's face weights exactly — a different constant
      // here is a visible brightness step at the window seam.
      var face = 1.0;
      if (far.axis == 0) { face = 0.96; }
      else if (far.axis == 2) { face = 0.92; }
      albedo *= surfaceGrain(far.cell << vec3<u32>(farCellShift(far.level)), 0.05);
      // Same wrapped diffuse as the near field — a different falloff here is a
      // visible brightness step at the window seam.
      var lambert = wrapDiffuse(dot(n, R.sunDir), 0.55);
      if (lambert > 0.0 && (R.flags & 1u) != 0u) {
        // start the shadow march just off the hit face, in fine-voxel coords
        let hp = R.camPos + rd * (far.t - 1e-3) +
                 n * (0.55 * f32(1u << farCellShift(far.level)));
        // SOFT, not hard-zero: at cascade resolution most shadow casters are
        // single-cell terrace steps and canopy rings, and a hard shadow term
        // turns them into high-frequency dark speckle ("ant trails") across
        // every hillside. 0.3 keeps the form cue without the noise.
        if (farShadowed(far.level, hp)) { lambert *= 0.3; }
      }
      // one-sample AO: an occupied cell directly above darkens — valley floors
      // and ground under flattened canopy stop rendering at full sky ambient
      var ao = 1.0;
      let up = far.cell + vec3<i32>(0, 1, 0);
      if (farInBox(up, F.origins[far.level - 1u].xyz) &&
          farMatAt(far.level, up) != 0u) { ao = 0.72; }
      // Same lighting model as the near field (hemisphere ambient x AO, plus
      // direct sun) so the two representations agree across the seam.
      let fsun = vec3f(1.0, 0.95, 0.86) * lambert * 1.35;
      color = albedo * face * (ambientAt(n) * ao + fsun);
      let emis = f32(m.emission) / 255.0;
      if (emis > 0.0) { color += albedo * emis * 1.7; }
      if (h.liqT <= 0.0) { color = applyAerial(color, rd, far.t); }
      else { color = applyAerial(color, rd, far.t - h.liqT); }
    } else {
      color = skyColor(rd);
    }
  } else {
    let mat = voxMat(h.word);
    let m = materials[mat];
    var albedo = paletteColor(m, voxState(h.word));
    if (m.klass == CLASS_LIQUID) {
      // liquid state nibble is fullness, not a palette variant: fuller = deeper
      let fullness = f32(voxState(h.word) + 1u) / 8.0;
      albedo = mix(unpackColor(m.color2), unpackColor(m.color0), fullness);
    }

    var n = vec3f(0.0);
    n[h.axis] = -h.sgn;

    // Voxel-scale grain: breaks up the white-noise palette confetti into
    // correlated patches (see surfaceGrain). Liquids are excluded — their state
    // nibble is fullness, not a palette variant, and graining a lake surface
    // fights the ripple normals.
    if (m.klass != CLASS_LIQUID) {
      albedo *= surfaceGrain(h.cell, 0.065);
    }

    // ---- ambient occlusion ----
    // Needs the hit point's position within the face, so build it from the
    // exact hit and take the two axes tangent to the face normal.
    let hp = R.camPos + rd * h.t;
    let ni = vec3<i32>(round(n));
    let a1 = select(0, 1, h.axis == 0);            // first tangent axis
    let a2 = select(2, 1, h.axis == 2);            // second tangent axis
    let uv = vec2f(fract(hp[a1]), fract(hp[a2]));
    let ao = voxelAO(h.cell, ni, a1, a2, uv);

    // Per-face constant: kept, but much gentler than the old 0.55/0.75/0.85
    // spread. That spread was doing the job real ambient should do, and doing
    // it wrong — it darkened every north-facing wall by a fixed 25% regardless
    // of where the sky actually was. Now the hemisphere ambient carries the
    // directional term and this only breaks the tie between the two horizontal
    // axes so parallel walls don't fuse.
    var face = 1.0;
    if (h.axis == 0) { face = 0.96; }
    else if (h.axis == 2) { face = 0.92; }

    // Wrapped diffuse (see wrapDiffuse): keeps the risers of the terrain
    // staircase within a few percent of their tops instead of 1.8x apart.
    // wrap = 1.0 is deliberately wide. Measured on a grass hillside, the old
    // hard Lambert produced a BIMODAL luminance histogram — a cluster at 88-112
    // (risers) and a separate spike at 208 (sun-facing tops) with the 144-176
    // range completely empty. Two disjoint populations interleaved at voxel
    // frequency is what the eye reports as "noise on the floor"; there was no
    // gradient between them to read as slope. Widening the wrap fills that gap.
    var lambert = wrapDiffuse(dot(n, R.sunDir), 0.55);
    if (lambert > 0.0 && (R.flags & 1u) != 0u) {
      lambert *= sunShadow(R.camPos + rd * (h.t - 1e-3), n, in.pos.xy);
    }
    // Direct sun + hemisphere ambient. Ambient is occluded by AO (it is sky
    // light, and AO measures how much sky the point can see); direct sun is
    // NOT — it already has its own shadow ray, and multiplying it by AO too
    // double-darkens contact regions into black smears.
    let sun = vec3f(1.0, 0.95, 0.86) * lambert * 1.35;
    color = albedo * face * (ambientAt(n) * ao + sun);

    // ---- emissive surfaces ----
    // MOLTEN materials (an emissive OPAQUE liquid — lava, molten glass) take
    // the crust treatment: they REPLACE the diffuse shade rather than adding
    // to it, because an incandescent surface is not lit by the sun in any
    // meaningful sense, it is its own light source. Adding emission on top of
    // a sun-lit albedo is exactly what saturated every channel and turned a
    // lava pool into a white slab.
    //
    // Detected by class + flags + emission, never by material ID: any modder's
    // emissive opaque liquid gets this for free (CLAUDE.md conventions).
    let emis = f32(m.emission) / 255.0;
    let isMolten = m.klass == CLASS_LIQUID &&
                   (m.flags & MATF_OPAQUE) != 0u && m.emission > 0u;
    if (isMolten) {
      color = shadeMolten(m, h.cell, R.camPos + rd * h.t, n, rd);
    } else if (emis > 0.0) {
      // everything else emissive (fire, embers) keeps the fast random flicker
      let ch = pcg(u32(h.cell.x * 7 + h.cell.y * 131 + h.cell.z * 2917));
      let flick = 0.82 + 0.28 * sin(R.time * 9.0 + f32(ch & 0xFFu) * 0.0245);
      color += albedo * emis * 1.7 * flick;
    }

    // distance fog (density per meter, so the look survives voxel-size
    // changes). Density is a uniform tracking the far field's currently
    // FILLED radius (FarField::SafeRadiusMeters, plan phase 3B) so the
    // cascade horizon fades out instead of ending in a cut — and so a
    // half-filled cascade fogs out before its empty bands become visible.
    //
    // SKIPPED when the ray crossed a water surface: below the surface it is
    // water absorbing the light, not air, and shadeWater() models that with
    // Beer-Lambert instead. The air path in front of the water still gets
    // fogged — once, at the surface distance, after shadeWater() runs. Fogging
    // here as well would double-count it and wash the lake bed out to sky
    // color, which is exactly the haze that hides the bottom.
    if (h.liqT <= 0.0) { color = applyAerial(color, rd, h.t); }
  }

  // ---- gas tint along the ray ----
  // Gases stay pure participating media: smoke and steam have no interface to
  // reflect off, so the accumulated tau/tint is the whole story. mediaTau and
  // mediaTint accumulate per cell in trace(), so a ray crossing fire INTO
  // smoke shades each stretch with its own material instead of painting the
  // whole path with the first one.
  //
  // Liquids used to be tinted here too, and that is precisely why water looked
  // like blue fog — an absorbing volume with no surface. They now take the
  // shadeWater() path below instead.
  if (h.mediaMat != 0u && materials[h.mediaMat].klass == CLASS_GAS) {
    let mm = materials[h.mediaMat];
    var mc = (unpackColor(mm.color0) + unpackColor(mm.color1)) * 0.5;
    if (h.mediaTau > 1e-5) { mc = h.mediaTint / h.mediaTau; }
    let tau = h.mediaTau * VOXEL_METERS * MEDIA_ABSORB;
    let a = 1.0 - exp(-tau);
    color = mix(color, mc, a);
  }

  // ---- water surface ----
  // Everything the primary march resolved so far (bed, terrain, or sky, with
  // gas tint applied) is what sits BEHIND the water; shadeWater decides how
  // much of it survives the trip back up and what covers the rest.
  if (h.liqT > 0.0) {
    let lm = voxMat(voxels[cellIndexW(h.liqCell)]);
    // Re-read defensively: a cell can have changed class between trace and
    // shade only if the material table moved under us (hot reload), and a
    // stale id would index the wrong absorption.
    if (lm != MAT_AIR && materials[lm].klass == CLASS_LIQUID) {
      let hitP = R.camPos + rd * h.liqT;
      // Camera inside the liquid: the surface was entered at t~0 through no
      // real face, and absorption applies to the entire view rather than to a
      // bounded depth.
      let underwater = h.liqT < 0.05;
      color = shadeWater(hitP, rd, lm, h.liqCell, h.liqAxis, h.liqSgn,
                         h.liqPath, max(h.mediaSurf, 0.125), color,
                         h.liqT, underwater);
      // The water surface itself is at liqT, nearer than whatever is behind
      // it, so aerial perspective applies from the SURFACE — otherwise a
      // distant lake gets the fog of its own bed and reads too hazy.
      color = applyAerial(color, rd, h.liqT);
    }
  }

  // fire glow: additive, from the flicker-weighted emissive path. Intensity
  // drives a temperature ramp across the material palette — stray flame
  // voxels stay wispy deep-orange, plume cores saturate toward white-hot.
  if (h.fireMat != 0u && h.fireGlow > 0.0) {
    let fm = materials[h.fireMat];
    let x = 1.0 - exp(-h.fireGlow * 1.4);
    var fc = mix(unpackColor(fm.color2), unpackColor(fm.color0),
                 clamp(x * 2.0, 0.0, 1.0));
    fc = mix(fc, unpackColor(fm.color1) * 1.25 + vec3f(0.10, 0.06, 0.0),
             clamp(x * 2.0 - 1.0, 0.0, 1.0));
    // slow global breathing on top of the per-cell flicker baked into fireGlow
    let breathe = 0.92 + 0.08 * sin(R.time * 5.3);
    color += fc * x * 2.1 * breathe;
  }

  // ---- tonemap ----
  // The renderer works in linear HDR and emissive surfaces legitimately exceed
  // 1.0 by a wide margin, so the output curve has to COMPRESS the highlights
  // rather than clip them. The previous `pow(color, 1/2.2)` was a bare gamma
  // curve: everything over 1.0 clamped flat, which meant a hot surface lost
  // all its colour AND all its structure at exactly the moment it got
  // interesting. It is half the reason lava rendered as a white slab (the
  // other half being that emission was added on top of a lit albedo).
  //
  // Reinhard-with-white-point, applied to LUMINANCE and then reapplied to the
  // colour, rather than per channel.
  //
  // Per-channel Reinhard desaturates catastrophically and at ALL intensities,
  // not just bright ones: the 1/(1+c) denominator compresses a strong channel
  // far harder than a weak one, so a saturated ember orange (#ff5a1a, sat
  // 0.90) comes out as tan (sat 0.59) even at half exposure. Every warm
  // emissive surface in the scene — lava cracks, fire, embers — turned gold.
  // Scaling by the luminance ratio instead preserves the authored hue exactly.
  //
  // A CONTROLLED amount of the per-channel behaviour is still wanted at the
  // very top, because genuine blackbody progression does shift toward white as
  // things get hotter. So blend a little of it back in, weighted by how far
  // into the shoulder the pixel sits: hue is preserved through the midtones,
  // and only the truly hot cores bleach.
  const WHITE : f32 = 4.2;   // intensity that maps to display white
  color = max(color, vec3f(0.0));
  let lum = max(dot(color, vec3f(0.2126, 0.7152, 0.0722)), 1e-5);
  let mapped = (lum * (1.0 + lum / (WHITE * WHITE))) / (1.0 + lum);
  let hueKept = color * (mapped / lum);
  let perCh = (color * (1.0 + color / (WHITE * WHITE))) / (1.0 + color);
  // Cubed so the crossover is late: below ~2x exposure the blend is a few
  // percent and hue is essentially untouched, and only genuinely hot cores
  // (3x and up) bleach toward white. A linear weight starts desaturating in
  // the midtones, which is the gold-honeycomb failure this replaces.
  let bleach = clamp(mapped * mapped * mapped * 0.9, 0.0, 0.9);
  color = mix(hueKept, perCh, bleach);
  color = pow(color, vec3f(1.0 / 2.2));
  var out : FSOut;
  out.color = vec4f(color, 1.0);
  out.depth = depth;
  return out;
}
