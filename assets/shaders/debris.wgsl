// debris.wgsl — instanced-cube raster paths composited against the raymarched
// terrain via the shared reversed-Z depth buffer (KNEAR / projectView in
// common.wgsl). Two entry points:
//   vsParticle — one cube per live GPU particle (reads the particle buffer)
//   vsSprite   — CPU-written instances (grenades, gameplay markers)
// Lighting is computed flat per-face in the vertex shader (sun lambert +
// ambient + emissive), matching the terrain's look closely enough that flying
// voxels read as the same material.

// The blockers mask and the openness grid (docs/PLAN_gi.md §2), so a cube is
// lit like the ground under it — opennessScaleAtBody in common.wgsl, the same
// read microbody.wgsl makes per fragment, made here per VERTEX from the cube's
// centre (eight reads per cube; the raster stage is nowhere near the frame's
// cost). Without it a burning ember or a thrown grenade glowed at full sky
// ambient in the middle of a cave.
// The world grid and its page table, read ONLY by fsBody's sun-shadow ray
// (bodySunShadow, common.wgsl). Both are Fragment-only in renderBGL_, which is
// precisely why the body path had to stop shading in its vertex shader — see
// the note on vsBody. Declaring `voxels` is also what keeps common.wgsl's
// page-table block (traceOpaque, voxWordAt) in this shader at all:
// BodyAddressesVoxels in src/gpu/resources.cpp reads the declaration rather
// than consulting a list, so there is nothing else to update.
@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(9) var<storage, read> pageTable : array<u32>;
@group(0) @binding(17) var<storage, read> openness    : array<u32>;
@group(0) @binding(18) var<storage, read> opennessGen : array<u32>;

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

// The BODY path's interstage block, and the reason it is a second struct rather
// than a widened VSOut (2026-09-04).
//
// A rigidbody had NO SHADOW TERM: it received 100% of the key light wherever it
// stood, which measured 3.2x too bright in an ordinary cast shadow and 10.4x
// indoors — "fine in full sun, washed out everywhere else". Fixing that needs a
// ray against `voxels`, and `voxels`/`pageTable` are FRAGMENT-ONLY in
// renderBGL_ (src/sim/simulation.cpp), so a vertex shader cannot reach the
// world at all. Widening their visibility to Vertex would be the wrong trade:
// it would hand the world grid to every particle and sprite vertex too, for one
// consumer.
//
// So the body path alone ships its surface across the interstage boundary and
// lights it in fsBody. vsParticle / vsFluid / vsSprite deliberately STAY on
// per-vertex litColorO with no shadow: they draw sub-voxel cubes (foam is 1/6
// of a cell, and there can be ~260k of them), a per-fragment shadow march on
// that population would cost more than the whole raster stage, and a droplet is
// too small for a missing shadow to read. That inconsistency is a decision, not
// an oversight.
//
// FLAT on everything the cube is constant over — the face normal, the material
// albedo, the emissive level and the openness probe are all per-instance or
// per-face, so interpolating them would only add error. `world` is the one
// genuinely per-pixel value: it is the shadow ray's origin, and interpolating
// it is what makes the shadow edge land inside a face instead of snapping to
// its corners.
struct BodyVSOut {
  @builtin(position) pos : vec4f,
  @location(0) @interpolate(flat) albedo : vec3f,
  @location(1) @interpolate(flat) wn : vec3f,
  @location(2) world : vec3f,
  // (emissive, openness ambient scale, raw openness) — packed into one slot
  // because all three are flat and a vec3f costs the same as a lone f32.
  @location(3) @interpolate(flat) misc : vec3f,
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
  var albedo = paletteColor(m, p.payload >> 12u, &materials);

