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
  mediaLen : f32,     // liquid/gas path length before the hit
  mediaMat : u32,
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

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = 1e-6; }
  if (abs(rd.y) < 1e-6) { rd.y = 1e-6; }
  if (abs(rd.z) < 1e-6) { rd.z = 1e-6; }
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

  var mediaStart = -1.0;
  var tCur = t;

  for (var i = 0; i < 4096; i++) {
    if (i >= maxSteps) { break; }
    if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
        cell.x >= i32(WORLD_N) || cell.y >= i32(WORLD_N) || cell.z >= i32(WORLD_N)) {
      break;
    }

    if (chunkOcc(cell) == 0u) {
      // empty chunk: jump straight to its exit face
      if (mediaStart >= 0.0) { out.mediaLen += tCur - mediaStart; mediaStart = -1.0; }
      let ch = cell / i32(CHUNK);
      let lo = vec3f(ch * i32(CHUNK));
      let hi = lo + f32(CHUNK);
      let e0 = (lo - ro) * inv;
      let e1 = (hi - ro) * inv;
      let ex = max(e0, e1);
      let tOut = min(ex.x, min(ex.y, ex.z));
      t = tOut + 1e-4;
      if (t >= tExit) { break; }
      p = ro + rd * t;
      cell = clamp(vec3<i32>(floor(p)), vec3<i32>(0), vec3<i32>(i32(WORLD_N) - 1));
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - ro[a]) * inv[a];
      }
      tCur = t;
      continue;
    }

    let w = voxels[cellIndex(vec3<u32>(cell))];
    let mat = voxMat(w);
    if (mat != MAT_AIR) {
      let k = materials[mat].klass;
      if (k == CLASS_LIQUID || k == CLASS_GAS) {
        if (wantMedia) {
          if (mediaStart < 0.0) { mediaStart = tCur; }
          if (out.mediaMat == 0u) { out.mediaMat = mat; }
        }
        // fall through and keep marching
      } else {
        if (mediaStart >= 0.0) { out.mediaLen += tCur - mediaStart; }
        out.hit = true;
        out.t = tCur;
        out.cell = cell;
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.word = w;
        return out;
      }
    } else if (mediaStart >= 0.0) {
      out.mediaLen += tCur - mediaStart;
      mediaStart = -1.0;
    }

    // DDA step
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    if (tCur >= tExit) { break; }
  }
  if (mediaStart >= 0.0) { out.mediaLen += tCur - mediaStart; }
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  let ndc = in.uv;
  let rd = normalize(R.camFwd
                   + R.camRight * (ndc.x * R.tanHalfFov * R.aspect)
                   + R.camUp    * (ndc.y * R.tanHalfFov));

  let h = trace(R.camPos, rd, 4096, true);

  var color : vec3f;
  if (!h.hit) {
    color = skyColor(rd);
  } else {
    let mat = voxMat(h.word);
    let m = materials[mat];
    let albedo = paletteColor(m, voxState(h.word));

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

    // distance fog
    let fog = 1.0 - exp(-h.t * 0.0016);
    color = mix(color, skyColor(rd) * 0.9, fog);
  }

  // liquid / gas tint along the ray
  if (h.mediaLen > 0.0 && h.mediaMat != 0u) {
    let mm = materials[h.mediaMat];
    let mc = (unpackColor(mm.color0) + unpackColor(mm.color1)) * 0.5;
    let dens = select(0.10, 0.035, mm.klass == CLASS_GAS);
    let a = 1.0 - exp(-h.mediaLen * dens);
    color = mix(color, mc, a);
  }

  // gamma-ish
  color = pow(max(color, vec3f(0.0)), vec3f(1.0 / 2.2));
  return vec4f(color, 1.0);
}
