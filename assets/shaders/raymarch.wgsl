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
    let seg = (tCur - tPrev) * weight;
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
    }
    // Media early-out: fs() will mix the background in at exp(-tau); once
    // that is ~0 the rest of the march (often hundreds of per-voxel steps
    // through a smoke plume) cannot change the pixel.
    if (out.mediaTau * VOXEL_METERS * MEDIA_ABSORB > MEDIA_TAU_MAX) {
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
struct FarHit {
  hit  : bool,
  t    : f32,          // fine-voxel units
  axis : i32,
  sgn  : f32,
  mat  : u32,
  cell : vec3<i32>,    // level cells (palette jitter)
};

fn farMatAt(level : u32, c : vec3<i32>) -> u32 {
  let bi = farVoxByteIndex(level, c);
  return (farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu;
}

fn traceFar(ro : vec3f, rdIn : vec3f, tStart : f32) -> FarHit {
  var out : FarHit;
  out.hit = false;

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  var tPrev = max(tStart, 0.0);   // fine-voxel units

  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let s = f32(1u << level);     // fine voxels per level cell
    let org = F.origins[level - 1u].xyz;
    // everything below is in LEVEL-CELL coords: pos/s, t/s (same rd)
    let roL = ro / s;
    let lo = vec3f(org * i32(CHUNK));
    let tt0 = (lo - roL) * inv;
    let tt1 = (lo + f32(WORLD_N) - roL) * inv;
    let tmin = min(tt0, tt1);
    let tmax = max(tt0, tt1);
    let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, tPrev / s));
    let tExit = min(tmax.x, min(tmax.y, tmax.z));
    if (tExit <= tEnter) { continue; }   // box missed (or fully behind tPrev)

    var t = tEnter + 1e-4;
    var p = roL + rd * t;
    let loI = org * i32(CHUNK);
    var cell = clamp(vec3<i32>(floor(p)), loI, loI + vec3<i32>(i32(WORLD_N) - 1));
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
      if (!inWindow(cell, org)) { break; }
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
        if (!inWindow(nc, org)) { break; }
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
    tPrev = max(tPrev, tExit * s);
  }
  return out;
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
    far = traceFar(R.camPos, rd, h.tExit);
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
      // far-field shading: palette + face term + unshadowed N·L + fog. No
      // shadow rays out here — at these distances fog dominates the term.
      let m = materials[far.mat];
      let jit = pcg(u32(far.cell.x * 7 + far.cell.y * 131 + far.cell.z * 2917));
      var albedo = paletteColor(m, jit);
      if (m.klass == CLASS_LIQUID) { albedo = unpackColor(m.color0); }
      var n = vec3f(0.0);
      n[far.axis] = -far.sgn;
      var face = 0.85;
      if (far.axis == 1) { face = select(0.55, 1.0, n.y > 0.0); }
      else if (far.axis == 2) { face = 0.75; }
      let lambert = max(dot(n, R.sunDir), 0.0);
      color = albedo * face * (0.38 + 0.72 * lambert * vec3f(1.0, 0.96, 0.88));
      let emis = f32(m.emission) / 255.0;
      if (emis > 0.0) { color += albedo * emis * 1.7; }
      let fog = 1.0 - exp(-far.t * VOXEL_METERS * R.fogDensity);
      color = mix(color, skyColor(rd) * 0.9, fog);
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

    // cheap per-face variation reads as form even in shadow
    var face = 0.85;
    if (h.axis == 1) { face = select(0.55, 1.0, n.y > 0.0); }
    else if (h.axis == 2) { face = 0.75; }

    var lambert = max(dot(n, R.sunDir), 0.0);
    if (lambert > 0.0 && (R.flags & 1u) != 0u) {
      let hp = R.camPos + rd * (h.t - 1e-3);
      let s = trace(hp + n * 0.02, R.sunDir, 384, false);
      if (s.hit) { lambert = 0.0; }
    }
    color = albedo * face * (0.38 + 0.72 * lambert * vec3f(1.0, 0.96, 0.88));

    // emissive materials glow through shadow, with a slow per-cell flicker
    // (render-only floats: the sim never sees any of this)
    let emis = f32(m.emission) / 255.0;
    if (emis > 0.0) {
      let ch = pcg(u32(h.cell.x * 7 + h.cell.y * 131 + h.cell.z * 2917));
      let flick = 0.82 + 0.28 * sin(R.time * 9.0 + f32(ch & 0xFFu) * 0.0245);
      color += albedo * emis * 1.7 * flick;
    }

    // distance fog (density per meter, so the look survives voxel-size
    // changes). Density is a uniform pinned to the far-field extent so the
    // cascade horizon fades out instead of ending in a cut.
    let fog = 1.0 - exp(-h.t * VOXEL_METERS * R.fogDensity);
    color = mix(color, skyColor(rd) * 0.9, fog);
  }

  // liquid / gas tint along the ray. mediaTau/mediaTint accumulate per cell
  // in trace(), so a ray crossing fire INTO smoke shades each stretch with
  // its own material instead of painting the whole path with the first one.
  if (h.mediaMat != 0u) {
    let mm = materials[h.mediaMat];
    var mc = (unpackColor(mm.color0) + unpackColor(mm.color1)) * 0.5;
    if (h.mediaTau > 1e-5) { mc = h.mediaTint / h.mediaTau; }
    var tau = h.mediaTau * VOXEL_METERS * MEDIA_ABSORB;
    // surface term: entering a liquid at all contributes opacity, scaled by
    // the entry cell's fullness — a shallow puddle still reads against the
    // ground instead of vanishing at ~zero path length
    if (mm.klass == CLASS_LIQUID) {
      tau += (f32(mm.opacity) / 255.0) * 1.2 * h.mediaSurf;
    }
    let a = 1.0 - exp(-tau);
    color = mix(color, mc, a);
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

  // gamma-ish
  color = pow(max(color, vec3f(0.0)), vec3f(1.0 / 2.2));
  var out : FSOut;
  out.color = vec4f(color, 1.0);
  out.depth = depth;
  return out;
}
