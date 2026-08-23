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
  // bits 0..11 material, 12..15 state, 16..27 body slot, 28..31 art colour
  // (0 = unpainted). See the bit-budget note on BodyVoxInst in phys/debris.h.
  packed : u32,
};
struct BodyXform {
  pos : vec3f, _p : f32,         // voxel units
  quat : vec4f,                  // x,y,z,w
};
@group(1) @binding(2) var<storage, read> bodyInst : array<BodyVoxInst>;
@group(1) @binding(3) var<storage, read> bodyXf : array<BodyXform>;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
};

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

// quatRotate / axisUnit / unpackColor / litColor / emberFlicker are shared with
// microbody.wgsl and live in common.wgsl — the two paths draw the same limbs.

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

  // A micro particle is drawn at 1/scale of a voxel — that size difference IS
  // the feature (a fine spray reading as droplets rather than as flying
  // bricks). 0.7 of a cell for ordinary particles is the established look;
  // micro keeps the same proportion of its own smaller cell.
  var size = 0.7;
  if (isMicro(p)) { size = 0.7 / f32(microScaleOf(p.flags)); }

  var n : vec3f;
  let off = cubeOffset(vi, &n) * size;
  let center = vec3f(f32(p.px), f32(p.py), f32(p.pz)) / 256.0;
  let world = center + off;

  let mat = p.payload & 0xFFFu;
  let m = materials[mat];
  var albedo = paletteColor(m, p.payload >> 12u);

  // FOAM particles (bit 31 of payload, set by sim_fluid.wgsl's g2p) are not
  // made of any material — they are air in water. Colouring them by a material
  // id would mean authoring a "foam" material whose only job is to be white,
  // and would tie the look to the material table instead of the tuner. They
  // take their colour straight from the render tuning, jittered per particle so
  // a burst reads as many bubbles rather than one flat white mass.
  if ((p.payload & PPAY_FOAM) != 0u) {
    let j = f32((p.payload >> 12u) & 7u) * (1.0 / 7.0);
    albedo = TUNE_FOAM_COLOR * (1.0 - TUNE_FOAM_COLOR_VAR * j);
  }

  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.color = litColor(albedo, n, world, f32(m.emission) / 255.0, R);
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
  // A painted voxel shows its ART colour (a 1-based index into the reserved
  // art run) rather than the material's cosmetic palette variant. Colour is
  // art; the material is what this voxel becomes if it lands in the grid.
  let art = (b.packed >> 28u) & 0xFu;
  var albedo : vec3f;
  if (art != 0u) {
    albedo = unpackColor(materials[ART_PALETTE_BASE + (art - 1u)].color0);
  } else {
    albedo = paletteColor(m, (b.packed >> 12u) & 0xFu);
  }

  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  // emissive body voxels (embers on burning debris) flicker like their grid
  // counterparts in raymarch.wgsl — same rate, per-voxel phase
  let fh = pcg(inst * 2917u + (b.packed >> 16u) * 131u);
  let emis = emberFlicker(f32(m.emission) / 255.0, fh, R.time);
  out.color = litColor(albedo, wn, world, emis, R);
  return out;
}

// MLS-MPM fluid prototype (docs/PLAN_mpm_fluids.md; sim_fluid.wgsl). One cube
// per fluid particle, positions Q16.16 world cells. Everything that makes a
// bag of dice read as one connected liquid is tunable (MPM Fluid Look):
//   * TUNE_FLUID_PARTICLE_SIZE oversizes the cube past the rest lattice pitch
//     so resting neighbours fuse into a surface;
//   * TUNE_FLUID_STRETCH elongates the cube along its velocity — free motion
//     blur, so a falling stream reads as streaks rather than droplets;
//   * TUNE_FLUID_DENSITY_SHADE darkens where p2g2 measured compression, so
//     pressure visibly travels through a pool;
//   * TUNE_FLUID_FOAM whitens with speed — spray and churn wash toward white;
//   * four species each carry their own albedo (TUNE_FLUID_COLOR..COLOR3).
@group(1) @binding(5) var<storage, read> fluid : array<FluidParticle>;

@vertex
fn vsFluid(@builtin(vertex_index) vi : u32,
           @builtin(instance_index) inst : u32) -> VSOut {
  let p = fluid[inst];
  var n : vec3f;
  var off = cubeOffset(vi, &n) * TUNE_FLUID_PARTICLE_SIZE;
  let center = vec3f(f32(p.px), f32(p.py), f32(p.pz)) / 65536.0;
  let v = vec3f(f32(p.vx), f32(p.vy), f32(p.vz)) / 65536.0;  // cells/tick
  let speed = length(v);
  if (speed > 0.05) {
    // Shear the cube along the motion direction. The normal is left alone —
    // at these sizes the lighting error is invisible and the stretch is not.
    let dir = v / speed;
    off += dir * dot(off, dir) * (TUNE_FLUID_STRETCH * min(speed, 3.0));
  }
  let world = center + off;
  var cols = array<vec3f, 4>(TUNE_FLUID_COLOR, TUNE_FLUID_COLOR1,
                             TUNE_FLUID_COLOR2, TUNE_FLUID_COLOR3);
  var albedo = cols[min(p.species, 3u)];
  // p.density is Q16.16 masses/cell; TUNE_FLUID_REST_DENSITY is the tuner's
  // human-unit particles/voxel (see sim_fluid.wgsl's conversion block).
  let rest = max(TUNE_FLUID_REST_DENSITY, 1.0) * 65536.0;
  let compress = clamp(f32(p.density) / rest - 1.0, 0.0, 1.0);
  albedo *= 1.0 - TUNE_FLUID_DENSITY_SHADE * compress;
  let foam = clamp(speed * TUNE_FLUID_FOAM * 0.5, 0.0, 0.85);
  albedo = mix(albedo, vec3f(0.92, 0.95, 0.98), foam);
  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.color = litColor(albedo, n, world, 0.0, R);
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
  out.color = litColor(unpackColor(s.color), n, world, s.emission, R);
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  // litColor is linear HDR; compress through the terrain's tonemap so a cube
  // matches the ground it lands on at any time of day (bare gamma here made
  // debris glow at night).
  return vec4f(tonemapHdr(in.color), 1.0);
}