  // FOAM particles (bit 31 of payload, set by sim_fluid.wgsl's g2p) are not
  // made of any material — they are air in water. Colouring them by a material
  // id would mean authoring a "foam" material whose only job is to be white,
  // and would tie the look to the material table instead of the tuner. They
  // take their colour straight from the render tuning, jittered per particle so
  // a burst reads as many bubbles rather than one flat white mass.
  if ((p.payload & PPAY_FOAM) != 0u) {
    // DISTANCE LOD: foam particles are micro (1/6 voxel at scale index 3) and
    // cover less than a pixel past ~32 cells. Rasterizing sub-pixel cubes that
    // are invisible against the raymarched surface wastes fill rate — over open
    // water at surface level the particle count dominates the frame. Cull them
    // past a fixed horizon and probabilistically thin them in the transition
    // band so the pop-off is invisible. The raymarched foam field (grid word 7)
    // still covers the far surface — this only gates the debris particles.
    let d2 = dot(center - R.camPos, center - R.camPos);
    let fadeBegin = 28.0 * 28.0;   // voxels^2: full density inside
    let fadeEnd   = 48.0 * 48.0;   // voxels^2: fully culled outside
    if (d2 > fadeEnd) { return clipped(); }
    if (d2 > fadeBegin) {
      // Probabilistic thin: hash the instance to a uniform [0,1) and keep the
      // particle with probability that ramps linearly from 1 at fadeBegin to 0
      // at fadeEnd. Stable per instance (no flicker).
      let rnd = f32(pcg(inst * 2917u + 0x1234u) & 0xFFFFu) * (1.0 / 65536.0);
      let keep = 1.0 - (d2 - fadeBegin) / (fadeEnd - fadeBegin);
      if (rnd > keep) { return clipped(); }
    }
    let j = f32((p.payload >> 12u) & 7u) * (1.0 / 7.0);
    albedo = TUNE_FOAM_COLOR * (1.0 - TUNE_FOAM_COLOR_VAR * j);
  }

  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  // A burning leaf thrown loose is still a burning leaf: the same breath
  // between the leaf palette and the flame colour the grid gives it (burnTint,
  // common.wgsl), keyed on the instance so it does not slide as the particle
  // flies. A no-op for every material without MATF_BURNTINT.
  let bt = burnTint(m, albedo, f32(m.emission) / 255.0,
                    burnTintWeightH(pcg(inst * 2917u), R.time));
  out.color = litColorO(bt.albedo, n, world, bt.emis, R,
                        opennessScaleAtBody(world, &occupancy, &openness, &opennessGen));
  return out;
}

@vertex
fn vsBody(@builtin(vertex_index) vi : u32,
          @builtin(instance_index) inst : u32) -> BodyVSOut {
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
    albedo = paletteColor(m, (b.packed >> 12u) & 0xFu, &materials);
  }

  var out : BodyVSOut;
  out.pos = projectView(world - R.camPos, R);
  // emissive body voxels (embers on burning debris) flicker like their grid
  // counterparts in raymarch.wgsl — same rate, per-voxel phase
  let fh = pcg(inst * 2917u + (b.packed >> 16u) * 131u);
  // ...and a burn-tinted one breathes toward the flame colour first, on the
  // same phase key. THE CROWN THAT FALLS OFF A BURNING TREE COMES THROUGH
  // HERE, not through the grid path, so leaving this out showed exactly the
  // green glow burnTint exists to prevent.
  let bt = burnTint(m, albedo, f32(m.emission) / 255.0,
                    burnTintWeightH(fh, R.time));
  out.albedo = bt.albedo;
  out.wn = wn;
  out.world = world;
  // The openness probe stays PER VERTEX — and now per CUBE CENTRE rather than
  // per corner. It is a 40 cm-block ambient multiplier, so it has nothing to
  // say at corner resolution, and taking it at the centre stops a corner that
  // pokes into the neighbouring block from swinging a whole face's ambient.
  // Six mask reads once per cube instead of six per vertex: cheaper than what
  // it replaced, while the term that genuinely needs per-pixel resolution (the
  // shadow) moved to the fragment stage.
  let center = xf.pos + quatRotate(xf.quat, vec3f(b.lx, b.ly, b.lz) + vec3f(0.5));
  let open = opennessAtBody(center, &occupancy, &openness, &opennessGen);
  out.misc = vec3f(emberFlicker(bt.emis, fh, R.time), open.x, open.y);
  return out;
}

// The body path's fragment stage. Everything here is what a vertex shader
// could not do: cast a shadow ray against `voxels` (Fragment-only in
// renderBGL_) from a per-pixel world position.
@fragment
fn fsBody(in : BodyVSOut) -> @location(0) vec4f {
  let sh = bodySunShadow(in.world, in.wn, R, &occupancy, &materials);
  let col = litColorS(in.albedo, in.wn, in.world, in.misc.x, R,
                      in.misc.y, in.misc.z, sh);
  // Same tonemap as fs() and as the terrain: a cube must match the ground it
  // lands on at any time of day.
  return vec4f(tonemapHdr(col), 1.0);
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
  // The CPU draw count is a CONSERVATIVE estimate (the seam owns the real
  // population and the snapshot is a tick or three behind), so slots past the
  // live range hold compaction leftovers. Their attr is dead (or the slot is
  // stale garbage from a prior tick) — collapse the cube to nothing rather
  // than draw a ghost.
  if (!fpAlive(p.attr)) {
    var dead : VSOut;
    dead.pos = vec4f(0.0, 0.0, -2.0, 1.0);  // clipped: behind the near plane
    dead.color = vec3f(0.0);
    return dead;
  }
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
  out.color = litColorO(albedo, n, world, 0.0, R,
                        opennessScaleAtBody(world, &occupancy, &openness, &opennessGen));
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
  out.color = litColorO(unpackColor(s.color), n, world, s.emission, R,
                        opennessScaleAtBody(world, &occupancy, &openness, &opennessGen));
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  // litColor is linear HDR; compress through the terrain's tonemap so a cube
  // matches the ground it lands on at any time of day (bare gamma here made
  // debris glow at night).
  return vec4f(tonemapHdr(in.color), 1.0);
}
