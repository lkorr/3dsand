// microbody.wgsl — dynamic microvoxel bodies (docs/PLAN_voxel_editor.md §C,
// DESIGN.md §9). One draw call, 36 vertices per micro body slot.
//
// WHY OBB RASTER + PER-FRAGMENT MARCH, rather than more cubes.
// The cube path (debris.wgsl `vsBody`) is one 36-vertex instance per VOXEL.
// That is exactly right when a limb is a dozen voxels and exactly wrong at 2x
// or 4x resolution, where the same silhouette costs 8x or 64x the instances
// and the extra triangles are all sub-pixel. Here the whole limb is ONE box:
// the vertex shader positions its 8 corners from the body transform, and the
// fragment shader marches the limb's brick in object space. Cost then scales
// with the SCREEN AREA the limb covers, which is the shape rule 2 asks for —
// a micro critter across the field costs a handful of fragments, not 6000
// instances.
//
// BACKFACES ONLY. Front faces would vanish the moment the camera entered the
// box (near-plane clipping, or the OBB being behind the fragment it should
// have generated). Drawing the FAR side instead means the box is covered for
// every camera position including one inside it, and the march simply starts
// at the ray's entry into the slab rather than at the rasterized surface.
//
// DEPTH is the shared reversed-Z convention from common.wgsl, matching
// raymarch.wgsl's `fs` exactly: viewZ = t * dot(rd, camFwd), then
// KNEAR / max(viewZ, KNEAR). That is what lets hardware GreaterEqual depth
// testing composite these bodies against the raymarched world, the particle
// cubes, the ordinary body cubes and the sprites with no sorting anywhere.
//
// SHADOWS: none, v1. Parity with the cube path, which also casts none. Shadow
// rays must never iterate models (the research note in the PLAN) — a coarse
// occupancy proxy stamped render-side is the stretch goal, not this.
//
// DETERMINISM: render-only. These buffers are bound here and nowhere else.

@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;

struct BodyXform {
  pos : vec3f, _p : f32,         // world voxels
  quat : vec4f,                  // x,y,z,w
};
@group(1) @binding(0) var<storage, read> bodyXf : array<BodyXform>;
@group(1) @binding(1) var<storage, read> models : array<MicroBodyModel>;
@group(1) @binding(2) var<storage, read> pool : array<u32>;
// One entry per micro body drawn this frame: the render SLOT it occupies in
// bodyXf, and the model index. Compacted on the CPU so the draw's instance
// count is exactly the number of micro bodies — zero of them means zero
// instances and the pass is skipped entirely (rule 2).
struct MicroBodyInst {
  slot : u32, model : u32, _a : u32, _b : u32,
};
@group(1) @binding(3) var<storage, read> insts : array<MicroBodyInst>;

// quatRotate / axisUnit / unpackColor / litColor / emberFlicker are shared with
// debris.wgsl and live in common.wgsl — the two paths draw the same limbs (a
// live mob's arm and the severed one beside it), so their shading must be one
// definition, not two that happen to agree today.

// Inverse rotation = rotation by the conjugate. Used to bring the camera ray
// into object space; the direction is deliberately left UNNORMALIZED so `t`
// stays in world-voxel units and feeds the depth formula directly.
fn quatRotateInv(q : vec4f, v : vec3f) -> vec3f {
  return quatRotate(vec4f(-q.xyz, q.w), v);
}

// Everything the fragment march needs that is constant across the instance is
// computed ONCE per vertex and interpolated flat. The alternative — refetching
// insts -> models -> bodyXf per fragment — is three dependent storage loads and
// a quaternion rotation before the slab test can even start, on every one of the
// thousands of pixels a limb covers.
struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) worldDir : vec3f,                    // camPos -> fragment
  @location(1) @interpolate(flat) roM : vec3f,      // ray origin, MICRO units
  @location(2) @interpolate(flat) quat : vec4f,     // body rotation
  @location(3) @interpolate(flat) dims : vec3<i32>, // brick extent, micro voxels
  @location(4) @interpolate(flat) base : u32,       // pool word offset
  @location(5) @interpolate(flat) scale : f32,      // micro voxels per world voxel
  @location(6) @interpolate(flat) slot : u32,       // ember flicker phase key
};

