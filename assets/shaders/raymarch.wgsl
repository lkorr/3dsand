// raymarch.wgsl — fullscreen two-level DDA over the voxel grid.
// Chunk-level skip via per-chunk occupancy counts, voxel DDA inside non-empty
// chunks. The renderer reads the same device-local buffers the sim writes —
// zero upload cost (DESIGN.md §9). Rendering is allowed to use floats; only
// sim state must stay integer.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;

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

struct Hit {
  hit      : bool,
  t        : f32,
  cell     : vec3<i32>,
  axis     : i32,     // axis stepped into the hit cell
  sgn      : f32,     // ray direction sign on that axis
  word     : u32,
  mediaLen : f32,     // fullness-weighted liquid/gas path length before the hit
  mediaMat : u32,     // first media material crossed
  mediaSurf: f32,     // fullness (0..1) of the first media cell — surface term
};

fn chunkOcc(cell : vec3<i32>) -> u32 {
  let ch = vec3<u32>(cell) / CHUNK;
  return occupancy[(ch.z * NCHUNK + ch.y) * NCHUNK + ch.x];
}

fn trace(ro : vec3f, rdIn : vec3f, maxSteps : i32, wantMedia : bool) -> Hit {
  var out : Hit;
  out.hit = false;
  out.mediaLen = 0.0;
  out.mediaMat = 0u;
  out.mediaSurf = 0.0;

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // clip to world AABB
  let nf = f32(WORLD_N);
  let tt0 = (vec3f(0.0) - ro) * inv;
  let tt1 = (vec3f(nf) - ro) * inv;
  let tmin = min(tt0, tt1);
  let tmax = max(tt0, tt1);
  let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
  let tExit = min(tmax.x, min(tmax.y, tmax.z));
  if (tExit <= tEnter) { return out; }

  var t = tEnter + 1e-4;
  var p = ro + rd * t;
  var cell = clamp(vec3<i32>(floor(p)), vec3<i32>(0), vec3<i32>(i32(WORLD_N) - 1));
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
    if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
        cell.x >= i32(WORLD_N) || cell.y >= i32(WORLD_N) || cell.z >= i32(WORLD_N)) {
      break;
    }

    if (chunkOcc(cell) == 0u) {
      // empty chunk: jump straight to its exit face (air — no media to add)
      let ch = cell / i32(CHUNK);
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

    let w = voxels[cellIndex(vec3<u32>(cell))];
    let mat = voxMat(w);
    var weight = 0.0;  // this cell's media contribution per unit length
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
          if (out.mediaMat == 0u) {
            out.mediaMat = mat;
            out.mediaSurf = weight;
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
    out.mediaLen += (tCur - tPrev) * weight;
    if (tCur >= tExit) { break; }
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

  // reversed-Z depth so raster geometry (particles/debris) composites in
  var depth = 0.0;  // sky = far
  if (h.hit) {
    let viewZ = h.t * dot(rd, R.camFwd);
    depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  }

  var color : vec3f;
  if (!h.hit) {
    color = skyColor(rd);
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

    // distance fog (density per meter, so the look survives voxel-size changes)
    let fog = 1.0 - exp(-h.t * VOXEL_METERS * 0.0128);
    color = mix(color, skyColor(rd) * 0.9, fog);
  }

  // liquid / gas tint along the ray
  if (h.mediaMat != 0u) {
    let mm = materials[h.mediaMat];
    let mc = (unpackColor(mm.color0) + unpackColor(mm.color1)) * 0.5;
    let memis = f32(mm.emission) / 255.0;
    let kOp = f32(mm.opacity) / 255.0;
    // per-meter absorption from the material's opacity (oil thick, water
    // clearer, steam wispy); mediaLen is fullness-weighted in trace()
    var tau = h.mediaLen * VOXEL_METERS * kOp * 6.4;
    // surface term: entering a liquid at all contributes opacity, scaled by
    // the entry cell's fullness — a shallow puddle still reads against the
    // ground instead of vanishing at ~zero path length
    if (mm.klass == CLASS_LIQUID) { tau += kOp * 1.2 * h.mediaSurf; }
    let a = 1.0 - exp(-tau);
    color = mix(color, mc, a);
    // emissive media (fire) glows additively — the flame is a light source
    if (memis > 0.0) {
      let mflick = 0.85 + 0.25 * sin(R.time * 11.0);
      color += mc * memis * a * 1.6 * mflick;
    }
  }

  // gamma-ish
  color = pow(max(color, vec3f(0.0)), vec3f(1.0 / 2.2));
  var out : FSOut;
  out.color = vec4f(color, 1.0);
  out.depth = depth;
  return out;
}
