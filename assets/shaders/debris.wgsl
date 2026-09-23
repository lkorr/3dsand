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
// The glow field (src/sim/world.h kGlowBytes, common.wgsl THE GLOW FIELD).
// ONE buffer load per shaded point, no ray and no voxel read, which is the only
// shape of emitter light this path can consume.
@group(0) @binding(20) var<storage, read> glow : array<u32>;
// The water veil (common.wgsl THE WATER VEIL), written by raymarch.wgsl in the
// half of the world pass before these draws.
@group(0) @binding(24) var<storage, read> waterVeil : array<u32>;

// Eye distance of a fragment, in voxels, from its reversed-Z depth — for the
// per-vertex paths, which carry no world position to the fragment stage.
// depth = KNEAR / viewZ, and a pixel's ray has |rd| / viewZ = the length of
// its unnormalized camera-basis direction (camFwd + ndc offsets).
fn fragEyeDist(fragPos : vec4f) -> f32 {
  let viewZ = KNEAR / max(fragPos.z, 1e-7);
  let h = max(R.viewPx, 1.0);
  let w = max(round(R.viewPx * R.aspect), 1.0);
  let ndc = vec2f(fragPos.x / w * 2.0 - 1.0, 1.0 - fragPos.y / h * 2.0);
  let off = ndc * vec2f(R.tanHalfFov * R.aspect, R.tanHalfFov);
  return viewZ * sqrt(1.0 + dot(off, off));
}

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

// ---- THE BODY PATH'S COST SWITCHES (2026-09-12, drawBodies attribution) ----
// One resting 28.4k-voxel oak 48 voxels from the camera cost the drawBodies
// span 1.0 ms mean / 3.3-3.9 ms p99 per frame, a quarter of the whole-world
// raymarch, and it was a STANDING cost (a body that does not rest grid-aligned
// never settles back). Two suspects, and both were real; the numbers are on
// the commit that added this block.
//
// BODY_SHADOW_RAY: false skips fsBody's sun-shadow march entirely. It is the
// attribution arm, not a quality setting — a body with no shadow term is the
// 3.2x-too-bright bug BodyVSOut exists to fix. Leave it true.
const BODY_SHADOW_RAY : bool = true;
// BODY_VIEW_CULL: bodyVertex collapses a cube face whose normal points away
// from the camera before it is rasterised (the pipeline's cullMode is None
// because cubeOffset's winding flips between the +/- faces of an axis). An
// opaque cube's back faces can never win the depth test against its own front
// faces, so this only removes work: half of every cube's triangles from both
// the depth pre-pass and the shaded pass.
const BODY_VIEW_CULL : bool = true;
// BODY_SUN_SKIP: fsBody casts no ray for a face the key light cannot reach
// (wrapDiffuse is exactly zero there, so the answer would be multiplied by 0).
// Pixel-identical with it on or off; it exists as a switch only so the
// attribution arms can be re-run one at a time.
const BODY_SUN_SKIP : bool = true;

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
                        opennessAtBody(world, &occupancy, &openness, &opennessGen));
  // Emitter light from the glow field. A spark shower thrown out of a forge or
  // a burning leaf tumbling past a lava pit is lit by it; before this the only
  // light on a loose particle was the sky and its own emission. `ao` 1.0 —
  // a particle is a free cube in the air, so there is no crease for the glow to
  // be occluded by, and the openness term above already carries what enclosure
  // it sits in.
  out.color += glowLight(bt.albedo, 1.0, glowAtPos(world, &glow),
                         TUNE_GLOW_STRENGTH);
  return out;
}

// The body cube's vertex, ONCE, for both body entry points. The depth
// pre-pass (vsBodyDepth) and the shaded pass (vsBody) must land every vertex
// on the identical clip position or the second pass fails its own depth test
// in a speckle of holes, and the only way to make two entry points agree to
// the last ulp is to have them run the same instructions. Returns false for a
// face the camera cannot see (BODY_VIEW_CULL).
fn bodyVertex(vi : u32, inst : u32, out_world : ptr<function, vec3f>,
              out_wn : ptr<function, vec3f>, out_n : ptr<function, vec3f>) -> bool {
  let b = bodyInst[inst];
  let xf = bodyXf[b.packed >> 16u];
  var n : vec3f;
  let off = cubeOffset(vi, &n);
  let local = vec3f(b.lx, b.ly, b.lz) + vec3f(0.5) + off;
  let world = xf.pos + quatRotate(xf.quat, local);
  let wn = quatRotate(xf.quat, n);
  *out_world = world;
  *out_wn = wn;
  *out_n = n;
  // The view-facing test is a PLANE test: dot(wn, p) is the same for every
  // point p of the face, so all six vertices of a back face take this branch
  // together and the collapsed triangles are degenerate rather than torn.
  return !(BODY_VIEW_CULL && dot(wn, world - R.camPos) > 0.0);
}