// vi in 0..35 -> a corner of the unit box. Every face must wind the SAME way
// around its own outward normal, or `cullMode: Front` keeps a different subset
// of faces depending on the view direction — and in the octant where the three
// mis-wound faces are exactly the three far ones, NOTHING survives and the body
// disappears completely. That was a real bug: with a fixed (t1, t2) basis the
// triple (t1, t2, n) flips handedness with `sgn`, so the three negative faces
// wound backwards and each micro body was invisible from 1/8 of all view
// directions (measured: 12.7% of the sphere, the +++ octant in object space).
//
// The fix is to swap the two tangents on the negative faces, which re-orients
// the sweep so (t1, t2, n) is right-handed for all six. The selftest's
// single-body probe orbits one identity-rotated body through the 8 octants and
// the 6 axes and asserts every one draws.
fn boxCorner(vi : u32) -> vec3f {
  let face = vi / 6u;
  let axis = face / 2u;
  let sgn = 1.0 - 2.0 * f32(face % 2u);
  let n = axisUnit(axis) * sgn;
  // Swap tangents on negative faces so the cross product t1 x t2 always points
  // along +n, making the winding consistent across the whole cube.
  let a1 = axisUnit((axis + 1u) % 3u);
  let a2 = axisUnit((axis + 2u) % 3u);
  let t1 = select(a2, a1, sgn > 0.0);
  let t2 = select(a1, a2, sgn > 0.0);
  var quad = array<vec2f, 6>(
      vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
      vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let q = quad[vi % 6u];
  // 0..1 box coordinates: n*0.5 picks the face, the tangents sweep it.
  return vec3f(0.5) + n * 0.5 + t1 * (q.x - 0.5) + t2 * (q.y - 0.5);
}

@vertex
fn vs(@builtin(vertex_index) vi : u32,
      @builtin(instance_index) inst : u32) -> VSOut {
  let m = models[insts[inst].model];
  let slot = insts[inst].slot;
  let xf = bodyXf[slot];
  let dims = microBodyDims(m);
  let scale = f32(max(m.scale, 1u));

  // Object-space box, in WORLD voxels: the limb is dims micro voxels across at
  // 1/scale world voxels each. Body-local coords are micro units divided by
  // scale, which is exactly the pitch the collider was built at (physics.cpp).
  let extent = vec3f(dims) / scale;
  // Half a micro voxel of skin, grown symmetrically about the box centre.
  // Without it a ray grazing a face can rasterize a fragment whose slab entry
  // lands an epsilon OUTSIDE the brick and discards, leaving a one-pixel crack
  // along every silhouette edge. The march itself is unaffected: the slab test
  // uses the true 0..dims box, so the skin only ever adds fragments that then
  // discard for real.
  let pad = 0.5 / scale;
  let local = boxCorner(vi) * (extent + pad * 2.0) - pad;

  let world = xf.pos + quatRotate(xf.quat, local);
  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.worldDir = world - R.camPos;
  // Ray origin in MICRO units: the eye, brought into object space and scaled.
  out.roM = quatRotateInv(xf.quat, R.camPos - xf.pos) * scale;
  out.quat = xf.quat;
  out.dims = dims;
  out.base = m.base;
  out.scale = scale;
  out.slot = slot;
  return out;
}

fn poolVoxAt(base : u32, dims : vec3<i32>, p : vec3<i32>) -> u32 {
  let idx = u32((p.z * dims.y + p.y) * dims.x + p.x);
  let w = base + (idx >> 2u);
  if (w >= MICRO_BODY_POOL_WORDS) { return 0u; }  // defensive
  return (pool[w] >> ((idx & 3u) * 8u)) & 0xFFu;
}

struct FSOut {
  @location(0) color : vec4f,
  @builtin(frag_depth) depth : f32,
};

@fragment
fn fs(in : VSOut) -> FSOut {
  // Everything instance-uniform arrives as a flat interpolant (see VSOut), so
  // this shader touches no storage buffer until the DDA's first brick fetch.
  let dims = in.dims;
  let scale = in.scale;
  let roM = in.roM;

  // ---- world ray -> object space ----
  // `rd` is NOT normalized: it is the camera-to-fragment vector, so any `t`
  // along it is in the same units the depth formula expects. Rotating it by the
  // conjugate quaternion (no scaling anywhere) preserves that, which is the
  // whole reason to avoid normalizing here.
  //
  // Working the slab test and the DDA in MICRO units makes them integer-indexed
  // and identical in shape to the world DDA; `t` is a fraction of rdWorld
  // either way, because roM and rdM are scaled by the SAME factor.
  let rdWorld = in.worldDir;
  let rdM = quatRotateInv(in.quat, rdWorld) * scale;
  let boxHi = vec3f(dims);
  // Clamp the direction's magnitude away from zero, keeping its sign, so an
  // axis-aligned ray yields a huge-but-finite tDelta instead of a NaN.
  let inv = 1.0 / select(rdM, sign(rdM + 1e-30) * 1e-9, abs(rdM) < vec3f(1e-9));
  let t0 = (vec3f(0.0) - roM) * inv;
  let t1 = (boxHi - roM) * inv;
  let tsmall = min(t0, t1);
  let tbig = max(t0, t1);
  // The camera may be INSIDE the box (that is exactly why we draw backfaces),
  // in which case tEnter is negative and the march starts at the eye.
  let tEnter = max(max(tsmall.x, tsmall.y), max(tsmall.z, 0.0));
  let tExit = min(tbig.x, min(tbig.y, tbig.z));
  if (tExit <= tEnter) { discard; }

  // ---- Amanatides-Woo over the brick ----
  // Nudge past the entry face before flooring: a ray entering at exactly x = 0
  // otherwise floors to -1 or 0 depending on float noise, and the silhouette
  // loses its first row of voxels.
  var p = roM + rdM * (tEnter + 1e-4);
  var c = vec3<i32>(floor(p));
  c = clamp(c, vec3<i32>(0), dims - vec3<i32>(1));
  let stepv = vec3<i32>(sign(rdM));
  let tDelta = abs(inv);
  var tMax = (vec3f(c) + select(vec3f(0.0), vec3f(1.0), rdM > vec3f(0.0)) - roM) * inv;

  // Seed `axis` with the ENTRY face, not with 0. If the very first cell the ray
  // lands in is solid — which is the common case for a limb whose surface is
  // its bounding box, e.g. a leg — the loop never steps, and a hardcoded 0
  // would light every such fragment as if it faced ±x. (When the camera is
  // inside the box tEnter is 0 and no slab bound matches; the fallback of 1
  // gives an up-facing normal, which is the least wrong choice for a camera
  // buried inside a creature and is never seen from outside.)
  var axis = 1;
  if (tEnter > 0.0) {
    if (tsmall.x >= tsmall.y && tsmall.x >= tsmall.z) { axis = 0; }
    else if (tsmall.y >= tsmall.z) { axis = 1; }
    else { axis = 2; }
  }
  var tCur = tEnter;
  var hitMat = 0u;
  // HARD CAP (rule 2: bound every emergent process). 3*maxDim covers a full
  // diagonal traverse of the brick; +4 is slack for the entry rounding above.
  // No data-dependent loop bound anywhere in this shader.
  let maxDim = max(dims.x, max(dims.y, dims.z));
  let maxSteps = 3 * maxDim + 4;
  for (var i = 0; i < maxSteps; i++) {
    if (c.x < 0 || c.y < 0 || c.z < 0 ||
        c.x >= dims.x || c.y >= dims.y || c.z >= dims.z) { break; }
    let v = poolVoxAt(in.base, dims, c);
    if (v != 0u) {
      hitMat = v;
      break;
    }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      c.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      c.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      c.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    if (tCur > tExit) { break; }
  }
  if (hitMat == 0u) { discard; }

  // ---- shading ----
  // Object-space face normal from the last-stepped axis, back to world space.
  var nLocal = vec3f(0.0);
  nLocal[axis] = -f32(stepv[axis]);
  let n = quatRotate(in.quat, nLocal);

  let mat = materials[hitMat];
  // Palette variant keyed on the micro CELL, not on the instance: a limb's
  // texture must not crawl when it rotates, and it must be identical on every
  // machine (this is render-only, but a replay should still look the same).
  let albedo = paletteColor(mat, u32(c.x * 7 + c.y * 13 + c.z * 29));

  // `tCur` is already the parameter along the UNNORMALIZED camera-to-fragment
  // vector, and that is the whole point of never normalizing anything: `ro/rd`
  // are `R.camPos - xf.pos` and `rdWorld` rotated by the conjugate (a rigid
  // rotation preserves magnitude), and `roM/rdM` are both scaled by the SAME
  // factor, so the micro-unit parametrization and the world one share `t`. The
  // hit is therefore just camPos + rdWorld * t, with no conversion.
  let worldPos = R.camPos + rdWorld * tCur;

  // emissive body voxels (embers) flicker exactly like their grid counterparts
  // and like the cube path's — one shared definition, in common.wgsl
  let fh = pcg(u32(c.x * 2917 + c.y * 131 + c.z * 7919) + in.slot * 977u);
  let emis = emberFlicker(f32(mat.emission) / 255.0, fh, R.time);
  let col = litColor(albedo, n, worldPos, emis, R);

  // ---- reversed-Z depth, EXACTLY raymarch.wgsl's convention ----
  // dot(rdWorld, camFwd) is the fragment's view-space Z at t = 1, so scaling it
  // by t gives the hit's view Z in world voxels — the same `t * dot(rd, camFwd)`
  // the raymarcher writes, just with an unnormalized rd on both sides. Any
  // deviation here (a normalized direction, a different near constant) shows up
  // as micro bodies punching through terrain or sinking into it.
  let viewZ = tCur * dot(rdWorld, R.camFwd);
  var out : FSOut;
  // litColor is linear HDR; same tonemap as terrain + the cube path, or a
  // live limb and the severed one beside it would shade differently.
  out.color = vec4f(tonemapHdr(col), 1.0);
  out.depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  return out;
}
