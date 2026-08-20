// debris.wgsl — instanced-cube raster paths composited against the raymarched
// terrain via the shared reversed-Z depth buffer (KNEAR / projectView in
// common.wgsl). Two entry points:
//   vsParticle — one cube per live GPU particle (reads the particle buffer)
//   vsSprite   — CPU-written instances (grenades, gameplay markers)
// Lighting is computed flat per-face in the vertex shader (sun lambert +
// ambient + emissive), matching the terrain's look closely enough that flying
// voxels read as the same material.

@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;

@group(1) @binding(0) var<storage, read> particles : array<Particle>;

struct Sprite {
  pos : vec3f, halfSize : f32,
  color : u32, emission : f32, _a : u32, _b : u32,
};
@group(1) @binding(1) var<storage, read> sprites : array<Sprite>;

// Debris rigidbodies (DESIGN.md §7): one cube per body voxel, transformed by
// the body's Jolt pose. Bodies keep their voxel payload, so debris stays
// voxel-crisp instead of marching-cubes-smooth.
struct BodyVoxInst {
  lx : f32, ly : f32, lz : f32,  // body-local voxel min corner
  packed : u32,                  // bits 0..15 payload, bits 16..27 body slot
};
struct BodyXform {
  pos : vec3f, _p : f32,         // voxel units
  quat : vec4f,                  // x,y,z,w
};
@group(1) @binding(2) var<storage, read> bodyInst : array<BodyVoxInst>;
@group(1) @binding(3) var<storage, read> bodyXf : array<BodyXform>;

fn quatRotate(q : vec4f, v : vec3f) -> vec3f {
  let t = 2.0 * cross(q.xyz, v);
  return v + q.w * t + cross(q.xyz, t);
}

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
};

fn axisUnit(a : u32) -> vec3f {
  if (a == 0u) { return vec3f(1.0, 0.0, 0.0); }
  if (a == 1u) { return vec3f(0.0, 1.0, 0.0); }
  return vec3f(0.0, 0.0, 1.0);
}

// vi in 0..35: face = vi/6 (+x,-x,+y,-y,+z,-z), two triangles per face.
fn cubeOffset(vi : u32, out_n : ptr<function, vec3f>) -> vec3f {
  let face = vi / 6u;
  let axis = face / 2u;
  let sgn = 1.0 - 2.0 * f32(face % 2u);
  let n = axisUnit(axis) * sgn;
  let t1 = axisUnit((axis + 1u) % 3u);
  let t2 = axisUnit((axis + 2u) % 3u);
  var quad = array<vec2f, 6>(
      vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),
      vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0));
  let q = quad[vi % 6u];
  *out_n = n;
  return n * 0.5 + t1 * (q.x * 0.5) + t2 * (q.y * 0.5);
}

fn unpackColor(c : u32) -> vec3f {
  return vec3f(f32(c & 0xFFu), f32((c >> 8u) & 0xFFu), f32((c >> 16u) & 0xFFu)) / 255.0;
}

fn litColor(albedo : vec3f, n : vec3f, worldPos : vec3f, emission : f32) -> vec3f {
  let lambert = max(dot(n, R.sunDir), 0.0);
  var c = albedo * (0.38 + 0.72 * lambert * vec3f(1.0, 0.96, 0.88));
  c += albedo * emission * 1.7;
  let dist = length(worldPos - R.camPos);
  let fog = 1.0 - exp(-dist * VOXEL_METERS * 0.0128);
  // cheap sky tint for fog (matches raymarch closely enough at these distances)
  return mix(c, vec3f(0.55, 0.65, 0.85), fog);
}

fn clipped() -> VSOut {
  var out : VSOut;
  out.pos = vec4f(0.0, 0.0, 2.0, 1.0);  // z > w: culled by the clipper
  out.color = vec3f(0.0);
  return out;
}

@vertex
fn vsParticle(@builtin(vertex_index) vi : u32,
              @builtin(instance_index) inst : u32) -> VSOut {
  let p = particles[inst];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return clipped(); }

  var n : vec3f;
  let off = cubeOffset(vi, &n) * 0.7;  // slightly smaller than a voxel
  let center = vec3f(f32(p.px), f32(p.py), f32(p.pz)) / 256.0;
  let world = center + off;

  let mat = p.payload & 0xFFFu;
  let m = materials[mat];
  var albedo = unpackColor(m.color0);
  switch ((p.payload >> 12u) % 3u) {
    case 1u: { albedo = unpackColor(m.color1); }
    case 2u: { albedo = unpackColor(m.color2); }
    default: {}
  }

  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.color = litColor(albedo, n, world, f32(m.emission) / 255.0);
  return out;
}

@vertex
fn vsBody(@builtin(vertex_index) vi : u32,
          @builtin(instance_index) inst : u32) -> VSOut {
  let b = bodyInst[inst];
  let xf = bodyXf[b.packed >> 16u];

  var n : vec3f;
  let off = cubeOffset(vi, &n);
  let local = vec3f(b.lx, b.ly, b.lz) + vec3f(0.5) + off;
  let world = xf.pos + quatRotate(xf.quat, local);
  let wn = quatRotate(xf.quat, n);

  let mat = b.packed & 0xFFFu;
  let m = materials[mat];
  var albedo = unpackColor(m.color0);
  switch (((b.packed >> 12u) & 0xFu) % 3u) {
    case 1u: { albedo = unpackColor(m.color1); }
    case 2u: { albedo = unpackColor(m.color2); }
    default: {}
  }

  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  // emissive body voxels (embers on burning debris) flicker like their grid
  // counterparts in raymarch.wgsl — same rate, per-voxel phase
  var emis = f32(m.emission) / 255.0;
  if (emis > 0.0) {
    let fh = pcg(inst * 2917u + (b.packed >> 16u) * 131u);
    emis *= 0.82 + 0.28 * sin(R.time * 9.0 + f32(fh & 0xFFu) * 0.0245);
  }
  out.color = litColor(albedo, wn, world, emis);
  return out;
}

@vertex
fn vsSprite(@builtin(vertex_index) vi : u32,
            @builtin(instance_index) inst : u32) -> VSOut {
  let s = sprites[inst];
  var n : vec3f;
  let off = cubeOffset(vi, &n) * (s.halfSize * 2.0);
  let world = s.pos + off;
  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.color = litColor(unpackColor(s.color), n, world, s.emission);
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  return vec4f(pow(max(in.color, vec3f(0.0)), vec3f(1.0 / 2.2)), 1.0);
}