// ---- THE DEPTH PRE-PASS (2026-09-12) ----------------------------------------
// Simulation::DrawBodies draws every body cube TWICE: first through this pair,
// which writes depth and (by a Zero/One blend) no colour, then through
// vsBody/fsBody, whose fragments now pass the depth test only where they are
// the nearest thing — so each pixel casts fsBody's shadow ray ONCE. Without it
// the instance order decided who got shaded: a cube drawn before the cube in
// front of it paid the full fragment stage and was then overwritten, and on a
// 28k-voxel crown with a perforated canopy that was most of the drawBodies
// span (culling the 15% of instances that were fully enclosed removed 48% of
// the time, which is the signature of overdraw, not of vertex work).
struct BodyDepthOut {
  @builtin(position) pos : vec4f,
};

@vertex
fn vsBodyDepth(@builtin(vertex_index) vi : u32,
               @builtin(instance_index) inst : u32) -> BodyDepthOut {
  var world : vec3f;
  var wn : vec3f;
  var n : vec3f;
  var out : BodyDepthOut;
  if (!bodyVertex(vi, inst, &world, &wn, &n)) {
    out.pos = vec4f(0.0, 0.0, -2.0, 1.0);
    return out;
  }
  out.pos = projectView(world - R.camPos, R);
  return out;
}

@fragment
fn fsBodyDepth() -> @location(0) vec4f {
  // The pipeline's blend is (Zero, One): this value never reaches the target.
  return vec4f(0.0);
}

@vertex
fn vsBody(@builtin(vertex_index) vi : u32,
          @builtin(instance_index) inst : u32) -> BodyVSOut {
  let b = bodyInst[inst];
  let xf = bodyXf[b.packed >> 16u];

  var world : vec3f;
  var wn : vec3f;
  var n : vec3f;
  let visible = bodyVertex(vi, inst, &world, &wn, &n);

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
  // z = -2 is outside reversed-Z's [0, 1] and is clipped, the same drop vsFluid
  // uses for a dead particle.
  if (!visible) {
    out.pos = vec4f(0.0, 0.0, -2.0, 1.0);
    return out;
  }
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
  // The shadow ray only ever MULTIPLIES the key-light lambert (litColorS), and
  // wrapDiffuse is exactly zero once the face turns past -TUNE_DIFFUSE_WRAP
  // from the light. A face the sun cannot reach gets no ray: same pixel, no
  // march. On a cube that is at least three of six faces, and after the view
  // cull it is roughly half of what is left.
  var sh = 1.0;
  if (BODY_SHADOW_RAY &&
      (!BODY_SUN_SKIP || dot(in.wn, keyLightDirP(R)) > -TUNE_DIFFUSE_WRAP)) {
    sh = bodySunShadow(in.world, in.wn, R, &occupancy, &materials);
  }
  let lit = litColorSNoFog(in.albedo, in.wn, in.world, in.misc.x, R,
                           in.misc.y, in.misc.z, sh);
  // Emitter light from the glow field (common.wgsl THE GLOW FIELD), and this
  // is the call site the whole feature exists for: THE CROWN THAT FALLS OFF A
  // BURNING TREE COMES THROUGH HERE. A rigid body is not in the voxel grid, so
  // neither irradiance injector can see it and `giGather` cannot be called for
  // it — before this a body beside a lava pool was lit by the sky alone
  // (docs/PLAN_rigidbody_lighting.md). PER FRAGMENT rather than in vsBody: the
  // field is a 40 cm grid and a body cube is 10 cm, so a per-vertex sample
  // would quantise the light across the cube's own corners.
  //
  // `in.misc.y` is the ambient openness multiplier the vertex stage measured —
  // the same occlusion the ambient takes, for the same reason.
  let add = glowLight(in.albedo, in.misc.y, glowAtPos(in.world, &glow),
                      TUNE_GLOW_STRENGTH);
  // Under a liquid surface the cube is seen through the water column exactly
  // as a micro body is (microbody.wgsl UNDER WATER); above it, air fog.
  let dist = length(in.world - R.camPos);
  let veil = waterVeilAt(&waterVeil, in.pos.xy, dist, &R);
  var col : vec3f;
  if (veil.on) {
    col = waterVeilApply(veil, lit + add, dist);
  } else {
    col = bodyAirFog(lit, in.world, R) + add;
  }
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
                        opennessAtBody(world, &occupancy, &openness, &opennessGen));
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
                        opennessAtBody(world, &occupancy, &openness, &opennessGen));
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  // A particle under a liquid surface (sand sinking into a pond, a droplet
  // below the waterline) is seen through the column like a body is. Its colour
  // was lit and air-fogged per vertex over the whole eye distance, so the fog
  // to the surface is counted twice here — at particle size, not worth a
  // second interstage colour to undo.
  var c = in.color;
  let dist = fragEyeDist(in.pos);
  let veil = waterVeilAt(&waterVeil, in.pos.xy, dist, &R);
  if (veil.on) { c = waterVeilApply(veil, c, dist); }
  // litColor is linear HDR; compress through the terrain's tonemap so a cube
  // matches the ground it lands on at any time of day (bare gamma here made
  // debris glow at night).
  return vec4f(tonemapHdr(c), 1.0);
}
